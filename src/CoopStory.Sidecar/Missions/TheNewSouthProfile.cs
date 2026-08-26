using System.Numerics;

namespace CoopStory.Sidecar.Missions;

/// <summary>
/// Chapter 3 "The New South" opening ride. The host's live anchor is captured
/// at activation instead of hard-coding version-sensitive game coordinates.
/// </summary>
public static class TheNewSouthProfile
{
    public static MissionProfile Create(Vector3 liveHostAnchor) => new(
        "the_new_south",
        "The New South",
        [
            Stage(liveHostAnchor, "talk_to_dutch", "Talk to Dutch to begin the ride.",
                MissionStageKind.StartTrigger, 8f, new(-2.5f, -3f, 0f), 1,
                MissionPresentationFallback.TeleportGuestToCompanionAnchor),
            Stage(liveHostAnchor, "ride_to_rhodes", "Ride with the gang toward Rhodes.",
                MissionStageKind.Ride, 30f, new(-3f, -5f, 0f), 2,
                MissionPresentationFallback.TeleportGuestToCompanionAnchor),
            Stage(liveHostAnchor, "rhodes_arrival", "Reach the Rhodes mission arrival area.",
                MissionStageKind.ArrivalTrigger, 15f, new(-2f, -4f, 0f), 3,
                MissionPresentationFallback.TeleportGuestToCompanionAnchor),
            Stage(liveHostAnchor, "first_cinematic", "Watch the host's mission presentation.",
                MissionStageKind.CutsceneBoundary, 15f, new(-2f, -4f, 0f), 3,
                MissionPresentationFallback.SpectatorCamera)
        ]);

    private static MissionStageProfile Stage(
        Vector3 hostAnchor, string id, string objective, MissionStageKind kind, float radius,
        Vector3 guestOffset, uint checkpoint, MissionPresentationFallback fallback) =>
        new(id, objective, kind, hostAnchor, radius, guestOffset, checkpoint, fallback);
}
