using System.Collections.Concurrent;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

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

        _interpolationDelayMs = interpolationDelayMs;
    }

    public int Count => _entities.Count;

    public bool ApplyPlayerState(ProtocolEnvelope envelope)
    {
        if (envelope.Type != MessageType.PlayerState)
        {
            throw new ArgumentException("Envelope is not a PlayerState.", nameof(envelope));
        }

        var state = BinaryPayloadCodec.DecodePlayerState(envelope.Payload.Span);
        var entry = _entities.GetOrAdd(state.EntityId, static _ => new EntityEntry());
        return entry.TryAdd(envelope.Sequence, envelope.Tick, state);
    }

    public bool Remove(NetEntityId entityId) =>
        _entities.TryRemove(entityId, out _);

    public void Clear() => _entities.Clear();

    public bool TrySample(
        NetEntityId entityId,
        ulong remoteNowTick,
        out PlayerStatePayload state)
    {
        if (_entities.TryGetValue(entityId, out var entry))
        {
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

public readonly record struct ReplicatedPlayerSnapshot(
    PlayerStatePayload State,
    ulong SenderTick,
    long ReceivedAtMilliseconds)
{
    public long AgeMilliseconds => Math.Max(
        0,
        Environment.TickCount64 - ReceivedAtMilliseconds);
}
