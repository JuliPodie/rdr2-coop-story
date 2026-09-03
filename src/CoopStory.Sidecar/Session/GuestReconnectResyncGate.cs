using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

// Tells the caller whether a reconnect reset was remembered or was already waiting; duplicate resets should never rebuild the local world twice.
internal enum GuestReconnectResyncDeferDisposition
{
    Stored,
    Duplicate
}

internal enum GuestReconnectResyncReplayDisposition
{
    Completed,
    NoPendingRequest,
    Invalidated,
    BridgeDeliveryFailed,
    PeerRequestFailed
}

internal readonly record struct GuestReconnectResyncReplayResult(
    GuestReconnectResyncReplayDisposition Disposition,
    bool BridgeDelivered,
    bool PeerRequested);

/// <summary>
/// Holds the guest-local reconnect reset while motion-mode negotiation gates normal network-to-bridge traffic.
/// Replay retains the validated request until both the local bridge delivery and the host replay request succeed.
/// </summary>
internal sealed class GuestReconnectResyncGate
{
    private sealed record PendingRequest(
        ProtocolEnvelope Envelope,
        long Generation,
        CancellationTokenSource Stop);

    private readonly object _sync = new();
    // Only one replay may clear the bridge and request the host snapshot at a time, even if reconnect and motion negotiation finish together.
    private readonly SemaphoreSlim _replayGate = new(1, 1);
    private PendingRequest? _pending;
    private PendingRequest? _active;
    private long _nextGeneration;

    public GuestReconnectResyncDeferDisposition Defer(
        ProtocolEnvelope envelope)
    {
        // Store an immutable copy because the original envelope may be backed by a receive buffer that is reused after this callback returns.
        Validate(envelope);
        lock (_sync)
        {
            if (_pending is not null)
            {
                return GuestReconnectResyncDeferDisposition.Duplicate;
            }

            // Generation/cancellation lets Clear invalidate a replay already part-way through an asynchronous bridge or host send.
            var generation = unchecked(++_nextGeneration);
            if (generation == 0)
            {
                generation = unchecked(++_nextGeneration);
            }
            _pending = new PendingRequest(
                Freeze(envelope),
                generation,
                new CancellationTokenSource());
            return GuestReconnectResyncDeferDisposition.Stored;
        }
    }

    public async ValueTask<GuestReconnectResyncReplayResult> ReplayOnceAsync(
        Action clearGuestState,
        Func<ProtocolEnvelope, CancellationToken, ValueTask<bool>>
            deliverToBridge,
        Func<CancellationToken, ValueTask<bool>> requestHostReplay,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(clearGuestState);
        ArgumentNullException.ThrowIfNull(deliverToBridge);
        ArgumentNullException.ThrowIfNull(requestHostReplay);
        cancellationToken.ThrowIfCancellationRequested();

        // The critical ordering is: clear stale guest state, tell the local bridge, then ask the host to send its authoritative snapshot.
        await _replayGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        PendingRequest? request = null;
        try
        {
            lock (_sync)
            {
                request = _pending;
                _active = request;
            }
            if (request is null)
            {
                return new GuestReconnectResyncReplayResult(
                    GuestReconnectResyncReplayDisposition.NoPendingRequest,
                    BridgeDelivered: false,
                    PeerRequested: false);
            }

            using var linked = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken,
                request.Stop.Token);
            // Purge all old cached proxy/mirror state before accepting the host replay; otherwise new and old entity identities could be mixed.
            clearGuestState();
            if (!await deliverToBridge(request.Envelope, linked.Token)
                    .ConfigureAwait(false))
            {
                return new GuestReconnectResyncReplayResult(
                    GuestReconnectResyncReplayDisposition
                        .BridgeDeliveryFailed,
                    BridgeDelivered: false,
                    PeerRequested: false);
            }

            // A new disconnect/reset can replace this request while its pipe delivery was pending, so it must not request an obsolete replay.
            if (!IsCurrent(request))
            {
                return new GuestReconnectResyncReplayResult(
                    GuestReconnectResyncReplayDisposition.Invalidated,
                    BridgeDelivered: true,
                    PeerRequested: false);
            }

            var peerRequested = await requestHostReplay(linked.Token)
                .ConfigureAwait(false);
            // Retain the request after a failed host send so a later retry repeats the complete safe reset rather than silently proceeding.
            if (peerRequested)
            {
                lock (_sync)
                {
                    if (ReferenceEquals(_pending, request))
                    {
                        _pending = null;
                    }
                }
            }

            return new GuestReconnectResyncReplayResult(
                peerRequested
                    ? GuestReconnectResyncReplayDisposition.Completed
                    : GuestReconnectResyncReplayDisposition.PeerRequestFailed,
                BridgeDelivered: true,
                peerRequested);
        }
        catch (OperationCanceledException) when (
            request?.Stop.IsCancellationRequested == true &&
            !cancellationToken.IsCancellationRequested)
        {
            return new GuestReconnectResyncReplayResult(
                GuestReconnectResyncReplayDisposition.Invalidated,
                BridgeDelivered: false,
                PeerRequested: false);
        }
        finally
        {
            var disposeRequest = false;
            lock (_sync)
            {
                if (ReferenceEquals(_active, request))
                {
                    _active = null;
                }
                disposeRequest = request is not null &&
                    !ReferenceEquals(_pending, request);
            }
            if (disposeRequest)
            {
                request!.Stop.Dispose();
            }
            _replayGate.Release();
        }
    }

    public void Clear()
    {
        // Cancel the pending or active work outside the lock to avoid running cancellation callbacks while other state is protected.
        PendingRequest? cleared;
        var dispose = false;
        lock (_sync)
        {
            cleared = _pending;
            _pending = null;
            dispose = cleared is not null &&
                !ReferenceEquals(_active, cleared);
        }

        if (cleared is not null)
        {
            cleared.Stop.Cancel();
            if (dispose)
            {
                cleared.Stop.Dispose();
            }
        }
    }

    public bool HasPendingRequest
    {
        get
        {
            lock (_sync)
            {
                return _pending is not null;
            }
        }
    }

    private bool IsCurrent(PendingRequest request)
    {
        lock (_sync)
        {
            return ReferenceEquals(_pending, request) &&
                _pending.Generation == request.Generation;
        }
    }

    private static void Validate(ProtocolEnvelope envelope)
    {
        ArgumentNullException.ThrowIfNull(envelope);
        // This gate is intentionally narrow: it defers only the empty resync command, never arbitrary network gameplay frames.
        if (envelope.Type != MessageType.ResyncRequest ||
            envelope.Version != ProtocolConstants.Version ||
            !envelope.Payload.IsEmpty)
        {
            throw new ProtocolException(
                "Deferred guest reconnect reset must be an empty protocol-v20 ResyncRequest.");
        }
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
