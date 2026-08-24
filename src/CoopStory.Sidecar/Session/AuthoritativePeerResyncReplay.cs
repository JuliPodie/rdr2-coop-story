using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

internal enum AuthoritativePeerResyncDefinitionDisposition
{
    Included,
    NotCached,
    MissingMissionState,
    InvalidMissionState,
    MissingCinematicState,
    InvalidCinematicState,
    MissionCinematicMismatch,
    InvalidWorldGraph,
    InvalidDefinition,
    DefinitionKeyMismatch,
    TerminalCinematic
}

internal readonly record struct AuthoritativePeerResyncReplayPlan(
    IReadOnlyList<ProtocolEnvelope> Frames,
    AuthoritativePeerResyncDefinitionDisposition DefinitionDisposition)
{
    public bool IncludesDefinition =>
        DefinitionDisposition ==
            AuthoritativePeerResyncDefinitionDisposition.Included;
}

internal readonly record struct AuthoritativePeerResyncReplayResult(
    bool Completed,
    int DeliveredFrames,
    MessageType? FailedType,
    bool DefinitionDelivered);

/// <summary>
/// Builds and sends a reconnect snapshot in dependency order. The AnimScene
/// definition is admitted only when the cached mission and cinematic keys are
/// coherent, and it is always the final reliable frame.
/// </summary>
internal static class AuthoritativePeerResyncReplay
{
    public static AuthoritativePeerResyncReplayPlan Create(
        ProtocolEnvelope? missionEnvelope,
        ProtocolEnvelope? cinematicEnvelope,
        IReadOnlyList<ProtocolEnvelope> parentFirstSpawns,
        ProtocolEnvelope? definitionEnvelope)
    {
        ArgumentNullException.ThrowIfNull(parentFirstSpawns);

        var frames = new List<ProtocolEnvelope>(
            3 + parentFirstSpawns.Count);
        var missionValid = TryDecodeMission(
            missionEnvelope,
            out var mission);
        if (missionValid)
        {
            frames.Add(Freeze(missionEnvelope!));
        }

        var cinematicValid = TryDecodeCinematic(
            cinematicEnvelope,
            out var cinematic);
        var missionCinematicMatch = missionValid && cinematicValid &&
            MissionAndCinematicKeysMatch(mission, cinematic);
        if (missionCinematicMatch)
        {
            frames.Add(Freeze(cinematicEnvelope!));
        }

        var worldGraphValid = TryFreezeParentFirstSpawns(
            parentFirstSpawns,
            out var frozenSpawns);
        if (worldGraphValid)
        {
            frames.AddRange(frozenSpawns);
        }

        var disposition = DetermineDefinitionDisposition(
            missionEnvelope,
            missionValid,
            mission,
            cinematicEnvelope,
            cinematicValid,
            cinematic,
            missionCinematicMatch,
            worldGraphValid,
            definitionEnvelope,
            out var frozenDefinition);
        if (disposition ==
                AuthoritativePeerResyncDefinitionDisposition.Included)
        {
            frames.Add(frozenDefinition!);
        }

        return new AuthoritativePeerResyncReplayPlan(
            frames.ToArray(),
            disposition);
    }

    public static async ValueTask<AuthoritativePeerResyncReplayResult>
        SendAsync(
            AuthoritativePeerResyncReplayPlan plan,
            Func<ProtocolEnvelope, CancellationToken, ValueTask<bool>> send,
            CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(plan.Frames);
        ArgumentNullException.ThrowIfNull(send);

        var delivered = 0;
        var definitionDelivered = false;
        foreach (var frame in plan.Frames)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!await send(frame, cancellationToken).ConfigureAwait(false))
            {
                return new AuthoritativePeerResyncReplayResult(
                    Completed: false,
                    delivered,
                    frame.Type,
                    definitionDelivered);
            }

            delivered++;
            definitionDelivered |=
                frame.Type == MessageType.AnimSceneDefinition;
        }

        return new AuthoritativePeerResyncReplayResult(
            Completed: true,
            delivered,
            FailedType: null,
            definitionDelivered);
    }

    private static AuthoritativePeerResyncDefinitionDisposition
        DetermineDefinitionDisposition(
            ProtocolEnvelope? missionEnvelope,
            bool missionValid,
            MissionStatePayload mission,
            ProtocolEnvelope? cinematicEnvelope,
            bool cinematicValid,
            MissionCinematicStatePayload cinematic,
            bool missionCinematicMatch,
            bool worldGraphValid,
            ProtocolEnvelope? definitionEnvelope,
            out ProtocolEnvelope? frozenDefinition)
    {
        frozenDefinition = null;
        if (definitionEnvelope is null)
        {
            return AuthoritativePeerResyncDefinitionDisposition.NotCached;
        }
        if (missionEnvelope is null)
        {
            return AuthoritativePeerResyncDefinitionDisposition
                .MissingMissionState;
        }
        if (!missionValid)
        {
            return AuthoritativePeerResyncDefinitionDisposition
                .InvalidMissionState;
        }
        if (cinematicEnvelope is null)
        {
            return AuthoritativePeerResyncDefinitionDisposition
                .MissingCinematicState;
        }
        if (!cinematicValid)
        {
            return AuthoritativePeerResyncDefinitionDisposition
                .InvalidCinematicState;
        }
        if (!missionCinematicMatch)
        {
            return AuthoritativePeerResyncDefinitionDisposition
                .MissionCinematicMismatch;
        }
        if (!worldGraphValid)
        {
            return AuthoritativePeerResyncDefinitionDisposition
                .InvalidWorldGraph;
        }
        if (cinematic.Phase is MissionCinematicPhase.Completed or
            MissionCinematicPhase.Aborted)
        {
            return AuthoritativePeerResyncDefinitionDisposition
                .TerminalCinematic;
        }
        if (!TryDecodeDefinition(definitionEnvelope, out var definition))
        {
            return AuthoritativePeerResyncDefinitionDisposition
                .InvalidDefinition;
        }
        if (definition.HostEntityId != mission.HostEntityId ||
            definition.MissionEpoch != mission.MissionEpoch ||
            definition.HostEntityId != cinematic.HostEntityId ||
            definition.MissionEpoch != cinematic.MissionEpoch ||
            definition.CinematicGeneration !=
                cinematic.CinematicGeneration)
        {
            return AuthoritativePeerResyncDefinitionDisposition
                .DefinitionKeyMismatch;
        }

        frozenDefinition = Freeze(definitionEnvelope);
        return AuthoritativePeerResyncDefinitionDisposition.Included;
    }

    private static bool MissionAndCinematicKeysMatch(
        MissionStatePayload mission,
        MissionCinematicStatePayload cinematic) =>
        mission.HostEntityId == cinematic.HostEntityId &&
        mission.MissionEpoch == cinematic.MissionEpoch &&
        mission.CheckpointGeneration == cinematic.CheckpointGeneration;

    private static bool TryDecodeMission(
        ProtocolEnvelope? envelope,
        out MissionStatePayload payload)
    {
        payload = default;
        if (!HasExpectedTypeAndVersion(envelope, MessageType.MissionState))
        {
            return false;
        }

        try
        {
            payload = BinaryPayloadCodec.DecodeMissionState(
                envelope!.Payload.Span);
            return true;
        }
        catch (ProtocolException)
        {
            return false;
        }
    }

    private static bool TryDecodeCinematic(
        ProtocolEnvelope? envelope,
        out MissionCinematicStatePayload payload)
    {
        payload = default;
        if (!HasExpectedTypeAndVersion(
                envelope,
                MessageType.MissionCinematicState))
        {
            return false;
        }

        try
        {
            payload = BinaryPayloadCodec.DecodeMissionCinematicState(
                envelope!.Payload.Span);
            return true;
        }
        catch (ProtocolException)
        {
            return false;
        }
    }

    private static bool TryDecodeDefinition(
        ProtocolEnvelope envelope,
        out AnimSceneDefinitionPayload payload)
    {
        payload = null!;
        if (!HasExpectedTypeAndVersion(
                envelope,
                MessageType.AnimSceneDefinition))
        {
            return false;
        }

        try
        {
            payload = BinaryPayloadCodec.DecodeAnimSceneDefinition(
                envelope.Payload.Span);
            return true;
        }
        catch (ProtocolException)
        {
            return false;
        }
    }

    private static bool TryFreezeParentFirstSpawns(
        IReadOnlyList<ProtocolEnvelope> spawns,
        out IReadOnlyList<ProtocolEnvelope> frozen)
    {
        var decoded = new List<(
            ProtocolEnvelope Envelope,
            WorldEntityStatePayload State)>(spawns.Count);
        var ids = new HashSet<NetEntityId>();
        foreach (var envelope in spawns)
        {
            if (!HasExpectedTypeAndVersion(
                    envelope,
                    MessageType.EntitySpawn))
            {
                frozen = [];
                return false;
            }

            try
            {
                var state = BinaryPayloadCodec.DecodeWorldEntityState(
                    envelope.Payload.Span);
                if (!ids.Add(state.EntityId))
                {
                    frozen = [];
                    return false;
                }
                decoded.Add((envelope, state));
            }
            catch (ProtocolException)
            {
                frozen = [];
                return false;
            }
        }

        var seen = new HashSet<NetEntityId>();
        var result = new List<ProtocolEnvelope>(decoded.Count);
        foreach (var item in decoded)
        {
            if (item.State.ParentEntityId.IsValid &&
                ids.Contains(item.State.ParentEntityId) &&
                !seen.Contains(item.State.ParentEntityId))
            {
                frozen = [];
                return false;
            }

            seen.Add(item.State.EntityId);
            result.Add(Freeze(item.Envelope));
        }

        frozen = result;
        return true;
    }

    private static bool HasExpectedTypeAndVersion(
        ProtocolEnvelope? envelope,
        MessageType type) =>
        envelope is not null &&
        envelope.Type == type &&
        envelope.Version == ProtocolConstants.Version;

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
