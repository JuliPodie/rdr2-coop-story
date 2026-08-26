using System.Numerics;

namespace CoopStory.Sidecar.Missions;

/// <summary>Data that adapts generic host-authoritative presentation to one mission.</summary>
public sealed record MissionProfile(
    string Id,
    string DisplayName,
    IReadOnlyList<MissionStageProfile> Stages)
{
    public void Validate()
    {
        if (string.IsNullOrWhiteSpace(Id) || Id.Length > 64 ||
            string.IsNullOrWhiteSpace(DisplayName) || DisplayName.Length > 128 ||
            Stages.Count == 0 || Stages.Count > 32 ||
            Stages.GroupBy(stage => stage.Id, StringComparer.OrdinalIgnoreCase)
                .Any(group => group.Count() != 1))
        {
            throw new ArgumentException("Mission profile identity or stages are invalid.");
        }
        foreach (var stage in Stages)
        {
            stage.Validate();
        }
    }
}

public sealed record MissionStageProfile(
    string Id,
    string Objective,
    MissionStageKind Kind,
    Vector3 HostAnchor,
    float TriggerRadiusMeters,
    Vector3 GuestCompanionOffset,
    uint CheckpointGeneration,
    MissionPresentationFallback Fallback)
{
    public void Validate()
    {
        if (string.IsNullOrWhiteSpace(Id) || Id.Length > 64 ||
            string.IsNullOrWhiteSpace(Objective) || Objective.Length > 256 ||
            !Enum.IsDefined(Kind) || !Enum.IsDefined(Fallback) ||
            !float.IsFinite(TriggerRadiusMeters) ||
            TriggerRadiusMeters is <= 0 or > 250 ||
            !Finite(HostAnchor) || !Finite(GuestCompanionOffset) ||
            CheckpointGeneration == 0)
        {
            throw new ArgumentException($"Mission stage '{Id}' is invalid.");
        }
    }

    private static bool Finite(Vector3 value) =>
        float.IsFinite(value.X) && float.IsFinite(value.Y) && float.IsFinite(value.Z);
}

public enum MissionStageKind { StartTrigger, Ride, ArrivalTrigger, CutsceneBoundary }
public enum MissionPresentationFallback { None, TeleportGuestToCompanionAnchor, SpectatorCamera }
