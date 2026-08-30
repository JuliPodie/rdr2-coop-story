#pragma once

#include "coopstory/bridge/IScriptHookFacade.hpp"
#include "coopstory/bridge/MissionBubble.hpp"
#include "coopstory/bridge/PlayerRuntime.hpp"
#include "coopstory/bridge/RemoteMotion.hpp"
#include "coopstory/bridge/SessionMenuController.hpp"
#include "coopstory/bridge/Telemetry.hpp"
#include "coopstory/bridge/Transport.hpp"
#include "coopstory/bridge/WorldMirror.hpp"

#include <cstdint>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace coopstory::bridge {

struct BridgeRuntimeConfig final {
    MissionBubbleOverLimitAction bubbleAction{
        MissionBubbleOverLimitAction::TeleportGuest};
};

struct LocalPlayerActionRuntime final {
    std::uint32_t actionId{};
    std::uint16_t revision{};
    std::uint64_t startedAtMs{};
    std::uint64_t lastSentAtMs{};
    NetEntityId targetEntityId{};
    Vec3 targetPoint{};
    std::uint32_t flags{};
    std::uint32_t weaponHash{};
    std::uint32_t variantHash{};
    bool active{};
};

struct LocalInteractionRuntime final {
    std::uint32_t interactionId{};
    std::uint16_t revision{};
    std::uint64_t lastSentAtMs{};
    std::uint64_t startedAtMs{};
    NetEntityId targetEntityId{};
    NetEntityId secondaryEntityId{};
    InteractionKind kind{InteractionKind::None};
    bool active{};
};

class BridgeRuntime final {
public:
    BridgeRuntime(
        IScriptHookFacade& facade,
        IFrameTransport& transport,
        BridgeRuntimeConfig config = {});

    [[nodiscard]] bool Start(const GameIdentity& identity, std::string& error);
    void Tick();
    void Stop(std::string_view reason = "bridge stopped") noexcept;
    void AbortAfterNativeException() noexcept;
    [[nodiscard]] std::string_view LastTickStage() const noexcept {
        return lastTickStage_;
    }

    [[nodiscard]] bool IsActive() const noexcept { return active_; }
    [[nodiscard]] bool ShouldUnload() const noexcept { return shouldUnload_; }
    [[nodiscard]] std::optional<PlayerSlot> LocalSlot() const noexcept {
        return localSlot_;
    }
    [[nodiscard]] NetEntityId LocalEntityId() const noexcept {
        return localEntityId_;
    }
    [[nodiscard]] bool RemoteConnected() const noexcept {
        return remoteReplicaId_.IsValid();
    }
    [[nodiscard]] CoopPlayerStateMachine& Players() noexcept { return players_; }

private:
    void SendHello(bool reconnect = false);
    void AcceptHelloAck(std::span<const std::uint8_t> payload);
    [[nodiscard]] GuestMissionIsolationStatus
    AcquireGuestMissionIsolationLease(std::string_view reason);
    void ReleaseGuestMissionIsolationLease(
        std::string_view reason) noexcept;
    void ApplyRemotePlayerState(
        const PlayerStatePayload& state,
        std::uint32_t sequence,
        std::uint64_t senderTickMs);
    void DespawnRemoteReplica() noexcept;
    void HandleMenuCommand(BridgeCommand command);
    void HandleTeleportToPlayerRequest();
    void HandleTeleportGuestRequest();
    void HandleInboundTeleportGuest(const CommandPayload& command);
    void ArmTeleportVerification(
        const Vec3& destination,
        std::uint64_t requestedAtMs) noexcept;
    void VerifyPendingTeleport(std::uint64_t nowMs);
    void HandleSessionOverlayAction(SessionOverlayAction action);
    void HandleInboundFrame(const Frame& frame);
    void TickWorldMirror(
        std::uint64_t nowMs,
        const std::optional<LocalPlayerSample>& localSample,
        bool remoteStreaming);
    bool SendWorldMirrorSignal(const WorldMirrorSignal& signal);
    bool FlushPendingHostWorldDespawns();
    void ResetHostWorldMirror(bool notifyPeer);
    void ResetGuestWorldMirror(
        bool preserveSequenceTombstones = false) noexcept;
    void HandleEntitySpawn(
        const Frame& frame,
        const WorldEntityStatePayload& state);
    void HandleEntityUpdate(
        const Frame& frame,
        const WorldEntityStatePayload& state);
    void HandleEntityDespawn(
        const Frame& frame,
        NetEntityId entityId);
    void ApplyGuestWorldGraphSignals(
        std::span<const WorldMirrorSignal> signals);
    void HandleDamageIntent(const DamageIntentPayload& intent);
    void HandleRemoteMountState(
        const Frame& frame,
        const PlayerMountStatePayload& state);
    void TickLocalPlayerActions(
        const LocalPlayerSample& sample,
        PlayerSlot localSlot,
        std::uint64_t nowMs);
    void TickLocalInteractions(
        const LocalPlayerSample& sample,
        PlayerSlot localSlot,
        std::uint64_t nowMs);
    void HandleInteractionResult(
        const InteractionResultPayload& result);
    void HandleRestraintState(
        const RestraintStatePayload& state);
    [[nodiscard]] bool ApplyMatchedRestraintState(
        const RestraintStatePayload& state);
    void CachePendingRestraintState(
        const RestraintStatePayload& state);
    void ApplyPendingRestraintState(NetEntityId subjectEntityId);
    void TickMissionAuthority(
        const LocalPlayerSample& sample,
        PlayerSlot localSlot,
        std::uint64_t nowMs,
        bool checkpointRespawnConfirmed);
    void TickMissionLoadingAuthority(std::uint64_t nowMs);
    void HandleRemoteMissionState(
        const Frame& frame,
        const MissionStatePayload& state);
    void TickMissionProgression(
        const LocalPlayerSample& sample,
        std::uint64_t nowMs);
    void TickMissionObjective(
        const LocalPlayerSample& sample,
        std::uint64_t nowMs);
    void HandleRemoteMissionObjective(
        const Frame& frame,
        const MissionObjectivePayload& objective);
    void TickMissionDialogue(PlayerSlot localSlot, std::uint64_t nowMs);
    void HandleRemoteMissionDialogueCue(
        const Frame& frame,
        const MissionDialogueCuePayload& cue);
    void HandleRemoteMissionDialogueReady(
        const Frame& frame,
        const MissionDialogueReadyPayload& ready);
    void SendMissionDialogueReady(
        const MissionDialogueCuePayload& cue,
        MissionDialogueReadyState state);
    void TickAmbientEncounter(
        const LocalPlayerSample& sample,
        PlayerSlot localSlot,
        std::uint64_t nowMs);
    void HandleRemoteAmbientEncounterProposal(
        const Frame& frame,
        const AmbientEncounterProposalPayload& proposal);
    void HandleRemoteAmbientEncounterState(
        const Frame& frame,
        const AmbientEncounterStatePayload& state);
    void StartPreparedAmbientEncounter(std::uint64_t nowMs);
    void PublishAmbientEncounterState(
        const AmbientEncounterInstance& instance,
        std::uint64_t nowMs);
    void HandleRemoteMissionProgression(
        const MissionProgressionPayload& payload);
    void TickMissionCinematic(
        const std::optional<LocalPlayerSample>& sample,
        PlayerSlot localSlot,
        std::uint64_t nowMs);
    void PublishMissionCinematicState(
        MissionCinematicPhase phase,
        std::uint16_t flags,
        const Vec3& anchor,
        float heading,
        std::uint64_t nowMs);
    void HandleRemoteMissionCinematicState(
        const Frame& frame,
        const MissionCinematicStatePayload& state);
    void HandleRemoteMissionCinematicAction(
        const Frame& frame,
        const MissionCinematicActionPayload& action);
    void SendMissionCinematicAction(
        MissionCinematicActionKind kind,
        std::uint16_t flags,
        std::uint64_t nowMs);
    void RequestCutsceneSkip(std::uint64_t nowMs);
    void TryCommitCutsceneSkip(std::uint64_t nowMs);
    void TickMissionCamera(
        PlayerSlot localSlot,
        std::uint64_t nowMs);
    void HandleRemoteMissionCamera(
        const Frame& frame,
        const MissionCameraStatePayload& state);
    void HandleRemoteAnimSceneReplicaState(
        const Frame& frame,
        const AnimSceneReplicaStatePayload& state);
    void TickAnimSceneHybridDefinition(
        PlayerSlot localSlot,
        const std::optional<LocalPlayerSample>& localSample,
        std::uint64_t nowMs);
    void HandleRemoteAnimSceneDefinition(
        const Frame& frame,
        const AnimSceneDefinitionPayload& definition);
    void PollRemoteAnimSceneDefinitionPreparation(std::uint64_t nowMs);
    void HandleRemoteAnimSceneControl(
        const Frame& frame,
        const AnimSceneControlPayload& control);
    void CommitPreparedGuestAnimSceneDefinition(
        const AnimSceneControlPayload& control);
    [[nodiscard]] bool SendAnimSceneControl(
        AnimSceneControlKind kind,
        AnimSceneControlReason reason,
        std::uint32_t flags,
        std::uint64_t playAtHostTick,
        float startPhase,
        float rate,
        std::uint64_t nowMs);
    void TryCommitHostAnimSceneDefinition(std::uint64_t nowMs);
    [[nodiscard]] bool HasActiveHostAnimSceneDefinition() const noexcept;
    [[nodiscard]] bool BeginHostAnimScenePrepareAttempt(
        std::string_view reason) noexcept;
    [[nodiscard]] bool SetHostAnimSceneStartBarrier(
        bool active,
        std::string_view reason) noexcept;
    void ResetAnimSceneHybridState(
        bool abortFacade,
        bool preserveHostDefinition = false) noexcept;
    void MaintainMissionPresentation(
        const std::optional<LocalPlayerSample>& localSample,
        bool remoteStreaming,
        std::uint64_t nowMs);
    void EmitRuntimeDiagnostics(
        bool remoteStreaming,
        std::uint64_t nowMs);
    void BeginProblemDiagnosticBurst(
        std::uint64_t correlationId,
        bool remoteOrigin,
        std::uint64_t nowMs) noexcept;
    void EmitProblemDiagnosticSnapshot(
        const std::optional<LocalPlayerSample>& localSample,
        bool remoteStreaming,
        std::uint64_t nowMs);
    [[nodiscard]] bool IsMissionWorldMirrorSafe() const noexcept;
    void HandleRemotePlayerAction(
        const Frame& frame,
        const PlayerActionPayload& action);
    void HandleLocalPauseToggle();
    void HandlePauseVote(
        const Frame& frame,
        const PauseVotePayload& payload);
    void PublishPauseVoteState();
    void ResetPauseVoteState(bool notifyPeer) noexcept;
    void HandlePlayerSignals(std::span<const PlayerRuntimeSignal> signals);
    void SendLifecycleState(
        MessageType type,
        PlayerSlot subject,
        PlayerLifecycle lifecycle,
        float healthFraction = 0.0F);
    void SendReviveCompleted(PlayerSlot target, float healthFraction);
    [[nodiscard]] std::optional<PlayerSlot> FindSlot(NetEntityId id) const noexcept;
    bool SendBestEffort(Frame frame);

    IScriptHookFacade& facade_;
    IFrameTransport& transport_;
    BridgeRuntimeConfig config_;
    MissionBubbleController bubble_;
    CoopPlayerStateMachine players_{};
    MenuController menu_{};
    SessionMenuController sessionMenu_{};
    TelemetryScheduler telemetry_{};
    FrameSequencer sequencer_{};
    SequenceWindow remotePlayerSequences_{};
    SequenceWindow remoteAnimationSequences_{};
    SequenceWindow remoteAnimationPayloadSequences_{};
    RemoteSnapshotBuffer remoteSnapshots_{};
    std::optional<WorldMirrorHost> worldMirrorHost_{};
    std::unordered_map<NetEntityId, bool, NetEntityIdHash>
        pendingHostWorldDespawns_{};
    WorldMirrorGuestGraph guestWorldGraph_{};
    std::array<NetEntityId, 2> playerEntityIds_{};
    std::optional<PlayerSlot> localSlot_{};
    std::optional<PlayerSlot> helloExpectedRole_{};
    NetEntityId localEntityId_{};
    std::optional<ReviveAttempt> pendingRevive_{};
    NetEntityId remoteReplicaId_{};
    std::optional<PlayerStatePayload> latestRemoteState_{};
    std::optional<PlayerAnimationStatePayload>
        latestRemoteAnimationState_{};
    std::optional<std::uint64_t>
        latestRemoteAnimationReceivedAtMs_{};
    std::uint64_t latestRemoteAnimationSenderTickMs_{};
    std::optional<PlayerIdentityPayload> remoteIdentity_{};
    std::optional<PlayerAppearanceStatePayload> remoteAppearance_{};
    std::optional<EquipmentStatePayload> remoteEquipment_{};
    std::optional<PlayerMountStatePayload> remoteMountState_{};
    std::optional<PlayerMountStatePayload> pendingRemoteMountAbsentState_{};
    std::optional<PlayerMountStatePayload> lastLocalMountState_{};
    std::optional<EquipmentStatePayload> lastLocalEquipment_{};
    std::optional<PlayerAppearanceStatePayload> lastLocalAppearance_{};
    std::optional<PlayerLifecycle> remoteLifecycle_{};
    std::optional<PlayerLifecycle> previousLocalLifecycle_{};
    std::uint64_t reviveRequestExpiresMs_{};
    std::uint64_t lastWorldDamageIntentMs_{};
    SequenceWindow worldDamageIntentSequences_{};
    SequenceWindow remoteMountSequences_{};
    SequenceWindow remotePlayerActionSequences_{};
    SequenceWindow interactionResultSequences_{};
    SequenceWindow restraintStateSequences_{};
    SequenceWindow remoteMissionSequences_{};
    SequenceWindow remoteMissionDialogueCueSequences_{};
    SequenceWindow remoteMissionDialogueReadySequences_{};
    SequenceWindow remoteAmbientEncounterProposalSequences_{};
    SequenceWindow remoteAmbientEncounterStateSequences_{};
    SequenceWindow remoteMissionCameraSequences_{};
    SequenceWindow remoteMissionCinematicSequences_{};
    SequenceWindow remoteMissionCinematicActionSequences_{};
    SequenceWindow remoteAnimSceneSequences_{};
    SequenceWindow remoteAnimSceneDefinitionSequences_{};
    SequenceWindow remoteAnimSceneControlSequences_{};
    SequenceWindow pauseVoteSequences_{};
    std::uint32_t localFireSequence_{};
    std::uint32_t localAnimationSampleSequence_{};
    std::uint32_t localPlayerActionSequence_{};
    std::uint32_t localPlayerActionId_{};
    std::array<LocalPlayerActionRuntime, 9> localPlayerActions_{};
    LocalInteractionRuntime localInteraction_{};
    std::uint32_t localInteractionId_{};
    std::uint64_t localCapabilityEventId_{};
    std::optional<RestraintStatePayload> remoteRestraintState_{};
    std::optional<RestraintStatePayload> localRestraintState_{};
    std::unordered_map<
        NetEntityId,
        RestraintStatePayload,
        NetEntityIdHash>
        pendingRestraintStates_{};
    std::optional<MissionStatePayload> localMissionState_{};
    std::optional<MissionStatePayload> remoteMissionState_{};
    std::optional<MissionObjectivePayload> localMissionObjective_{};
    std::optional<MissionObjectivePayload> remoteMissionObjective_{};
    std::optional<MissionDialogueCuePayload> localMissionDialogueCue_{};
    std::optional<MissionDialogueCuePayload> remoteMissionDialogueCue_{};
    std::optional<MissionDialogueCuePayload> pendingHostMissionDialogueCue_{};
    std::optional<MissionDialogueReadyPayload> remoteMissionDialogueReady_{};
    AmbientEncounterCoordinator ambientEncounterCoordinator_{};
    std::optional<AmbientEncounterStatePayload> remoteAmbientEncounter_{};
    std::uint64_t localAmbientEncounterProposalId_{};
    std::uint64_t localAmbientEncounterProposalExpiresMs_{};
    std::uint64_t localAmbientEncounterInstanceId_{};
    std::uint64_t localAmbientEncounterTerminalAtMs_{};
    std::uint64_t remoteAmbientEncounterTerminalAtMs_{};
    std::uint64_t localExactEncounterPreflightDeadlineMs_{};
    std::uint64_t nextExactEncounterPreflightPublishMs_{};
    std::uint64_t remoteExactEncounterPreflightInstanceId_{};
    std::uint32_t localMissionDialogueSequence_{};
    std::uint64_t lastMissionDialogueCueSentMs_{};
    std::uint64_t pendingHostMissionDialogueDueMs_{};
    std::uint32_t localMissionObjectiveRevision_{};
    std::uint64_t nextMissionObjectiveSampleMs_{};
    std::optional<MissionProgressionPayload> localMissionProgressionOffer_{};
    // A mission completion can carry persistent save changes. Retain it for
    // bounded retransmission until the guest explicitly acknowledges that
    // its local transaction completed.
    std::optional<MissionProgressionPayload> localMissionProgressionCompletion_{};
    std::optional<MissionProgressionPayload> remoteMissionProgressionOffer_{};
    // The host opens this only after the guest has confirmed the exact
    // MissionData entry. It is a bounded permission to use the guest's own
    // vanilla prompt, never a general Story-mode exemption.
    std::optional<MissionProgressionPayload> localMissionStartBarrier_{};
    std::optional<MissionProgressionPayload> remoteMissionStartBarrier_{};
    std::uint64_t localMissionStartBarrierDeadlineMs_{};
    std::uint64_t remoteMissionStartBarrierDeadlineMs_{};
    // A guest barrier starts guarded. The short verified-idle interval
    // distinguishes a fresh host-authorized prompt interaction from a local
    // mission that was already entering when the network barrier arrived.
    std::uint64_t remoteMissionStartBarrierPromptArmedAtMs_{};
    bool localMissionStartBarrierGuestStarted_{};
    bool remoteMissionStartBarrierGuestStarted_{};
    bool remoteMissionStartBarrierReleased_{};
    bool remoteMissionStartBarrierRejected_{};
    // Set only after the host releases an exact matching local mission
    // instance. Eligibility alone never authorizes a save mutation.
    bool remoteMissionProgressionParticipated_{};
    std::uint64_t nextGuestMissionInstanceStartedRetryMs_{};
    // This is calculated locally when the guest receives the offer.  A host
    // completion frame may never substitute for that local-save evidence.
    bool remoteMissionProgressionEligible_{};
    // Completion frames are reliable enough to be retransmitted across a
    // reconnect.  Keep their effects exactly-once even after a future native
    // mapping is enabled.
    std::optional<std::uint64_t> remoteMissionProgressionAppliedEventId_{};
    std::uint32_t localProgressionMissionId_{};
    bool guestMissionProgressionEligible_{};
    bool guestMissionProgressionCompletionAcknowledged_{};
    std::uint64_t nextMissionProgressionCompletionRetryMs_{};
    std::optional<MissionCinematicStatePayload>
        localMissionCinematicState_{};
    std::optional<MissionCinematicStatePayload>
        remoteMissionCinematicState_{};
    std::optional<MissionCameraStatePayload> remoteMissionCameraState_{};
    std::optional<AnimSceneReplicaStatePayload> remoteAnimSceneState_{};
    std::optional<AnimSceneReplicaStatePayload> lastLocalAnimSceneState_{};
    std::optional<AnimSceneDefinitionPayload> localAnimSceneDefinition_{};
    std::optional<AnimSceneDefinitionPayload> remoteAnimSceneDefinition_{};
    std::optional<AnimSceneControlPayload> pendingRemoteAnimSceneCommit_{};
    std::optional<std::uint64_t> remoteMissionCameraReceivedAtMs_{};
    std::optional<std::uint64_t> remoteMissionCinematicReceivedAtMs_{};
    std::optional<std::uint64_t> remoteAnimSceneReceivedAtMs_{};
    std::uint32_t previousLocalWeaponHash_{};
    std::uint32_t previousLocalWeaponAmmo_{};
    std::uint32_t localMountGeneration_{1U};
    std::uint32_t previousLocalMountModelHash_{};
    LocalEntityHandle previousLocalMountHandle_{};
    std::uint64_t lastLocalOwnedMountPresentMs_{};
    Vec3 localShotAimTarget_{};
    Vec3 localTraversalAnchor_{};
    Vec3 localTraversalApproachVelocity_{};
    Vec3 localTraversalObstaclePoint_{};
    Vec3 localTraversalObstacleNormal_{};
    Vec3 localTraversalExpectedLanding_{};
    Vec3 previousLocalPosition_{};
    float localTraversalHeading_{};
    float localTraversalObstacleTopZ_{};
    float previousLocalHeading_{};
    std::uint64_t localShotLatchExpiresMs_{};
    std::uint64_t nextMissionStateHeartbeatMs_{};
    std::uint64_t nextMissionCinematicHeartbeatMs_{};
    std::uint64_t nextMissionCameraSampleMs_{};
    std::uint64_t nextAnimSceneSampleMs_{};
    std::uint64_t nextRemoteMountMaintainMs_{};
    std::uint64_t nextMissionWorldWarningMs_{};
    std::uint64_t localMissionRecoveryUntilMs_{};
    std::uint64_t localSampleUnavailableSinceMs_{};
    std::uint64_t localCinematicControlRecoveredSinceMs_{};
    std::uint64_t localCinematicPrepareStartedMs_{};
    std::uint64_t localCinematicTerminalClearSinceMs_{};
    std::uint64_t localCinematicCameraWaitStartedMs_{};
    bool localCinematicResumeWaitWarned_{};
    bool localCinematicCameraWaitWarned_{};
    std::string_view lastTickStage_{"not-started"};
    std::uint64_t localCutsceneSkipUntilMs_{};
    std::uint64_t localCutsceneSkipVoteUntilMs_{};
    std::uint64_t remoteCutsceneSkipVoteUntilMs_{};
    std::uint64_t guestQuarantineSkipUntilMs_{};
    std::uint64_t guestPostCinematicSkipUntilMs_{};
    bool localCinematicTerminalLatchActive_{};
    std::uint64_t localTraversalLatchExpiresMs_{};
    std::uint16_t localLocomotionEpoch_{1U};
    std::uint16_t localTraversalActionId_{};
    std::uint16_t localTraversalRevision_{};
    std::uint16_t localTraversalSentRevision_{};
    std::uint32_t localMissionEpoch_{1U};
    std::uint32_t localMissionRevision_{1U};
    std::uint32_t localMissionCinematicGeneration_{};
    std::uint32_t localMissionCinematicRevision_{};
    std::uint32_t localMissionCinematicActionId_{};
    std::uint32_t remoteMissionCinematicActionId_{};
    std::uint32_t localMissionCameraRevision_{1U};
    std::uint32_t localAnimSceneRevision_{1U};
    std::uint32_t localAnimSceneDefinitionRevision_{1U};
    std::uint32_t localAnimSceneControlActionId_{};
    std::uint32_t remoteAnimSceneControlActionId_{};
    std::uint64_t lastCapturedAnimSceneSequence_{};
    std::uint64_t localAnimSceneDefinitionSentAtMs_{};
    std::uint64_t localAnimSceneGuestReadyAtMs_{};
    std::uint64_t remoteAnimSceneDefinitionReceivedAtMs_{};
    std::uint64_t remoteAnimSceneReadySentAtMs_{};
    std::uint64_t nextRemoteAnimScenePrepareDiagnosticsMs_{};
    std::uint32_t localAppearanceRevision_{1U};
    std::uint32_t localCheckpointGeneration_{1U};
    PlayerTraversalKind localTraversalKind_{PlayerTraversalKind::None};
    std::uint32_t localTraversalFlags_{};
    bool previousLocalFiring_{};
    bool previousLocalJumping_{};
    bool previousLocalClimbing_{};
    bool localTraversalBecameActive_{};
    bool localTraversalLandingCaptured_{};
    bool previousLocalMounted_{};
    bool previousLocalMissionActive_{};
    bool localMissionInitialized_{};
    bool localMissionCameraActive_{};
    bool localAnimSceneActive_{};
    bool remoteNativeAnimSceneActive_{};
    bool localAnimSceneGuestReady_{};
    bool localAnimSceneCommitSent_{};
    bool localAnimSceneDefinitionTimedOut_{};
    bool localAnimSceneStartBarrierActive_{};
    bool remoteAnimSceneDefinitionPrepared_{};
    bool remoteAnimSceneDefinitionCommitted_{};
    bool remoteAnimSceneDefinitionResponseSent_{};
    ReplicatedAnimScenePrepareStage remoteAnimScenePrepareStage_{
        ReplicatedAnimScenePrepareStage::None};
    std::uint16_t remoteAnimScenePrepareResolvedRoles_{};
    std::uint16_t remoteAnimScenePrepareRequiredRoles_{};
    NetEntityId remoteAnimScenePreparePendingEntityId_{};
    std::string remoteAnimScenePreparePendingRoleName_{};
    bool hostAnimSceneReconnectPending_{};
    bool awaitingRestoredGuestStream_{};
    bool forceHostWorldMirrorReplay_{};
    bool hostWorldReplayAwaitingGuest_{};
    std::uint64_t hostWorldReplayGuestDeadlineMs_{};
    bool localCinematicCameraReady_{};
    bool localCinematicPresentationReady_{};
    bool localCinematicResumeReady_{};
    bool remoteCinematicPresentationReadySent_{};
    bool remoteCinematicResumeReadySent_{};
    bool remoteCinematicResumeFallbackUsed_{};
    bool previousGuestLocalMissionDetected_{};
    bool guestMissionQuarantineActive_{};
    bool guestMissionQuarantineSkipLatched_{};
    bool remoteParticipantSceneIsolated_{};
    bool hasPreviousLocalTransform_{};
    bool previousQuickMarkerPressed_{};
    bool previousEscapePressed_{};
    bool hasPreviousLocalWeaponSample_{};
    bool active_{};
    bool shouldUnload_{};
    bool diagnostics_{};
    bool soloOverride_{};
    bool cutsceneSpectator_{};
    bool synchronizedPaused_{};
    bool pauseStateChangedByRemoteThisTick_{};
    bool hostPauseVoted_{};
    bool guestPauseVoted_{};
    bool previousRemoteStreaming_{};
    std::uint32_t pauseVoteGeneration_{};
    std::uint32_t userProblemMarkerId_{};
    std::uint64_t notificationUntilMs_{};
    std::string notificationText_{};
    std::uint64_t problemDiagnosticCorrelationId_{};
    std::uint64_t problemDiagnosticUntilMs_{};
    std::uint64_t nextProblemDiagnosticSnapshotMs_{};
    std::uint32_t problemDiagnosticSampleIndex_{};
    bool problemDiagnosticRemoteOrigin_{};
    std::optional<std::uint64_t> localRespawnCandidateSinceMs_{};
    std::uint64_t previousTickMs_{};
    std::uint64_t nextReconnectMs_{};
    std::uint64_t nextWorldStateMs_{};
    std::uint64_t nextWorldMirrorSampleMs_{};
    std::uint64_t nextWorldGraphDiagnosticsMs_{};
    std::uint64_t nextEquipmentRefreshMs_{};
    std::uint64_t nextAppearanceRefreshMs_{};
    std::uint64_t nextMotionDiagnosticsMs_{};
    std::uint64_t nextMissionIsolationDiagnosticsMs_{};
    // A short entry debounce rejects incidental control suppression (such as
    // the interaction frontend), while the release hold prevents one-frame
    // control/UI transitions from toggling guest spectator repeatedly.
    std::optional<std::uint64_t> spectatorClassifierCandidateSinceMs_{};
    std::uint64_t spectatorClassifierReleaseUntilMs_{};
    std::uint64_t nextRuntimeDiagnosticsMs_{};
    std::uint64_t runtimeDiagnosticTickCount_{};
    std::uint64_t runtimeDiagnosticTickElapsedSumMs_{};
    std::uint64_t runtimeDiagnosticHitchCount_{};
    std::uint32_t runtimeDiagnosticTickMaximumMs_{};
    std::uint64_t remoteMountAbsentSinceMs_{};
    std::array<std::uint64_t, 4> motionSampleCounts_{};
    std::uint64_t motionApplyFailures_{};
    std::uint64_t motionSourceAgeMaximumMs_{};
    std::uint64_t motionArrivalGapMaximumMs_{};
    std::uint32_t consecutiveMotionApplyFailures_{};
    std::optional<std::uint64_t> lastRemoteStateMs_{};
    std::optional<std::uint64_t>
        lastRemotePlayerActionReceivedAtMs_{};
    std::optional<Vec3> pendingTeleportDestination_{};
    std::optional<CommandPayload> pendingMissionGuestTeleport_{};
    std::uint64_t pendingTeleportRequestedAtMs_{};
    bool hostWorldMirrorActive_{};
    bool guestWorldMirrorActive_{};
    // SOLO TEST only: accepts the looped-back, offset host graph through the
    // normal guest proxy renderer so one PC can switch between host and guest
    // population layers. It is gated by the SyntheticTest player flag.
    bool soloGuestWorldViewEnabled_{};
    bool guestWorldAuthorityConfirmed_{};
    // This lease starts before a JOIN request leaves the bridge and survives
    // transport reconnects, spectator windows and Solo override. Releasing it
    // any earlier would briefly reopen the guest's process-local Story VM.
    bool guestMissionIsolationLeaseActive_{};
    bool remoteMountAbsenceConfirmed_{};
    std::string hostInviteCode_{};
    std::uint32_t sessionEpoch_{};
    MotionReplicationWireMode motionReplicationMode_{
        MotionReplicationWireMode::TaskNavmesh};
    std::uint16_t motionReplicationFlags_{};
    std::uint32_t motionReplicationRevision_{};
    std::uint64_t animationSamplesSent_{};
    std::uint64_t animationSamplesReceived_{};
    std::uint64_t animationSamplesRejected_{};
    std::uint64_t animationSamplesExpired_{};
    std::uint64_t playerActionsSent_{};
    std::uint64_t playerActionsReceived_{};
    std::uint64_t playerActionsDuplicate_{};
    std::uint64_t playerActionsStale_{};
    std::uint64_t playerActionsRejected_{};
    std::uint64_t guestMissionIsolationDetections_{};
    std::uint64_t guestMissionQuarantineTicks_{};
    std::uint64_t remoteParticipantIsolationTransitions_{};
    std::string lastMissionTimelineState_{};
    bool playerDivergenceStateInitialized_{};
    bool playerDivergenceActive_{};
    bool entityDivergenceStateInitialized_{};
    bool entityDivergenceActive_{};
};

}  // namespace coopstory::bridge
