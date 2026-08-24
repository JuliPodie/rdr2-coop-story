using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

internal enum AnimSceneDefinitionCacheDisposition
{
    Accepted,
    Refreshed,
    Stale
}

internal readonly record struct AnimSceneDefinitionCacheKey(
    NetEntityId HostEntityId,
    uint MissionEpoch,
    uint CinematicGeneration,
    uint DefinitionRevision,
    ulong FingerprintLow,
    ulong FingerprintHigh);

internal readonly record struct AnimSceneDefinitionCacheUpdate(
    AnimSceneDefinitionCacheDisposition Disposition,
    AnimSceneDefinitionPayload Definition,
    AnimSceneDefinitionCacheKey Key);

/// <summary>
/// Retains the latest validated, active host-authored AnimScene definition.
/// Mission epoch, cinematic generation and definition revision establish
/// ordering. An equal version must keep the same host, fingerprint and bytes.
/// </summary>
internal sealed class AuthoritativeAnimSceneDefinitionCache
{
    private readonly object _sync = new();
    private ProtocolEnvelope? _envelope;
    private AnimSceneDefinitionPayload? _definition;
    private AnimSceneDefinitionCacheKey? _key;

    public AnimSceneDefinitionCacheUpdate Apply(ProtocolEnvelope envelope)
    {
        ArgumentNullException.ThrowIfNull(envelope);
        if (envelope.Type != MessageType.AnimSceneDefinition)
        {
            throw new ProtocolException(
                "The AnimScene definition cache only accepts AnimSceneDefinition frames.");
        }

        var candidate = BinaryPayloadCodec.DecodeAnimSceneDefinition(
            envelope.Payload.Span);
        var candidateKey = CreateKey(candidate);
        var frozen = Freeze(envelope);
        lock (_sync)
        {
            if (_envelope is null || _definition is null || _key is null)
            {
                _envelope = frozen;
                _definition = candidate;
                _key = candidateKey;
                return new AnimSceneDefinitionCacheUpdate(
                    AnimSceneDefinitionCacheDisposition.Accepted,
                    candidate,
                    candidateKey);
            }

            var ordering = CompareVersion(candidateKey, _key.Value);
            if (ordering < 0)
            {
                return new AnimSceneDefinitionCacheUpdate(
                    AnimSceneDefinitionCacheDisposition.Stale,
                    candidate,
                    candidateKey);
            }

            if (ordering == 0)
            {
                if (candidateKey != _key.Value ||
                    !envelope.Payload.Span.SequenceEqual(
                        _envelope.Payload.Span))
                {
                    throw new ProtocolException(
                        "AnimSceneDefinition changed its host, fingerprint or payload without advancing its definition revision.");
                }
                if (!SequenceNumber.IsNewer(
                        envelope.Sequence,
                        _envelope.Sequence))
                {
                    return new AnimSceneDefinitionCacheUpdate(
                        AnimSceneDefinitionCacheDisposition.Stale,
                        candidate,
                        candidateKey);
                }

                _envelope = frozen;
                return new AnimSceneDefinitionCacheUpdate(
                    AnimSceneDefinitionCacheDisposition.Refreshed,
                    candidate,
                    candidateKey);
            }

            _envelope = frozen;
            _definition = candidate;
            _key = candidateKey;
            return new AnimSceneDefinitionCacheUpdate(
                AnimSceneDefinitionCacheDisposition.Accepted,
                candidate,
                candidateKey);
        }
    }

    public ProtocolEnvelope? Capture()
    {
        lock (_sync)
        {
            return _envelope;
        }
    }

    public bool ClearMatching(AnimSceneControlPayload control)
    {
        lock (_sync)
        {
            if (_key is not { } key ||
                key.HostEntityId != control.HostEntityId ||
                key.MissionEpoch != control.MissionEpoch ||
                key.CinematicGeneration != control.CinematicGeneration ||
                key.DefinitionRevision != control.DefinitionRevision ||
                key.FingerprintLow != control.FingerprintLow ||
                key.FingerprintHigh != control.FingerprintHigh)
            {
                return false;
            }

            ClearLocked();
            return true;
        }
    }

    public bool ClearTerminal(MissionCinematicStatePayload state)
    {
        if (state.Phase is not MissionCinematicPhase.Completed and
            not MissionCinematicPhase.Aborted)
        {
            return false;
        }

        lock (_sync)
        {
            if (_key is not { } key ||
                key.HostEntityId != state.HostEntityId ||
                key.MissionEpoch != state.MissionEpoch ||
                key.CinematicGeneration != state.CinematicGeneration)
            {
                return false;
            }

            ClearLocked();
            return true;
        }
    }

    public void Clear()
    {
        lock (_sync)
        {
            ClearLocked();
        }
    }

    private void ClearLocked()
    {
        _envelope = null;
        _definition = null;
        _key = null;
    }

    private static AnimSceneDefinitionCacheKey CreateKey(
        AnimSceneDefinitionPayload definition) =>
        new(
            definition.HostEntityId,
            definition.MissionEpoch,
            definition.CinematicGeneration,
            definition.DefinitionRevision,
            definition.FingerprintLow,
            definition.FingerprintHigh);

    private static int CompareVersion(
        AnimSceneDefinitionCacheKey left,
        AnimSceneDefinitionCacheKey right)
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
            : left.DefinitionRevision.CompareTo(right.DefinitionRevision);
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
