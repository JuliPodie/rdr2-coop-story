using System.Buffers.Binary;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

// Says whether a message was queued, replaced an old update, or was dropped because the queue is full/stopping.
internal enum NetworkBridgeEnqueueDisposition
{
    // The message received a new waiting place.
    Queued,
    // A waiting place already existed for this type or NPC, so the newest usable state is kept there.
    Coalesced,
    // The message could not be queued, for example because its queue was full or shutting down.
    Rejected
}

// Tell the caller what happened and how much work remains, including an active delivery.
internal readonly record struct NetworkBridgeEnqueueResult(
    NetworkBridgeEnqueueDisposition Disposition,
    int Backlog);

// A report for diagnostics about queue activity and the message currently being written to the game.
// Delivered counts successful local writes, rather than proof that an NPC has moved on screen.
internal readonly record struct NetworkBridgePumpSnapshot(
    long Queued,
    long Coalesced,
    long Rejected,
    long Dequeued,
    long Delivered,
    long Unavailable,
    long Invalidated,
    int Backlog,
    int MaxBacklog,
    MessageType? ActiveType,
    long ActiveMilliseconds);

/// <summary>
/// Holds messages from the other computer while the local Bridge is busy receiving earlier ones.
/// The network can keep receiving without waiting for RDR2 to finish every pipe write.
/// Frequent state updates keep one waiting entry per type, while NPC updates keep one per entity ID.
/// Events such as creating and removing an NPC keep their arrival order in a queue with a size limit.
/// </summary>
internal sealed class NetworkBridgeDeliveryPump
{
    // Keep the message with a final validity check and an optional callback for successful delivery.
    // Those callbacks let SidecarRuntime prevent an old session's message reaching a newly connected game.
    private sealed record QueuedDelivery(
        ProtocolEnvelope Envelope,
        Func<bool>? IsValid,
        Action<ProtocolEnvelope>? AfterDelivered,
        long EnqueueOrder)
    {
        private readonly long _queuedAtMs = Environment.TickCount64;

        public ProtocolEnvelope? PrepareEnvelope()
        {
            if (Envelope.Type != MessageType.NpcAnimation) return Envelope;
            try
            {
                var payload = NpcAnimationCodec.Age(
                    NpcAnimationCodec.Decode(Envelope.Payload.Span),
                    Environment.TickCount64 - _queuedAtMs);
                return payload is { } live
                    ? Envelope with { Payload = NpcAnimationCodec.Encode(live) } : null;
            }
            catch (ProtocolException) { return null; }
        }

        // Recheck the connection/session at delivery time because it may have changed while we waited.
        public bool IsStillValid()
        {
            try
            {
                return IsValid?.Invoke() ?? true;
            }
            catch
            {
                return false;
            }
        }

        // Tell the owner that the local write completed so it can update readiness and diagnostics.
        public void NotifyDelivered()
        {
            try
            {
                AfterDelivered?.Invoke(Envelope);
            }
            catch
            {
                // Delivery accounting and the barrier must always complete.
                // A best-effort observer may withhold its derived readiness lease, but it cannot terminate the pump worker.
            }
        }
    }

    // Owning this object means normal deliveries are temporarily paused for a reset.
    // Disposing it resumes delivery, including when an await using block exits after an error.
    private sealed class DeliveryBarrierLease : IAsyncDisposable
    {
        private NetworkBridgeDeliveryPump? _owner;

        public DeliveryBarrierLease(NetworkBridgeDeliveryPump owner)
        {
            _owner = owner;
        }

        public ValueTask DisposeAsync()
        {
            // Clear the owner atomically so disposing twice cannot resume or release the same pause twice.
            Interlocked.Exchange(ref _owner, null)?.ExitDeliveryBarrier();
            return ValueTask.CompletedTask;
        }
    }

    // Network callbacks and the delivery worker can run at the same time.
    // This lock protects the shared queues and counters while either side changes them.
    private readonly object _sync = new();
    // Important messages stay in order.
    // Fast updates keep only the newest one so a slow game pipe cannot build an endless queue.
    private readonly Queue<QueuedDelivery> _criticalQueue = new();
    private readonly Dictionary<MessageType, QueuedDelivery> _coalesced = [];
    private readonly Dictionary<NetEntityId, QueuedDelivery> _entityUpdates = [];
    // Wake the worker when there is work, and allow only one reset owner to pause delivery at a time.
    private readonly SemaphoreSlim _signal = new(0, 1);
    private readonly SemaphoreSlim _deliveryBarrierGate = new(1, 1);
    private readonly Func<ProtocolEnvelope, ValueTask<bool>> _deliverAsync;
    private readonly int _criticalCapacity;
    private readonly int _entityUpdateCapacity;
    private long _nextEnqueueOrder;
    private bool _accepting = true;
    private bool _deliveryPaused;
    private bool _inFlight;
    private TaskCompletionSource<bool>? _idleSignal;
    private MessageType? _activeType;
    private long _activeSinceTimestamp;
    private long _queued;
    private long _coalescedCount;
    private long _rejected;
    private long _dequeued;
    private long _delivered;
    private long _unavailable;
    private long _invalidated;
    private int _maxBacklog;
    private int _runStarted;

    // The caller supplies the function that actually writes an envelope to the local Bridge.
    // Separate queue limits prevent a burst of different NPCs or critical events from growing memory forever.
    public NetworkBridgeDeliveryPump(
        Func<ProtocolEnvelope, ValueTask<bool>> deliverAsync,
        int criticalCapacity = 128,
        int entityUpdateCapacity = 64)
    {
        _deliverAsync =
            deliverAsync ?? throw new ArgumentNullException(nameof(deliverAsync));
        if (criticalCapacity <= 0)
        {
            throw new ArgumentOutOfRangeException(
                nameof(criticalCapacity),
                criticalCapacity,
                "Critical queue capacity must be positive.");
        }

        _criticalCapacity = criticalCapacity;
        if (entityUpdateCapacity <= 0)
        {
            throw new ArgumentOutOfRangeException(
                nameof(entityUpdateCapacity),
                entityUpdateCapacity,
                "Entity-update capacity must be positive.");
        }

        _entityUpdateCapacity = entityUpdateCapacity;
    }

    // Add an incoming message to the appropriate waiting area without waiting for the game to read it.
    // Coalescing means several updates share one entry that keeps the newest usable value.
    public NetworkBridgeEnqueueResult TryEnqueue(
        ProtocolEnvelope envelope,
        Func<bool>? isValid = null,
        Action<ProtocolEnvelope>? afterDelivered = null)
    {
        // Make our own copy because the network code may reuse its byte buffer.
        var frozen = Freeze(envelope);
        NetworkBridgeEnqueueDisposition disposition;
        int backlog;
        var signal = false;

        lock (_sync)
        {
            if (!_accepting)
            {
                _rejected++;
                return new NetworkBridgeEnqueueResult(
                    NetworkBridgeEnqueueDisposition.Rejected,
                    GetBacklogLocked());
            }

            var enqueueOrder = unchecked(++_nextEnqueueOrder);
            // This is local queue order, separate from the sender's network sequence number.
            // Replacing a waiting state preserves its existing turn so a busy type does not wait forever.
            if (enqueueOrder == 0)
            {
                enqueueOrder = unchecked(++_nextEnqueueOrder);
            }
            var queued = new QueuedDelivery(
                frozen,
                isValid,
                afterDelivered,
                enqueueOrder);

            // Cutscene messages must stay in order.
            // A new cutscene setup cannot jump in front of the state that says it is allowed.
            if (IsOrderedReliableCinematicType(frozen.Type))
            {
                // Keep mission state, cutscene phase, scene definitions, and readiness/play controls in their received order.
                // A scene definition must not overtake the cutscene state that says which scene generation is current.
                if (_criticalQueue.Count >= _criticalCapacity)
                {
                    _rejected++;
                    return new NetworkBridgeEnqueueResult(
                        NetworkBridgeEnqueueDisposition.Rejected,
                        GetBacklogLocked());
                }

                _criticalQueue.Enqueue(queued);
                disposition = NetworkBridgeEnqueueDisposition.Queued;
            }
            else if (frozen.Type == MessageType.EntityUpdate)
            {
                if (!TryReadEntityUpdateId(frozen, out var entityId))
                {
                    _rejected++;
                    return new NetworkBridgeEnqueueResult(
                        NetworkBridgeEnqueueDisposition.Rejected,
                        GetBacklogLocked());
                }

                // Keep only the newest update for each NPC/object.
                // Make/remove messages still stay in order.
                var replaced = _entityUpdates.TryGetValue(
                    entityId,
                    out var pendingEntityUpdate);
                if (!replaced && _entityUpdates.Count >= _entityUpdateCapacity)
                {
                    _rejected++;
                    return new NetworkBridgeEnqueueResult(
                        NetworkBridgeEnqueueDisposition.Rejected,
                        GetBacklogLocked());
                }

                var pendingEntityUpdateValid =
                    replaced && pendingEntityUpdate!.IsStillValid();
                // Arrival order is not necessarily sender order because UDP messages can arrive late.
                // Keep the existing update if it is still valid and the arriving sequence is not newer.
                if (!replaced ||
                    !pendingEntityUpdateValid ||
                    SequenceNumber.IsNewer(
                        frozen.Sequence,
                        pendingEntityUpdate!.Envelope.Sequence))
                {
                    _entityUpdates[entityId] = pendingEntityUpdateValid
                        ? queued with
                        {
                            EnqueueOrder = pendingEntityUpdate!.EnqueueOrder
                        }
                        : queued;
                }
                disposition = replaced
                    ? NetworkBridgeEnqueueDisposition.Coalesced
                    : NetworkBridgeEnqueueDisposition.Queued;
                if (replaced)
                {
                    _coalescedCount++;
                }
            }
            else if (IsCoalescedType(frozen.Type))
            {
                // For player/camera/animation updates, old data is not useful.
                // Send the newest view instead of a long backlog.
                var replaced = _coalesced.TryGetValue(
                    frozen.Type,
                    out var pendingState);
                var pendingStateValid =
                    replaced && pendingState!.IsStillValid();
                // As with NPC updates, a delayed older sample must not replace a newer pending sample.
                if (!replaced ||
                    !pendingStateValid ||
                    SequenceNumber.IsNewer(
                        frozen.Sequence,
                        pendingState!.Envelope.Sequence))
                {
                    _coalesced[frozen.Type] = pendingStateValid
                        ? queued with
                        {
                            EnqueueOrder = pendingState!.EnqueueOrder
                        }
                        : queued;
                }
                disposition = replaced
                    ? NetworkBridgeEnqueueDisposition.Coalesced
                    : NetworkBridgeEnqueueDisposition.Queued;
                if (replaced)
                {
                    _coalescedCount++;
                }
            }
            else
            {
                if (_criticalQueue.Count >= _criticalCapacity)
                {
                    _rejected++;
                    return new NetworkBridgeEnqueueResult(
                        NetworkBridgeEnqueueDisposition.Rejected,
                        GetBacklogLocked());
                }

                _criticalQueue.Enqueue(queued);
                disposition = NetworkBridgeEnqueueDisposition.Queued;
            }

            _queued++;
            backlog = GetBacklogLocked();
            _maxBacklog = Math.Max(_maxBacklog, backlog);
            signal = true;
        }

        if (signal)
        {
            SignalWorker();
        }

        return new NetworkBridgeEnqueueResult(disposition, backlog);
    }

    // Start the single worker that removes waiting messages and writes them to the Bridge one at a time.
    // For example, NPC B can keep receiving network updates while NPC A's local write is still waiting.
    public async Task RunAsync(CancellationToken cancellationToken)
    {
        if (Interlocked.Exchange(ref _runStarted, 1) != 0)
        {
            throw new InvalidOperationException(
                "Network-to-bridge delivery pump can only be run once.");
        }

        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                await _signal.WaitAsync(cancellationToken).ConfigureAwait(false);
                while (!cancellationToken.IsCancellationRequested &&
                       TryTakeNext(out var queued))
                {
                    // The game may reconnect after this was queued.
                    // Do not send it to the new game connection if it belongs to the old one.
                    var preparedEnvelope = queued.PrepareEnvelope();
                    if (!queued.IsStillValid() || preparedEnvelope is null)
                    {
                        CompleteDelivery(
                            delivered: false,
                            invalidated: true);
                        continue;
                    }

                    var delivered = false;
                    try
                    {
                        // A protocol frame is never cancelled half-way through a pipe write.
                        // Runtime shutdown closes the connection to release a blocked write and discards that stream.
                        delivered = await _deliverAsync(preparedEnvelope)
                            .ConfigureAwait(false);
                        if (delivered)
                        {
                            // Publish causal readiness while the delivery is still in-flight.
                            // A reset barrier therefore cannot rotate the logical pipe token between the full frame write and this callback.
                            queued.NotifyDelivered();
                        }
                    }
                    finally
                    {
                        CompleteDelivery(delivered, invalidated: false);
                    }
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        finally
        {
            StopAccepting();
        }
    }

    // Refuse further enqueues and wake the worker so shutdown can continue.
    // The cancellation token controls the worker's lifetime separately.
    public void StopAccepting()
    {
        lock (_sync)
        {
            _accepting = false;
        }

        SignalWorker();
    }

    // Remove queued messages that should not survive a resync.
    // This does not stop an already-started write; the delivery barrier handles that case.
    public void ClearPending()
    {
        lock (_sync)
        {
            ClearPendingLocked();
        }
    }

    /// <summary>
    /// Pause ordinary delivery while the caller sends a reset directly to the Bridge.
    /// Clear the existing queue and wait for any current write to finish so old bytes cannot follow the reset.
    /// New arrivals wait in the queue until the returned object is disposed.
    /// </summary>
    public async ValueTask<IAsyncDisposable> EnterDeliveryBarrierAsync(
        CancellationToken cancellationToken = default)
    {
        await _deliveryBarrierGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);

        Task waitForIdle;
        lock (_sync)
        {
            _deliveryPaused = true;
            ClearPendingLocked();
            if (_inFlight)
            {
                _idleSignal ??= new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                waitForIdle = _idleSignal.Task;
            }
            else
            {
                waitForIdle = Task.CompletedTask;
            }
        }

        try
        {
            await waitForIdle.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            return new DeliveryBarrierLease(this);
        }
        catch
        {
            ExitDeliveryBarrier();
            throw;
        }
    }

    // Copy the counters while holding the lock so the log sees one consistent view of queue activity.
    // ActiveMilliseconds helps detect a write that has been stuck waiting for the game.
    public NetworkBridgePumpSnapshot ReadSnapshot()
    {
        lock (_sync)
        {
            var activeMilliseconds = _inFlight
                ? Math.Max(0, Environment.TickCount64 - _activeSinceTimestamp)
                : 0;
            return new NetworkBridgePumpSnapshot(
                _queued,
                _coalescedCount,
                _rejected,
                _dequeued,
                _delivered,
                _unavailable,
                _invalidated,
                GetBacklogLocked(),
                _maxBacklog,
                _activeType,
                activeMilliseconds);
        }
    }

    // Choose the oldest waiting place across the critical, per-type, and per-NPC collections.
    // Mark it active before releasing the lock so a simultaneous reset knows a write is in progress.
    private bool TryTakeNext(out QueuedDelivery queued)
    {
        lock (_sync)
        {
            // Pause normal messages while the reset message is sent directly.
            if (_deliveryPaused ||
                _criticalQueue.Count == 0 &&
                _coalesced.Count == 0 &&
                _entityUpdates.Count == 0)
            {
                queued = default!;
                return false;
            }

            QueuedDelivery? oldest = null;
            var selectedLane = -1;
            var selectedCoalescedType = default(MessageType);
            var selectedEntityId = NetEntityId.None;

            // Pick the oldest message we kept so replacing old updates does not mess up the important message order.
            if (_criticalQueue.TryPeek(out var critical))
            {
                oldest = critical;
                selectedLane = 0;
            }
            foreach (var pair in _coalesced)
            {
                if (oldest is null ||
                    pair.Value.EnqueueOrder < oldest.EnqueueOrder)
                {
                    oldest = pair.Value;
                    selectedLane = 1;
                    selectedCoalescedType = pair.Key;
                }
            }
            foreach (var pair in _entityUpdates)
            {
                if (oldest is null ||
                    pair.Value.EnqueueOrder < oldest.EnqueueOrder)
                {
                    oldest = pair.Value;
                    selectedLane = 2;
                    selectedEntityId = pair.Key;
                }
            }

            if (oldest is null)
            {
                queued = default!;
                return false;
            }

            queued = oldest;
            switch (selectedLane)
            {
                case 0:
                    _ = _criticalQueue.Dequeue();
                    break;
                case 1:
                    _ = _coalesced.Remove(selectedCoalescedType);
                    break;
                case 2:
                    _ = _entityUpdates.Remove(selectedEntityId);
                    break;
                default:
                    throw new InvalidOperationException(
                        "Delivery pump selected an invalid lane.");
            }

            _dequeued++;
            _inFlight = true;
            _activeType = queued.Envelope.Type;
            _activeSinceTimestamp = Environment.TickCount64;
            _maxBacklog = Math.Max(_maxBacklog, GetBacklogLocked());
            return true;
        }
    }

    // Record whether the write succeeded, failed, or was skipped because the session changed.
    // Wake a reset operation that was waiting for this active delivery to finish.
    private void CompleteDelivery(bool delivered, bool invalidated)
    {
        lock (_sync)
        {
            if (invalidated)
            {
                _invalidated++;
            }
            else if (delivered)
            {
                _delivered++;
            }
            else
            {
                _unavailable++;
            }

            _inFlight = false;
            _activeType = null;
            _activeSinceTimestamp = 0;
            _idleSignal?.TrySetResult(true);
            _idleSignal = null;
        }
    }

    // Resume normal queue delivery after the caller has finished its direct reset work.
    private void ExitDeliveryBarrier()
    {
        lock (_sync)
        {
            _deliveryPaused = false;
        }
        _deliveryBarrierGate.Release();
        SignalWorker();
    }

    // Empty all three waiting areas while the caller holds the queue lock.
    // The active write is tracked separately and is not removed by clearing these collections.
    private void ClearPendingLocked()
    {
        _criticalQueue.Clear();
        _coalesced.Clear();
        _entityUpdates.Clear();
    }

    // Include a write already underway as well as messages still waiting in the collections.
    private int GetBacklogLocked() =>
        _criticalQueue.Count +
        _coalesced.Count +
        _entityUpdates.Count +
        (_inFlight ? 1 : 0);

    // Give the worker one wake-up notice even if many packets arrived while it was busy.
    // The worker drains available work after waking, so extra notices are unnecessary.
    private void SignalWorker()
    {
        try
        {
            _signal.Release();
        }
        catch (SemaphoreFullException)
        {
            // A wake-up is already pending.
        }
    }

    // These messages describe a current view, so retaining old versions has no value once a newer version is ready for the same local bridge.
    private static bool IsCoalescedType(MessageType type) =>
        type is MessageType.PlayerState or
            MessageType.PlayerAnimationState or
            MessageType.MissionCameraState or
            MessageType.AnimSceneReplicaState or
            MessageType.WorldState or
            MessageType.EquipmentState or
            MessageType.PlayerIdentity or
            MessageType.PlayerAppearanceState or
            MessageType.PlayerMountState;

    // Keep every step of mission/cutscene setup because later steps depend on earlier ones being delivered.
    private static bool IsOrderedReliableCinematicType(MessageType type) =>
        type is MessageType.MissionState or
            MessageType.MissionCinematicState or
            MessageType.AnimSceneDefinition or
            MessageType.AnimSceneControl;

    // Read just the leading network ID to choose this NPC's waiting entry.
    // This is a queue lookup check, not a replacement for the full entity payload validation elsewhere.
    private static bool TryReadEntityUpdateId(
        ProtocolEnvelope envelope,
        out NetEntityId entityId)
    {
        if (envelope.Payload.Length < sizeof(ulong))
        {
            entityId = NetEntityId.None;
            return false;
        }

        entityId = new NetEntityId(
            BinaryPrimitives.ReadUInt64LittleEndian(envelope.Payload.Span));
        return entityId.IsValid;
    }

    // Own a separate payload array for as long as this message waits in the queue.
    // ReadOnlyMemory prevents writes through that view, but the original array could still be reused by its owner.
    private static ProtocolEnvelope Freeze(ProtocolEnvelope envelope) =>
        new(
            envelope.Type,
            envelope.Sequence,
            envelope.Tick,
            envelope.Payload.ToArray())
        {
            Version = envelope.Version
        };
}
