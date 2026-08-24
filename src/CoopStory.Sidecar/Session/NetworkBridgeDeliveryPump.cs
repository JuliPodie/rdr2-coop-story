using System.Buffers.Binary;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

internal enum NetworkBridgeEnqueueDisposition
{
    Queued,
    Coalesced,
    Rejected
}

internal readonly record struct NetworkBridgeEnqueueResult(
    NetworkBridgeEnqueueDisposition Disposition,
    int Backlog);

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
/// Decouples authenticated LAN receive loops from a potentially blocked game
/// pipe. Replaceable state occupies one latest-only slot per message type while
/// commands and lifecycle events retain FIFO ordering in a bounded queue.
/// </summary>
internal sealed class NetworkBridgeDeliveryPump
{
    private sealed record QueuedDelivery(
        ProtocolEnvelope Envelope,
        Func<bool>? IsValid,
        Action<ProtocolEnvelope>? AfterDelivered,
        long EnqueueOrder)
    {
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

        public void NotifyDelivered()
        {
            try
            {
                AfterDelivered?.Invoke(Envelope);
            }
            catch
            {
                // Delivery accounting and the barrier must always complete.
                // A best-effort observer may withhold its derived readiness
                // lease, but it cannot terminate the pump worker.
            }
        }
    }

    private sealed class DeliveryBarrierLease : IAsyncDisposable
    {
        private NetworkBridgeDeliveryPump? _owner;

        public DeliveryBarrierLease(NetworkBridgeDeliveryPump owner)
        {
            _owner = owner;
        }

        public ValueTask DisposeAsync()
        {
            Interlocked.Exchange(ref _owner, null)?.ExitDeliveryBarrier();
            return ValueTask.CompletedTask;
        }
    }

    private readonly object _sync = new();
    private readonly Queue<QueuedDelivery> _criticalQueue = new();
    private readonly Dictionary<MessageType, QueuedDelivery> _coalesced = [];
    private readonly Dictionary<NetEntityId, QueuedDelivery> _entityUpdates = [];
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

    public NetworkBridgeEnqueueResult TryEnqueue(
        ProtocolEnvelope envelope,
        Func<bool>? isValid = null,
        Action<ProtocolEnvelope>? afterDelivered = null)
    {
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
            if (enqueueOrder == 0)
            {
                enqueueOrder = unchecked(++_nextEnqueueOrder);
            }
            var queued = new QueuedDelivery(
                frozen,
                isValid,
                afterDelivered,
                enqueueOrder);

            if (IsOrderedReliableCinematicType(frozen.Type))
            {
                // The cinematic FSM, definition revisions and 2PC controls
                // share one reliable receive order. A Definition may not jump
                // ahead of the FSM generation that authorizes it.
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
                var replaced = _coalesced.TryGetValue(
                    frozen.Type,
                    out var pendingState);
                var pendingStateValid =
                    replaced && pendingState!.IsStillValid();
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
                    if (!queued.IsStillValid())
                    {
                        CompleteDelivery(
                            delivered: false,
                            invalidated: true);
                        continue;
                    }

                    var delivered = false;
                    try
                    {
                        // A protocol frame is never cancelled half-way through
                        // a pipe write. Runtime shutdown closes the connection
                        // to release a blocked write and discards that stream.
                        delivered = await _deliverAsync(queued.Envelope)
                            .ConfigureAwait(false);
                        if (delivered)
                        {
                            // Publish causal readiness while the delivery is
                            // still in-flight. A reset barrier therefore cannot
                            // rotate the logical pipe token between the full
                            // frame write and this callback.
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

    public void StopAccepting()
    {
        lock (_sync)
        {
            _accepting = false;
        }

        SignalWorker();
    }

    public void ClearPending()
    {
        lock (_sync)
        {
            ClearPendingLocked();
        }
    }

    /// <summary>
    /// Stops new pipe deliveries, discards every queued pre-reset frame and
    /// waits for an already active full-frame write to finish. Frames arriving
    /// while the barrier is held remain queued and resume only after the owner
    /// has delivered the reset directly to the bridge.
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

    private bool TryTakeNext(out QueuedDelivery queued)
    {
        lock (_sync)
        {
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

    private void ExitDeliveryBarrier()
    {
        lock (_sync)
        {
            _deliveryPaused = false;
        }
        _deliveryBarrierGate.Release();
        SignalWorker();
    }

    private void ClearPendingLocked()
    {
        _criticalQueue.Clear();
        _coalesced.Clear();
        _entityUpdates.Clear();
    }

    private int GetBacklogLocked() =>
        _criticalQueue.Count +
        _coalesced.Count +
        _entityUpdates.Count +
        (_inFlight ? 1 : 0);

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

    private static bool IsOrderedReliableCinematicType(MessageType type) =>
        type is MessageType.MissionState or
            MessageType.MissionCinematicState or
            MessageType.AnimSceneDefinition or
            MessageType.AnimSceneControl;

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
