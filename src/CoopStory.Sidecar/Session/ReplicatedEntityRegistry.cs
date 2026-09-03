using System.Collections.Concurrent;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

// Stores remote-player snapshot histories for the sidecar.
// The native bridge owns actual RDR2 handles; this class knows only stable protocol entity IDs.
public sealed class ReplicatedEntityRegistry
{
    private readonly ConcurrentDictionary<NetEntityId, EntityEntry> _entities = new();
    private readonly int _interpolationDelayMs;

    public ReplicatedEntityRegistry(int interpolationDelayMs)
    {
        if (interpolationDelayMs < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(interpolationDelayMs));
        }

        // Sampling slightly in the past gives packet jitter time to arrive and makes remote movement smoother than showing each packet immediately.
        _interpolationDelayMs = interpolationDelayMs;
    }

    public int Count => _entities.Count;

    public bool ApplyPlayerState(ProtocolEnvelope envelope)
    {
        if (envelope.Type != MessageType.PlayerState)
        {
            throw new ArgumentException("Envelope is not a PlayerState.", nameof(envelope));
        }

        // Decode only a PlayerState and put it into that entity's ordered history; stale/replayed sequence numbers are rejected by the buffer.
        var state = BinaryPayloadCodec.DecodePlayerState(envelope.Payload.Span);
        var entry = _entities.GetOrAdd(state.EntityId, static _ => new EntityEntry());
        return entry.TryAdd(envelope.Sequence, envelope.Tick, state);
    }

    // Remove an entity's history when its lifecycle despawn arrives, preventing an old sample from moving a later replica with the same local handle.
    public bool Remove(NetEntityId entityId) =>
        _entities.TryRemove(entityId, out _);

    // A full reconnect resync intentionally discards all guest-local history.
    public void Clear() => _entities.Clear();

    public bool TrySample(
        NetEntityId entityId,
        ulong remoteNowTick,
        out PlayerStatePayload state)
    {
        if (_entities.TryGetValue(entityId, out var entry))
        {
            // Convert current host time into a delayed display sample.
            return entry.TrySample(remoteNowTick, _interpolationDelayMs, out state);
        }

        state = default;
        return false;
    }

    public bool TryGetLatest(
        NetEntityId entityId,
        out ReplicatedPlayerSnapshot snapshot)
    {
        if (_entities.TryGetValue(entityId, out var entry))
        {
            return entry.TryGetLatest(out snapshot);
        }

        snapshot = default;
        return false;
    }

    private sealed class EntityEntry
    {
        private readonly object _sync = new();
        // This lock keeps the sequence history and separately exposed latest metadata consistent while UDP receive and session logic run together.
        private readonly PlayerStateInterpolationBuffer _buffer = new();
        private PlayerStatePayload _latest;
        private ulong _latestSenderTick;
        private long _latestReceivedAtMs;
        private bool _hasLatest;

        public bool TryAdd(
            uint sequence,
            ulong tick,
            PlayerStatePayload state)
        {
            lock (_sync)
            {
                if (!_buffer.TryAdd(sequence, tick, state))
                {
                    return false;
                }
                _latest = state;
                _latestSenderTick = tick;
                // Local receipt time lets watchdogs report age even though the sender's tick belongs to the remote machine's timebase.
                _latestReceivedAtMs = Environment.TickCount64;
                _hasLatest = true;
                return true;
            }
        }

        public bool TryGetLatest(out ReplicatedPlayerSnapshot snapshot)
        {
            lock (_sync)
            {
                if (!_hasLatest)
                {
                    snapshot = default;
                    return false;
                }
                snapshot = new ReplicatedPlayerSnapshot(
                    _latest,
                    _latestSenderTick,
                    _latestReceivedAtMs);
                return true;
            }
        }

        public bool TrySample(
            ulong tick,
            int delayMs,
            out PlayerStatePayload state)
        {
            lock (_sync)
            {
                return _buffer.TrySample(tick, delayMs, out state);
            }
        }
    }
}

// A diagnostic/latest-state view; motion rendering normally samples the whole interpolation buffer instead of directly applying this newest packet.
public readonly record struct ReplicatedPlayerSnapshot(
    PlayerStatePayload State,
    ulong SenderTick,
    long ReceivedAtMilliseconds)
{
    public long AgeMilliseconds => Math.Max(
        0,
        Environment.TickCount64 - ReceivedAtMilliseconds);
}
