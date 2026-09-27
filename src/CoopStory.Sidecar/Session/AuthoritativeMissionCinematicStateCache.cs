using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

internal enum MissionCinematicStateCacheDisposition
{
    Accepted,
    Refreshed,
    Stale
}

internal readonly record struct MissionCinematicStateCacheUpdate(
    MissionCinematicStateCacheDisposition Disposition,
    MissionCinematicStatePayload State);

/// <summary>
/// Retains the latest reliable cinematic FSM state while the game pipe is unavailable.
/// Ordering is mission epoch, cinematic generation, then revision; equal versions must be byte-equivalent heartbeats.
/// </summary>
internal sealed class AuthoritativeMissionCinematicStateCache
{
    private readonly object _sync = new();
    private ProtocolEnvelope? _envelope;
    private MissionCinematicStatePayload? _state;

    public MissionCinematicStateCacheUpdate Apply(ProtocolEnvelope envelope)
    {
        ArgumentNullException.ThrowIfNull(envelope);
        if (envelope.Type != MessageType.MissionCinematicState)
        {
            throw new ProtocolException(
                "The cinematic cache only accepts MissionCinematicState frames.");
        }

        var candidate = BinaryPayloadCodec.DecodeMissionCinematicState(
            envelope.Payload.Span);
        var frozen = Freeze(envelope);
        lock (_sync)
        {
            if (_envelope is null || _state is null)
            {
                _envelope = frozen;
                _state = candidate;
                return new MissionCinematicStateCacheUpdate(
                    MissionCinematicStateCacheDisposition.Accepted,
                    candidate);
            }

            var current = _state.Value;
            // Cinematic generation sits between mission epoch and revision: a newer cutscene must supersede an older one even if its revision is low.
            var ordering = CompareVersion(candidate, current);
            if (ordering < 0)
            {
                return new MissionCinematicStateCacheUpdate(
                    MissionCinematicStateCacheDisposition.Stale,
                    candidate);
            }

            if (ordering == 0)
            {
                if (candidate != current)
                {
                    throw new ProtocolException(
                        "MissionCinematicState changed without advancing its revision.");
                }
                if (!SequenceNumber.IsNewer(envelope.Sequence, _envelope.Sequence))
                {
                    return new MissionCinematicStateCacheUpdate(
                        MissionCinematicStateCacheDisposition.Stale,
                        candidate);
                }

                _envelope = frozen;
                return new MissionCinematicStateCacheUpdate(
                    MissionCinematicStateCacheDisposition.Refreshed,
                    candidate);
            }

            _envelope = frozen;
            _state = candidate;
            return new MissionCinematicStateCacheUpdate(
                MissionCinematicStateCacheDisposition.Accepted,
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

    // Compare semantic cinematic identity, not transport sequence, because TCP reconnect replay may assign newer delivery sequence numbers to old state.
    private static int CompareVersion(
        MissionCinematicStatePayload left,
        MissionCinematicStatePayload right)
    {
        var epoch = left.MissionEpoch.CompareTo(right.MissionEpoch);
        if (epoch != 0)
        {
            return epoch;
        }

        var generation = left.CinematicGeneration.CompareTo(
            right.CinematicGeneration);
        return generation != 0
            ? generation
            : left.Revision.CompareTo(right.Revision);
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
