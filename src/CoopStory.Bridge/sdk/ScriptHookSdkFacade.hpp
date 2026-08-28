#pragma once

#include "coopstory/bridge/EntityRegistry.hpp"
#include "coopstory/bridge/IScriptHookFacade.hpp"
#include "coopstory/bridge/PlayerActionPolicy.hpp"
#include "coopstory/bridge/RemoteMotion.hpp"

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coopstory::bridge::sdk {

class ScriptHookSdkFacade final : public IScriptHookFacade {
public:
    ScriptHookSdkFacade();
    ~ScriptHookSdkFacade() override;
    void AbandonNativeCleanupAfterFatal() noexcept;

    [[nodiscard]] std::uint64_t TickMilliseconds() noexcept override;
    [[nodiscard]] RuntimeMode QueryRuntimeMode() noexcept override;
    [[nodiscard]] std::optional<LocalPlayerSample> SampleLocalPlayer()
        noexcept override;
    [[nodiscard]] std::optional<PlayerAppearanceStatePayload>
    SampleLocalAppearance(
        NetEntityId entityId,
        PlayerSlot slot,
        std::uint32_t revision) noexcept override;
    [[nodiscard]] std::optional<MissionCameraSample> SampleMissionCamera()
        noexcept override;
    [[nodiscard]] std::optional<MissionObjectiveSample>
    SampleMissionObjective() noexcept override;
    [[nodiscard]] std::vector<MissionDialogueSample>
    SampleMissionDialogue(std::uint32_t missionId) noexcept override;
    [[nodiscard]] std::optional<AmbientEncounterObservation>
    SampleAmbientEncounterObservation() noexcept override;
    [[nodiscard]] std::optional<ExactEncounterObservation>
    SampleExactEncounterObservation() noexcept override;
    [[nodiscard]] bool BeginAmbientEncounterPresentation(
        const AmbientEncounterInstance& instance) noexcept override;
    [[nodiscard]] std::optional<AmbientEncounterPhase>
    SampleAmbientEncounterOutcome(std::uint64_t instanceId) noexcept override;
    void ClearAmbientEncounterPresentation(std::uint64_t instanceId) noexcept override;
    [[nodiscard]] bool PresentHostMissionDialogue(
        std::uint32_t missionId,
        std::uint32_t rootId) noexcept override;
    void ClearHostMissionDialoguePresentation() noexcept override;
    [[nodiscard]] std::optional<AnimSceneReplicaStatePayload>
    SampleHostAnimScene(
        NetEntityId hostEntityId,
        std::uint32_t missionEpoch,
        std::uint32_t cinematicGeneration,
        std::uint32_t revision) noexcept override;
    [[nodiscard]] std::optional<LocalEntityHandle>
    SampledHostAnimSceneLocalHandle() noexcept override;
    [[nodiscard]] std::vector<CapturedAnimSceneDefinition>
    DrainCapturedAnimSceneDefinitions() noexcept override;
    [[nodiscard]] std::optional<NetEntityId>
    FindKnownReplicaNetworkId(
        LocalEntityHandle localHandle) noexcept override;
    [[nodiscard]] std::optional<PlayerAnimationStatePayload>
    SampleLocalAnimationState(
        NetEntityId entityId,
        PlayerSlot slot,
        std::uint16_t locomotionEpoch,
        std::uint32_t sampleSequence) noexcept override;
    [[nodiscard]] std::optional<WorldStatePayload> SampleWorldState()
        noexcept override;
    [[nodiscard]] std::vector<HostWorldEntitySample> SampleWorldEntities(
        float radiusMeters,
        std::size_t maximumEntities) noexcept override;
    [[nodiscard]] std::optional<DamageIntentPayload> SampleWorldDamageIntent(
        NetEntityId attackerId) noexcept override;
    [[nodiscard]] std::vector<VanillaPickupCollection>
    DrainVanillaPickupCollections() noexcept override;
    [[nodiscard]] std::vector<CampaignCapabilityObservation>
    DrainCampaignCapabilityObservations() noexcept override;
    [[nodiscard]] std::optional<CampaignMissionProbe>
    ProbeCampaignMission(std::uint32_t expectedMissionId) noexcept override;
    [[nodiscard]] bool ApplyCampaignMissionCompletion(
        std::uint32_t missionId,
        std::uint64_t completionEventId,
        std::uint8_t completionRating) noexcept override;
    [[nodiscard]] std::optional<std::int32_t>
    QueryLocalCashBalance() noexcept override;
    [[nodiscard]] bool ApplyCampaignMissionCashAward(
        std::uint64_t completionEventId,
        std::int32_t amount) noexcept override;
    void ObserveVanillaPickupCollection() noexcept;
    void ObserveScriptEvents() noexcept;
    [[nodiscard]] std::optional<float> HostGuestDistanceMeters()
        noexcept override;
    [[nodiscard]] MenuInputState ReadMenuInput() noexcept override;
    void DrawMenu(
        bool open,
        std::span<const BridgeCommand> commands,
        std::size_t selected) noexcept override;
    void DrawSessionMenu(
        const SessionOverlayView& state) noexcept override;
    void DrawBridgeHud(const BridgeHudState& state) noexcept override;
    void DrawPauseVoteStatus(
        const PauseVoteView& state) noexcept override;
    void DrawNotification(
        std::string_view text,
        bool success) noexcept override;
    [[nodiscard]] std::string ReadClipboardText() noexcept override;
    [[nodiscard]] bool WriteClipboardText(
        std::string_view text) noexcept override;
    void ShowMissionBubbleWarning(float distanceMeters) noexcept override;
    [[nodiscard]] bool ExecuteCommand(BridgeCommand command) noexcept override;
    [[nodiscard]] bool ApplyNetworkCommand(
        const CommandPayload& command) noexcept override;
    [[nodiscard]] bool ApplyRemoteTransform(
        const PlayerStatePayload& state) noexcept override;
    [[nodiscard]] bool ApplyRemoteAnimationState(
        const PlayerAnimationStatePayload& state) noexcept override;
    void ConfigureMotionReplication(
        const MotionReplicationConfigPayload& config) noexcept override;
    void SetAnimSceneCaptureAuthority(
        bool hostAuthority) noexcept override;
    [[nodiscard]] bool ApplyRemoteTraversal(
        const PlayerTraversalPayload& traversal) noexcept override;
    [[nodiscard]] bool ApplyRemotePlayerAction(
        const PlayerActionPayload& action) noexcept override;
    [[nodiscard]] bool ApplyInteractionResult(
        const InteractionResultPayload& result,
        NetEntityId localEntityId) noexcept override;
    [[nodiscard]] bool ApplyRestraintState(
        const RestraintStatePayload& state,
        NetEntityId localEntityId) noexcept override;
    void MaintainLocalDownedState(
        bool active,
        float restoredHealthFraction) noexcept override;
    [[nodiscard]] bool ApplyRemoteIdentity(
        const PlayerIdentityPayload& identity) noexcept override;
    [[nodiscard]] bool ApplyRemoteAppearance(
        const PlayerAppearanceStatePayload& appearance) noexcept override;
    [[nodiscard]] bool ApplyWorldState(
        const WorldStatePayload& state) noexcept override;
    [[nodiscard]] bool ApplyRemoteEquipment(
        const EquipmentStatePayload& state) noexcept override;
    [[nodiscard]] bool UnlockLocalWeaponEntitlement(
        std::uint32_t weaponHash) noexcept override;
    [[nodiscard]] bool ApplyCampaignCapability(
        const CampaignCapabilityPayload& capability) noexcept override;
    [[nodiscard]] bool MaintainRemoteMount(
        const PlayerMountStatePayload& state,
        const std::optional<PlayerMountStatePayload>& localState) noexcept override;
    void ClearRemoteMount() noexcept override;
    [[nodiscard]] bool SpawnWorldEntityProxy(
        const WorldEntityStatePayload& state) noexcept override;
    [[nodiscard]] bool UpdateWorldEntityProxy(
        const WorldEntityStatePayload& state) noexcept override;
    void DespawnWorldEntityProxy(NetEntityId entityId) noexcept override;
    void MaintainWorldMirrorGuest(
        bool active,
        bool authoritativePopulationReady,
        float radiusMeters) noexcept override;
    [[nodiscard]] bool ApplyWorldEntityDamage(
        LocalEntityHandle target,
        float damage) noexcept override;
    [[nodiscard]] bool ApplyMissionWorldEntityDamage(
        LocalEntityHandle target,
        std::uint32_t weaponHash,
        float damage) noexcept override;
    void MaintainRealtimeSession(
        bool active,
        bool synchronizedPaused) noexcept override;
    [[nodiscard]] GuestMissionIsolationStatus MaintainMissionAuthority(
        bool active,
        bool hostMissionActive,
        bool hostPresentationActive,
        bool allowExpectedLocalMissionInstance = false) noexcept override;
    void MaintainMissionSpectator(bool active) noexcept override;
    void MaintainMissionResumeBarrier(bool active) noexcept override;
    [[nodiscard]] MissionResumePreparation PrepareMissionCinematicResume(
        const Vec3& anchor,
        float heading,
        std::uint64_t nowMs) noexcept override;
    [[nodiscard]] bool IsCutsceneSkipPressed() noexcept override;
    void MaintainCutsceneSkipInput(bool active) noexcept override;
    void MaintainReplicatedMissionCamera(
        bool spectatorActive,
        const std::optional<MissionCameraStatePayload>& state) noexcept override;
    [[nodiscard]] bool MaintainReplicatedAnimScene(
        bool spectatorActive,
        const std::optional<AnimSceneReplicaStatePayload>& state) noexcept override;
    [[nodiscard]] ReplicatedAnimScenePrepareResult
    PrepareReplicatedAnimSceneDefinition(
        const AnimSceneDefinitionPayload& definition,
        NetEntityId localEntityId) noexcept override;
    [[nodiscard]] bool MaintainHostAnimSceneStartBarrier(
        bool active) noexcept override;
    [[nodiscard]] bool CommitReplicatedAnimSceneDefinition(
        const AnimSceneControlPayload& commit) noexcept override;
    void AbortReplicatedAnimSceneDefinition() noexcept override;
    void MaintainMissionCompanionPresentation(
        const MissionCompanionPresentation& state) noexcept override;
    [[nodiscard]] RuntimeDivergenceDiagnostics
    SampleRuntimeDivergenceDiagnostics() noexcept override;
    void MaintainRemoteMissionParticipant(bool hidden) noexcept override;
    void RequestCheckpointRetry() noexcept override;
    void Log(std::string_view text) noexcept override;
    void WaitForNextTick() noexcept override;

private:
    struct AmbientEncounterPresentation final {
        std::uint64_t instanceId{};
        AmbientEncounterProfile profile{AmbientEncounterProfile::RoadsideAmbush};
        std::uint32_t sourceScriptId{};
        // The first hostileCount entries are host-authoritative combatants.
        // The remaining entries are non-combatant scene roles.
        std::vector<LocalEntityHandle> peds{};
        std::size_t hostileCount{};
        std::size_t protectedCivilianCount{};
        struct SuppressedSourcePed final {
            LocalEntityHandle handle{};
            std::uint32_t modelHash{};
            bool wasVisible{};
        };
        // Only tracks source actors hidden while a bridge-owned exact event
        // is active.  They are restored, never deleted or rewarded by us.
        std::vector<SuppressedSourcePed> suppressedSourcePeds{};
    };
    bool abandonNativeCleanupAfterFatal_{};
    std::optional<AmbientEncounterPresentation> ambientEncounterPresentation_{};
    std::uint32_t ambientEncounterCooldownScriptId_{};
    std::uint64_t ambientEncounterCooldownUntilMs_{};
    bool remoteTransformNativeFaultLogged_{};
    void InspectAnimSceneHybridHandlers(int sceneHandle) noexcept;
    void ResetRemoteMotionTracking() noexcept;
    [[nodiscard]] bool ApplyRemoteTransformUnsafe(
        const PlayerStatePayload& state) noexcept;
    [[nodiscard]] bool ApplyRemoteAnimGraphTransform(
        LocalEntityHandle handle,
        const PlayerStatePayload& state) noexcept;
    [[nodiscard]] bool EnsureRemoteGunshotAudio() noexcept;
    void ReleaseRemoteGunshotAudio() noexcept;
    [[nodiscard]] bool StartRemoteVisualFirePulse(
        LocalEntityHandle actor,
        const Vec3& target,
        std::uint64_t nowMs) noexcept;
    void RestoreRemoteVisualFireAmmo(
        LocalEntityHandle actor,
        std::uint64_t nowMs,
        bool force) noexcept;
    void ClearRemotePlayerActions() noexcept;
    void ExpireRemotePlayerActions(std::uint64_t nowMs) noexcept;
    void RefreshRemotePlayerActionDerivedState() noexcept;
    void CancelRemoteMeleeVisual(
        LocalEntityHandle actor,
        std::string_view reason) noexcept;
    void MaintainPendingPeerDismount(std::uint64_t nowMs) noexcept;
    void DeleteRemotePeerLassoRope(
        std::string_view reason =
            "terminal-preemption-or-session-reset") noexcept;
    void RestoreLocalLassoHogtieFlags() noexcept;
    void ClearRemoteIdentityDecoration() noexcept;
    void CaptureRemoteWaypoint(
        const Vec3& position,
        std::uint64_t nowMs) noexcept;
    void ConsumeReachedRemoteWaypoints(
        const Vec3& currentPosition) noexcept;
    [[nodiscard]] std::optional<Vec3> SelectRemoteNavigationWaypoint()
        noexcept;
    [[nodiscard]] std::optional<Vec3> SelectRemoteRouteWaypoint(
        float lookAheadMeters) const noexcept;
    [[nodiscard]] float EstimateRemoteRouteCurvatureDegrees() const noexcept;
    void RestoreHiddenAmbientPeds() noexcept;
    void MaintainHiddenPedAttachments() noexcept;
    void CleanupWorldEntityProxies() noexcept;
    [[nodiscard]] bool IsOwnedHybridAnimSceneEntity(
        LocalEntityHandle handle) const noexcept;

    enum class WorldProxySpawnDisposition : std::uint8_t {
        PendingModel,
        PendingDependency,
        RetryableFailure,
        Spawned,
        PermanentFailure,
    };

    struct WorldProxyEntry final {
        WorldEntityStatePayload state{};
        std::uint64_t requestedAtMs{};
        std::uint64_t receivedAtMs{};
        std::uint64_t previousAimTaskMs{};
        std::uint64_t previousTaskMs{};
        Vec3 previousTaskTarget{};
        WorldTaskKind previousTaskKind{WorldTaskKind::Idle};
        std::uint32_t weaponHash{};
        std::uint32_t spawnAttempts{};
        std::uint64_t nextSpawnRetryMs{};
        WorldProxySpawnDisposition spawnDisposition{
            WorldProxySpawnDisposition::PendingModel};
        bool aiming{};
        bool mounted{};
        bool modelWaitLogged{};
        bool permanentFailureLogged{};
    };

    struct HiddenAmbientEntry final {
        std::uint32_t modelHash{};
        bool wasVisible{};
    };

    struct RemoteWaypoint final {
        Vec3 position{};
        std::uint64_t capturedAtMs{};
    };

    enum class RemoteTraversalKind {
        Jump,
        Climb,
    };

    struct RemoteTraversalIntent final {
        RemoteTraversalKind kind{RemoteTraversalKind::Jump};
        Vec3 position{};
        float heading{};
        std::uint16_t actionId{};
        std::uint64_t capturedAtMs{};
        std::uint16_t revision{};
        std::uint16_t locomotionEpoch{};
        std::uint32_t flags{};
        Vec3 approachVelocity{};
        Vec3 obstaclePoint{};
        Vec3 obstacleNormal{};
        float obstacleTopZ{};
        Vec3 expectedLanding{};
        int geometryProbeHandle{};
        std::uint64_t geometryProbeStartedMs{};
        bool geometryProbeComplete{};
        bool geometryConfirmed{};
    };

    struct RemotePlayerActionChannel final {
        std::uint32_t actionId{};
        std::uint16_t revision{};
        PlayerActionPhase phase{PlayerActionPhase::None};
        std::uint64_t receivedAtMs{};
        bool active{};
        bool targetsLocalPlayer{};
        bool physicalTargetEffect{};
    };

    std::chrono::steady_clock::time_point started_;
    bool previousMenuOpen_{};
    std::size_t previousSelection_{};
    bool previousSessionMenuOpen_{};
    SessionOverlayPhase previousSessionPhase_{
        SessionOverlayPhase::WaitingForSidecar};
    std::size_t previousSessionSelection_{};
    EntityRegistry replicas_{};
    EntityRegistry remoteMountReplicas_{};
    EntityRegistry remoteVehicleReplicas_{};
    NetEntityId remotePlayerId_{};
    NetEntityId remoteMountId_{};
    std::uint32_t remoteMountModelHash_{};
    std::uint32_t remoteMountGeneration_{};
    std::uint64_t remoteMountRequestedAtMs_{};
    std::uint64_t previousRemoteMountTaskMs_{};
    std::uint64_t previousRemoteMountTransformMs_{};
    std::uint64_t remoteMountDiagnosticsStartedMs_{};
    std::uint64_t remoteMountSamples_{};
    std::uint64_t remoteMountCorrections_{};
    std::uint64_t remoteMountHardCorrections_{};
    std::uint64_t remoteMountTaskStarts_{};
    std::uint64_t remoteMountAttachAttempts_{};
    std::uint64_t remoteMountDismountAttempts_{};
    std::uint64_t remoteMountMaximumApplyGapMs_{};
    std::uint64_t previousRemoteMountAttachAttemptMs_{};
    std::uint64_t previousRemoteMountDismountAttemptMs_{};
    float remoteMountMaximumError_{};
    Vec3 previousRemoteMountTaskDestination_{};
    LocalEntityHandle remotePlayerMountHandle_{};
    bool remoteMountMoving_{};
    bool remotePlayerMounted_{};
    bool remotePlayerMountBorrowed_{};
    LocalEntityHandle localKnownMountHandle_{};
    std::uint32_t localKnownMountModelHash_{};
    std::uint64_t localKnownMountConfirmedMs_{};
    std::uint64_t previousOwnedMountScanMs_{};
    LocalEntityHandle localKnownVehicleHandle_{};
    std::uint32_t localKnownVehicleModelHash_{};
    NetEntityId remoteVehicleId_{};
    std::uint32_t remoteVehicleModelHash_{};
    std::uint32_t remoteVehicleGeneration_{};
    std::uint64_t remoteVehicleRequestedAtMs_{};
    int localTraversalProbeHandle_{};
    std::uint64_t previousLocalTraversalProbeMs_{};
    std::uint64_t localTraversalObstacleCapturedAtMs_{};
    Vec3 localTraversalObstaclePoint_{};
    Vec3 localTraversalObstacleNormal_{};
    float localTraversalObstacleTopZ_{};
    bool localTraversalObstacleValid_{};
    std::uint64_t previousRemoteTransformMs_{};
    std::uint64_t previousRemoteTaskMs_{};
    std::uint64_t previousRemoteCoordinateCorrectionMs_{};
    std::uint64_t previousRemoteTaskRecoveryMs_{};
    std::uint64_t previousRemoteLocomotionChangeMs_{};
    Vec3 previousRemoteTaskDestination_{};
    Vec3 previousRemoteTargetPosition_{};
    float remoteDiagnosticsTargetHeading_{};
    float previousRemoteTaskHeading_{};
    RemoteLocomotion previousRemoteLocomotion_{
        RemoteLocomotion::Idle};
    bool hasPreviousRemoteTarget_{};
    bool hasRemoteLocomotionState_{};
    bool hasRemoteLocomotionTask_{};
    std::uint64_t remoteMotionDiagnosticsStartedMs_{};
    std::uint64_t remoteMotionApplyCount_{};
    std::uint64_t remoteMotionMaximumApplyGapMs_{};
    std::uint64_t remoteMotionGaitChanges_{};
    std::uint64_t remoteMotionWarps_{};
    std::uint64_t remoteMotionSoftCorrections_{};
    std::uint64_t remoteMotionHardResyncs_{};
    std::uint64_t remoteMotionEmergencyHardResyncs_{};
    std::uint64_t remoteMotionHardResyncMaximumWaitMs_{};
    float remoteMotionHardResyncMaximumDistance_{};
    std::uint64_t remoteHardResyncErrorStartedMs_{};
    std::uint64_t remoteHardResyncCooldownUntilMs_{};
    std::uint64_t remoteMotionTaskRecoveries_{};
    std::uint64_t remoteMotionAnimationTaskStarts_{};
    std::uint64_t remoteMotionDestinationRefreshes_{};
    std::uint64_t remoteMotionCatchUpTicks_{};
    std::uint64_t remoteMotionDestinationHeadingTicks_{};
    std::uint64_t remoteMotionPhysicsAssistTicks_{};
    std::uint64_t remoteMotionVerticalAssistTicks_{};
    std::uint64_t remoteMotionGroundedVerticalSuppressions_{};
    std::uint64_t remoteMotionPhysicsInterruptedTicks_{};
    std::uint64_t remoteMotionPhysicsInterruptionTransitions_{};
    double remoteMotionPositionErrorSum_{};
    float remoteMotionPositionErrorMax_{};
    float remoteMotionTargetGapMax_{};
    float remoteMotionMaximumMoveRate_{1.0F};
    float remoteMotionMaximumAssistSpeed_{};
    Vec3 remoteMotionAssistVelocity_{};
    bool hasRemoteMotionAssistVelocity_{};
    bool remoteMotionPhysicsAssistActive_{};
    bool remoteMotionPhysicsInterrupted_{};
    float remoteMotionAppliedMoveRate_{1.0F};
    std::uint16_t remoteLocomotionEpoch_{};
    std::uint16_t lastRemoteTraversalActionId_{};
    PuppetControlMode remoteControlMode_{
        PuppetControlMode::GroundedLocomotion};
    bool hasRemoteControlMode_{};
    std::array<std::uint64_t, 9> remoteControlModeTicks_{};
    std::uint64_t remoteControlModeTransitions_{};
    std::uint64_t remoteMotionGroundedVelocitySuppressions_{};
    double remoteRouteLookAheadSum_{};
    float remoteRouteLookAheadMaximum_{};
    float remoteRouteCurvatureMaximum_{};
    std::uint64_t remoteRouteLookAheadSamples_{};
    std::uint64_t remoteTaskWatchdogCooldownMs_{500U};
    std::uint64_t remoteTaskWatchdogReissues_{};
    std::uint64_t previousRemoteTaskWatchdogReissueMs_{};
    std::deque<RemoteWaypoint> remoteWaypoints_{};
    std::uint64_t previousRemoteNavigationProbeMs_{};
    std::uint64_t remoteNavigationStalledSinceMs_{};
    std::uint64_t remoteNavigationEnteredMs_{};
    std::uint64_t remoteNavigationCooldownUntilMs_{};
    std::uint64_t previousRemoteNavigationTaskMs_{};
    std::size_t remoteNavigationDestinationWaypointCount_{};
    Vec3 remoteNavigationProbePosition_{};
    Vec3 remoteNavigationProbeTarget_{};
    Vec3 remoteNavigationDestination_{};
    float remoteNavigationProbeError_{};
    bool hasRemoteNavigationProbe_{};
    bool hasRemoteNavigationDestination_{};
    bool remoteNavigationActive_{};
    std::uint64_t remoteNavigationEntries_{};
    std::uint64_t remoteNavigationExits_{};
    std::uint64_t remoteNavigationStalledSamples_{};
    std::uint64_t remoteNavigationActiveTicks_{};
    std::uint64_t remoteNavigationTaskStarts_{};
    std::uint64_t remoteNavigationWaypointCaptures_{};
    std::uint64_t remoteNavigationWaypointReached_{};
    std::uint64_t remoteNavigationWaypointDrops_{};
    std::uint64_t remoteNavigationExpiredWaypoints_{};
    std::uint64_t remoteNavigationObsoleteWaypoints_{};
    std::uint64_t remoteNavigationTrailResets_{};
    std::uint64_t remoteNavigationTimeouts_{};
    std::uint64_t remoteNavigationSafeRecoveryTeleports_{};
    float remoteNavigationSafeRecoveryMaxDistance_{};
    std::uint64_t remoteNavigationDirectTargetSelections_{};
    std::uint64_t remoteNavigationPausedTicks_{};
    std::uint64_t remoteNavigationSafeCoordHits_{};
    std::uint64_t remoteNavigationSafeCoordMisses_{};
    std::uint64_t remoteNavigationAssistSuppressedTicks_{};
    std::size_t remoteNavigationMaximumQueue_{};
    std::uint64_t previousBubbleLogMs_{};
    BridgeHudState previousHudState_{};
    bool hasPreviousHudState_{};
    std::string remoteNickname_{};
    Vec3 remoteNicknameAnchor_{};
    bool hasRemoteNicknameAnchor_{};
    int remoteBlip_{};
    std::uint32_t remoteWeaponHash_{};
    std::uint32_t remoteWeaponAmmo_{};
    SequenceWindow remoteFireSequences_{};
    std::uint64_t previousRemoteAimTaskMs_{};
    Vec3 previousRemoteAimTarget_{};
    std::uint64_t previousRemoteMeleeTaskMs_{};
    std::uint64_t localMeleeAttackLatchUntilMs_{};
    std::uint64_t localMeleeBlockLatchUntilMs_{};
    std::uint64_t localMeleeGrappleLatchUntilMs_{};
    std::uint64_t localPeerCombatEffectUntilMs_{};
    std::uint64_t localPeerKnockdownLatchUntilMs_{};
    std::uint64_t localPeerLassoIntentUntilMs_{};
    std::uint64_t localPeerMountPullLatchUntilMs_{};
    bool previousLocalMeleeAttackInput_{};
    bool previousLocalPeerGrappleInput_{};
    bool previousRemoteReplicaRagdolled_{};
    bool previousLocalDuckInput_{};
    bool localStealthToggleLatched_{};
    bool previousLocalCoverSemantic_{};
    std::uint64_t localCoverSemanticUntilMs_{};
    std::uint64_t lastRemoteAimTargetMs_{};
    Vec3 lastRemoteAimTarget_{};
    std::uint64_t pendingRemoteFireExpiresMs_{};
    Vec3 pendingRemoteFireTarget_{};
    bool remoteAiming_{};
    bool remoteAimRootSuppressed_{};
    bool remoteMeleeCombat_{};
    bool remoteMeleeBlocking_{};
    bool animGraphCoverActive_{};
    bool animGraphCoverFacingLeft_{};
    bool animGraphAimingFromCover_{};
    bool animGraphCoverFallbackCrouchActive_{};
    std::uint64_t previousRemoteCoverTaskMs_{};
    std::uint64_t animGraphCoverAcquireStartedMs_{};
    std::uint8_t animGraphCoverAcquireRetries_{};
    std::uint64_t animGraphCoverMissingSinceMs_{};
    std::uint64_t animGraphPreviousCoverRecoveryMs_{};
    std::uint64_t previousRemoteCoverFallbackAssertMs_{};
    std::uint64_t animGraphCoverTransitions_{};
    std::uint64_t animGraphCoverTaskStarts_{};
    std::uint64_t animGraphCoverTaskCancels_{};
    std::uint64_t animGraphCoverExpectedTicks_{};
    std::uint64_t animGraphCoverObservedTicks_{};
    std::uint64_t animGraphCoverFallbackStarts_{};
    std::uint64_t animGraphCoverReacquires_{};
    std::uint64_t animGraphCoverFallbackRecoveries_{};
    std::array<RemotePlayerActionChannel, 9> remotePlayerActionChannels_{};
    bool reliablePlayerActionProtocolObserved_{};
    bool remoteActionAimActive_{};
    bool remoteActionMeleeActive_{};
    bool remoteActionBlockActive_{};
    bool remoteActionGrappleActive_{};
    bool remoteActionLassoActive_{};
    bool remoteActionKnockdownActive_{};
    bool remoteActionCraftingActive_{};
    std::uint32_t remoteMeleeVisualActionId_{};
    std::uint64_t remoteMeleeVisualDeadlineMs_{};
    std::uint32_t remotePeerDismountActionId_{};
    std::uint64_t remotePeerDismountStartedMs_{};
    std::uint64_t remotePeerDismountLastAttemptMs_{};
    std::uint8_t remotePeerDismountAttempts_{};
    int remotePeerLassoRope_{};
    std::uint32_t remotePeerLassoRopeActionId_{};
    std::uint64_t remotePeerLassoTaskStartedMs_{};
    std::uint8_t remotePeerLassoTaskAttempts_{};
    bool remotePeerLassoTaskPending_{};
    bool remotePeerLassoEngineOwned_{};
    std::array<bool, 8> localLassoOriginalFlags_{};
    bool localLassoFlagsCaptured_{};
    std::uint64_t remotePlayerActionBegins_{};
    std::uint64_t remotePlayerActionSustains_{};
    std::uint64_t remotePlayerActionTerminals_{};
    std::uint64_t remotePlayerActionPreemptions_{};
    std::uint64_t remotePlayerActionStaleRevisions_{};
    std::uint64_t remotePlayerActionNativeCancels_{};
    std::uint64_t remotePlayerActionRopeCreates_{};
    std::uint64_t remotePlayerActionRopeDeletes_{};
    std::uint64_t remotePlayerActionTimeouts_{};
    std::uint64_t remotePlayerActionLassoPending_{};
    std::uint64_t remotePlayerActionLassoConfirmed_{};
    std::uint64_t remotePlayerActionLassoFailed_{};
    std::uint64_t remotePlayerActionVictimFallbacks_{};
    std::uint64_t remotePlayerActionMotorYields_{};
    std::uint64_t remotePlayerActionEpochRejects_{};
    std::uint64_t remotePlayerActionForeignTerminals_{};
    std::uint64_t remotePlayerActionMeleeVisualStarts_{};
    std::uint64_t remotePlayerActionMeleeDeadlineCancels_{};
    std::uint64_t remotePlayerActionMeleeSemanticOnly_{};
    std::uint64_t remotePlayerActionDismountRequests_{};
    std::uint64_t remotePlayerActionDismountRetries_{};
    std::uint64_t remotePlayerActionDismountConfirmed_{};
    std::uint64_t remotePlayerActionDismountFailed_{};
    std::uint64_t remotePlayerActionDiagnosticsStartedMs_{};
    bool localPeerLassoLatched_{};
    bool remotePeerLassoActive_{};
    bool remotePeerKnockdownActive_{};
    std::uint64_t previousPeerLassoRagdollMs_{};
    bool remoteJumping_{};
    bool remoteClimbing_{};
    std::deque<RemoteTraversalIntent> pendingRemoteTraversals_{};
    std::uint64_t remoteTraversalTaskGuardUntilMs_{};
    bool remoteReloading_{};
    std::uint64_t remoteActionDiagnosticsStartedMs_{};
    std::uint64_t remoteActionAimTransitions_{};
    std::uint64_t remoteActionAimTargetUpdates_{};
    std::uint64_t remoteActionFireEvents_{};
    std::uint64_t remoteActionVisualShots_{};
    std::uint64_t remoteActionFireGraphPulses_{};
    std::uint64_t remoteActionFireGraphPulseSuppressed_{};
    std::uint64_t remoteActionFireAmmoRestores_{};
    std::uint64_t remoteActionAudioShots_{};
    std::uint64_t remoteActionAudioNotReady_{};
    std::uint64_t remoteActionEquipmentUpdates_{};
    std::uint64_t remoteActionWeaponGrants_{};
    std::uint64_t remoteActionEquipmentSuppressed_{};
    std::uint64_t remoteActionAimRootSuppressedTicks_{};
    std::uint64_t remoteActionAimRootSuppressionTransitions_{};
    std::uint64_t remoteActionJumpTaskStarts_{};
    std::uint64_t remoteActionClimbTaskStarts_{};
    std::uint64_t remoteActionTraversalDeferred_{};
    std::uint64_t remoteActionTraversalExpired_{};
    std::uint64_t remoteActionTraversalReliableUpdates_{};
    std::uint64_t remoteActionTraversalGeometryConfirmed_{};
    std::uint64_t remoteActionTraversalGeometryFallback_{};
    std::uint64_t remoteWeaponNextGrantMs_{};
    std::uint64_t previousRemoteWeaponVisualMs_{};
    std::uint32_t remoteWeaponGrantAttempts_{};
    bool remoteWeaponConfirmedOwned_{};
    std::uint32_t remoteVisualFireSuppressedWeaponHash_{};
    std::uint32_t remoteVisualFireRestoreAmmo_{};
    std::uint32_t remoteVisualFireRestoreClipAmmo_{};
    std::uint64_t remoteVisualFireRestoreAtMs_{};
    bool remoteVisualFireRestoreClipKnown_{};
    bool remoteGunshotSoundSetReady_{};
    std::uint32_t remoteRelationshipGroup_{};
    std::uint32_t remoteRelationshipLocalGroup_{};
    bool realtimePolicyActive_{};
    bool pauseOverrideActive_{};
    bool synchronizedPauseActive_{};
    bool frontendPauseTogglePending_{};
    bool frontendResumeTogglePending_{};
    bool frontendPauseCycleCompleted_{};
    std::uint64_t frontendPauseToggleStartedMs_{};
    std::uint64_t peerCombatIsolationUntilMs_{};
    bool peerCombatIsolationActive_{};
    PlayerRestraintState authoritativeLocalRestraint_{
        PlayerRestraintState::Free};
    std::uint32_t authoritativeLocalRestraintRevision_{};
    std::uint64_t authoritativeLocalRestraintReceivedMs_{};
    std::uint64_t previousAuthoritativeRestraintRagdollMs_{};
    PlayerRestraintState authoritativeRemoteRestraint_{
        PlayerRestraintState::Free};
    NetEntityId authoritativeRemoteRestraintSubject_{};
    std::uint32_t authoritativeRemoteRestraintRevision_{};
    std::uint64_t authoritativeRemoteRestraintReceivedMs_{};
    std::uint64_t previousAuthoritativeRemoteRestraintRagdollMs_{};
    bool localDownedPolicyActive_{};
    std::uint64_t previousLocalDownedRagdollMs_{};
    std::uint64_t previousRemoteDownedRagdollMs_{};
    std::uint64_t previousRemoteSemanticRagdollMs_{};
    int peerCombatInitialWantedLevel_{};
    bool worldClockWeatherOverrideActive_{};
    bool cutsceneSkipInputActive_{};
    bool missionFlagOverrideActive_{};
    bool missionFlagOverrideRestoreValue_{};
    bool missionFlagOriginalCaptured_{};
    bool missionFlagOriginalValue_{};
    bool guestMissionUnsafeAtAcquire_{};
    bool guestMissionLocalFlagPreviouslyDetected_{};
    bool guestMissionQuarantineActive_{};
    bool guestMissionContaminated_{};
    std::uint64_t guestMissionQuarantineUntilMs_{};
    std::uint64_t guestMissionClearSinceMs_{};
    std::uint64_t guestMissionIsolationDiagnosticsMs_{};
    std::uint64_t guestMissionLocalTransitions_{};
    bool guestMissionInteractionSuppressed_{};
    std::uint64_t guestMissionInteractionSuppressions_{};
    std::uint64_t guestMissionSandboxOverrides_{};
    int guestMissionAnimSceneProbeCursor_{1};
    std::unordered_set<int> guestMissionQuarantinedAnimSceneHandles_{};
    std::uint64_t guestMissionAuthoredSceneSeenUntilMs_{};
    std::uint64_t guestMissionAnimSceneProbeUntilMs_{};
    std::uint64_t guestMissionAnimSceneNextProbeMs_{};
    bool guestHostMissionActive_{};
    bool guestHostPresentationActive_{};
    std::uint64_t guestLocalHazardRecoveries_{};
    bool missionSpectatorActive_{};
    int missionSpectatorCamera_{};
    std::uint64_t missionSpectatorStartedAtMs_{};
    LocalEntityHandle missionSpectatorPed_{};
    LocalEntityHandle missionSpectatorMount_{};
    bool missionSpectatorWasVisible_{true};
    bool missionSpectatorMountWasVisible_{true};
    Vec3 missionSpectatorSavedPosition_{};
    float missionSpectatorSavedHeading_{};
    Vec3 missionResumePosition_{};
    Vec3 missionResumeMountPosition_{};
    Vec3 missionResumeAnchor_{};
    float missionResumeHeading_{};
    std::uint64_t missionResumeRequestedAtMs_{};
    std::uint64_t missionDeferredResumeRetryMs_{};
    bool missionResumePreparing_{};
    bool missionResumePrepared_{};
    bool missionResumeFallbackUsed_{};
    bool missionResumeBarrierActive_{};
    LocalEntityHandle missionResumeBarrierPed_{};
    bool missionDeferredResumePending_{};
    bool missionStreamingFocusActive_{};
    Vec3 missionStreamingFocus_{};
    bool missionReplicatedCameraActive_{};
    int missionCameraSampleSource_{-1};
    std::uint32_t missionReplicatedCameraGeneration_{};
    std::uint32_t missionReplicatedCameraRevision_{};
    std::uint64_t missionReplicatedCameraUpdatedMs_{};
    std::uint64_t missionReplicatedCameraRenderAssertedMs_{};
    Vec3 missionReplicatedCameraPosition_{};
    Vec3 missionReplicatedCameraRotation_{};
    Vec3 missionReplicatedCameraTargetPosition_{};
    Vec3 missionReplicatedCameraTargetRotation_{};
    float missionReplicatedCameraFieldOfView_{55.0F};
    float missionReplicatedCameraTargetFieldOfView_{55.0F};
    int hostAnimSceneHandle_{};
    int hostAnimSceneProbeCursor_{1};
    bool animSceneHybridInspectorEnabled_{};
    bool animSceneHybridInspectorAttempted_{};
    bool animSceneCaptureHostAuthority_{};
    bool animSceneHybridHandlersValidated_{};
    std::array<std::uint8_t*, 7U> animSceneHybridResolvedHandlers_{};
    bool animSceneHybridNativeCreationEnabled_{};
    bool animSceneHybridPreparationWarningLogged_{};
    int ownedHybridAnimSceneHandle_{};
    std::uint32_t ownedHybridAnimSceneDefinitionRevision_{};
    std::uint64_t ownedHybridAnimSceneFingerprintLow_{};
    std::uint64_t ownedHybridAnimSceneFingerprintHigh_{};
    std::uint16_t ownedHybridAnimSceneResolvedRoles_{};
    std::vector<std::string> ownedHybridAnimSceneBoundRoles_{};
    std::vector<LocalEntityHandle> ownedHybridAnimSceneBoundEntities_{};
    bool ownedHybridAnimSceneLoadRequested_{};
    bool ownedHybridAnimSceneStarted_{};
    int hostAnimSceneStartBarrierHandle_{};
    bool hostAnimSceneStartBarrierActive_{};
    int guestAnimSceneHandle_{};
    int guestAnimSceneProbeCursor_{1};
    std::uint32_t guestAnimSceneProbeAttempts_{};
    std::uint32_t guestAnimSceneDictionaryHash_{};
    std::uint32_t guestAnimSceneGeneration_{};
    std::uint64_t guestAnimSceneLastProbeLogMs_{};
    std::uint64_t guestAnimSceneLastCorrectionLogMs_{};
    std::uint64_t guestAnimSceneLastFailureLogMs_{};
    Vec3 missionNativeAnimSceneOrigin_{};
    bool missionNativeAnimSceneActive_{};
    bool missionProxyCastFallbackActive_{};
    bool guestAnimScenePausedByBridge_{};
    bool guestAnimSceneRestartAttempted_{};
    std::uint64_t remoteAppearanceFingerprint_{};
    std::uint32_t remoteAppearanceModelHash_{};
    std::vector<std::uint32_t> remoteAppearanceComponents_{};
    bool missionCompanionPresentationActive_{};
    bool missionCompanionPresentationLive_{};
    int missionObjectiveBlip_{};
    std::uint64_t missionObjectiveBlipRetryMs_{};
    Vec3 missionCompanionTarget_{};
    bool remoteMissionParticipantHidden_{};
    LocalEntityHandle remoteMissionParticipantPed_{};
    LocalEntityHandle remoteMissionParticipantMount_{};
    // Bridge-owned hidden actor shells used only by the host-only dialogue
    // presenter. They are never networked or reused as world replicas.
    std::array<LocalEntityHandle, 2U> hostMissionDialogueProxyPeds_{};
    std::string hostMissionDialogueRoot_{};
    bool remoteMissionParticipantWasVisible_{true};
    bool remoteMissionParticipantMountWasVisible_{true};
    EntityRegistry worldEntityReplicas_{};
    std::unordered_map<NetEntityId, WorldProxyEntry, NetEntityIdHash>
        worldProxyEntries_{};
    std::unordered_map<LocalEntityHandle, HiddenAmbientEntry>
        hiddenAmbientPeds_{};
    std::unordered_map<LocalEntityHandle, HiddenAmbientEntry>
        hiddenAmbientAttachments_{};
    std::uint64_t previousWorldMirrorMaintainMs_{};
    std::uint64_t previousWorldMirrorDiagnosticsMs_{};
    std::uint64_t previousWorldSampleDiagnosticsMs_{};
    std::uint64_t previousWorldDamageIntentMs_{};
    std::uint64_t previousPickupObservationMs_{};
    std::uint64_t previousScriptEventObservationMs_{};
    std::unordered_map<std::uint32_t, bool>
        observedCampaignWeaponOwnership_{};
    std::vector<CampaignCapabilityObservation>
        pendingCampaignCapabilityObservations_{};
    std::unordered_map<int, std::uint64_t> observedVanillaPickups_{};
    std::vector<VanillaPickupCollection> pendingVanillaPickupCollections_{};
    std::uint32_t worldDamageShotSequence_{};
    bool worldMirrorGuestActive_{};
    MotionReplicationWireMode motionReplicationMode_{
        MotionReplicationWireMode::TaskNavmesh};
    std::uint16_t motionReplicationFlags_{};
    std::uint32_t motionReplicationRevision_{};
    std::optional<PlayerAnimationStatePayload>
        latestRemoteAnimationState_{};
    std::optional<std::uint64_t>
        latestRemoteAnimationStateReceivedAtMs_{};
    NetEntityId animGraphReplicaPreparedEntity_{};
    std::uint32_t lastRemoteAnimationStateHash_{};
    std::uint32_t lastRemoteAnimationGraphHash_{};
    RemoteLocomotion animGraphVisualLocomotion_{
        RemoteLocomotion::Idle};
    RemoteMovementDirection animGraphVisualDirection_{
        RemoteMovementDirection::None};
    Vec3 animGraphVisualTaskDestination_{};
    float animGraphVisualTaskHeading_{};
    std::uint64_t animGraphVisualTaskStartedMs_{};
    bool animGraphVisualTaskActive_{};
    std::uint64_t animGraphReplicaTicks_{};
    std::uint64_t animGraphReplicaCorrections_{};
    std::uint64_t animGraphReplicaStateApplies_{};
    std::uint64_t animGraphVisualTaskStarts_{};
    std::uint64_t animGraphVisualTaskRefreshes_{};
    std::uint64_t animGraphDirectionTransitions_{};
    std::uint64_t animGraphTurnInPlaceTaskStarts_{};
    std::uint64_t animGraphIkPreparations_{};
    std::uint64_t animGraphExpectedMovingTicks_{};
    std::uint64_t animGraphObservedMovingTicks_{};
    std::uint64_t animGraphMissingLocomotionTicks_{};
    std::uint64_t animGraphMissingLocomotionSinceMs_{};
    std::uint64_t animGraphPreviousLocomotionRecoveryMs_{};
    std::uint64_t animGraphLocomotionRecoveries_{};
    PlayerLocomotionMode animGraphPreviousLocomotionMode_{
        PlayerLocomotionMode::Grounded};
    bool animGraphLocomotionModeInitialized_{};
    std::uint64_t animGraphTraversalExpectedTicks_{};
    std::uint64_t animGraphTraversalObservedTicks_{};
    std::uint64_t animGraphTraversalMissingTicks_{};
    std::uint64_t animGraphTraversalJumpTaskStarts_{};
    std::uint64_t animGraphTraversalClimbTaskStarts_{};
    std::uint64_t animGraphAirborneLaunches_{};
    std::uint64_t animGraphPhysicalRootYieldTicks_{};
    std::uint64_t animGraphPhysicalRootLeashCorrections_{};
    std::uint64_t animGraphRagdollTaskStarts_{};
    std::uint64_t animGraphStealthExpectedTicks_{};
    std::uint64_t animGraphStealthObservedTicks_{};
    std::uint64_t animGraphStealthTransitions_{};
    std::uint64_t animGraphStealthMissingSinceMs_{};
    std::uint64_t animGraphPreviousStealthRecoveryMs_{};
    std::uint64_t animGraphStealthRecoveries_{};
    bool animGraphStealthActive_{};
    std::uint64_t animGraphWaterExpectedTicks_{};
    std::uint64_t animGraphWaterObservedTicks_{};
    std::uint64_t animGraphSwimmingExpectedTicks_{};
    std::uint64_t animGraphSwimmingObservedTicks_{};
    std::uint64_t animGraphMeleeExpectedTicks_{};
    std::uint64_t animGraphMeleeTaskStarts_{};
    std::uint64_t animGraphMeleeMissingTargetTicks_{};
    std::uint64_t animGraphMeleeTaskCancels_{};
    std::uint64_t animGraphAimExpectedTicks_{};
    std::uint64_t animGraphAimIdleTaskStarts_{};
    std::uint64_t animGraphAimMovingTaskStarts_{};
    std::uint64_t animGraphAimTaskCancels_{};
    Vec3 animGraphAimTaskDestination_{};
    std::uint64_t animGraphTraversalExpired_{};
    std::uint64_t animGraphReplicaMoveNetworkSamples_{};
    std::uint64_t animGraphReplicaUnavailableSamples_{};
    std::uint64_t animGraphReplicaDiagnosticsStartedMs_{};
    double animGraphReplicaPositionErrorSum_{};
    float animGraphReplicaPositionErrorMax_{};
};

}  // namespace coopstory::bridge::sdk
