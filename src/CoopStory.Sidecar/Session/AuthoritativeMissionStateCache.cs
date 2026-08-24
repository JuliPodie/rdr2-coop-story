using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

internal enum MissionStateCacheDisposition
{
    Accepted,
    Refreshed,
    Stale
}

internal readonly record struct MissionStateCacheUpdate(
    MissionStateCacheDisposition Disposition,
    MissionStatePayload State);

/// <summary>
/// Keeps the newest validated host mission state available while the game pipe
/// is disconnected. Mission epoch/revision order wins over transport order;
/// equal revisions are accepted only as byte-equivalent heartbeats.
/// </summary>
internal sealed class AuthoritativeMissionStateCache
{
    private readonly object _sync = new();
    private ProtocolEnvelope? _envelope;
    private MissionStatePayload? _state;

    public MissionStateCacheUpdate Apply(ProtocolEnvelope envelope)
    {
        ArgumentNullException.ThrowIfNull(envelope);
        if (envelope.Type != MessageType.MissionState)
        {
            throw new ProtocolException(
                "The authoritative mission cache only accepts MissionState frames.");
        }

        var candidate = BinaryPayloadCodec.DecodeMissionState(
            envelope.Payload.Span);
        var frozen = Freeze(envelope);
        lock (_sync)
        {
            if (_envelope is null || _state is null)
            {
                _envelope = frozen;
                _state = candidate;
                return new MissionStateCacheUpdate(
                    MissionStateCacheDisposition.Accepted,
                    candidate);
            }

            var current = _state.Value;
            if (candidate.MissionEpoch < current.MissionEpoch ||
                (candidate.MissionEpoch == current.MissionEpoch &&
                 candidate.Revision < current.Revision))
            {
                return new MissionStateCacheUpdate(
                    MissionStateCacheDisposition.Stale,
                    candidate);
            }

            var sameVersion =
                candidate.MissionEpoch == current.MissionEpoch &&
                candidate.Revision == current.Revision;
            if (sameVersion)
            {
                if (candidate != current)
                {
                    throw new ProtocolException(
                        "MissionState changed without advancing its revision.");
                }
                if (!SequenceNumber.IsNewer(
                        envelope.Sequence,
                        _envelope.Sequence))
                {
                    return new MissionStateCacheUpdate(
                        MissionStateCacheDisposition.Stale,
                        candidate);
                }

                _envelope = frozen;
                return new MissionStateCacheUpdate(
                    MissionStateCacheDisposition.Refreshed,
                    candidate);
            }

            _envelope = frozen;
            _state = candidate;
            return new MissionStateCacheUpdate(
                MissionStateCacheDisposition.Accepted,
                candidate);
        }
    }

    public ProtocolEnvelope? Capture()
    {
        lock (_sync)
        {
            return _envelope;
        }
    }

    public void Clear()
    {
        lock (_sync)
        {
            _envelope = null;
            _state = null;
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
