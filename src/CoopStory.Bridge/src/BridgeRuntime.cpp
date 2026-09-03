#include "coopstory/bridge/BridgeRuntime.hpp"
#include "coopstory/bridge/BuildInfo.hpp"

#include "coopstory/bridge/CampaignMissionCatalog.hpp"
#include "coopstory/bridge/BridgeOwnedEncounterCatalog.hpp"
#include "coopstory/bridge/ExactEncounterCatalog.hpp"

#include <chrono>
#include "coopstory/bridge/RemoteMotion.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace coopstory::bridge {
namespace {

// If we have not heard from the other player for one second, do not trust their old position or let it control NPC/game actions.
constexpr std::uint64_t kRemotePlayerFreshnessMilliseconds = 1'000U;
constexpr std::uint64_t kRemotePlayerDespawnTimeoutMilliseconds = 5'000U;
constexpr std::uint64_t kWorldStateIntervalMilliseconds = 500U;
constexpr std::uint64_t kEquipmentRefreshMilliseconds = 1'000U;
constexpr std::uint64_t kMissionProgressionCompletionRetryMilliseconds = 1'000U;
constexpr std::uint64_t kMissionObjectiveSampleMilliseconds = 250U;
constexpr std::uint64_t kAmbientEncounterProposalTimeoutMilliseconds = 10'000U;
constexpr std::uint64_t kExactEncounterPreflightTimeoutMilliseconds = 2'000U;
constexpr std::uint64_t kExactEncounterPreflightRepublishMilliseconds = 250U;
// A defeated bridge-owned bandit remains locally lootable long enough for a normal RDR2 search animation.
// The host remains the only outcome authority; after this window its world entities are despawned on both peers.
constexpr std::uint64_t kAmbientEncounterTerminalRetentionMilliseconds = 30'000U;
constexpr float kAmbientEncounterAbandonDistanceMeters = 160.0F;
// A guest must use their own verified vanilla prompt in this window.
// The barrier is deliberately short enough that a stale packet cannot leave their Story VM unguarded, but long enough to cover one normal conversation start.
constexpr std::uint64_t kMissionStartBarrierMilliseconds = 45'000U;
constexpr std::uint64_t kMissionStartBarrierRetryMilliseconds = 1'000U;
// Keep the guest's nearby Story-prompt guard asserted briefly after the host's exact-ID barrier arrives.
// This lets us reject a local mission that was already being entered, instead of treating it as newly authorized.
constexpr std::uint64_t kMissionStartBarrierPromptArmMilliseconds = 250U;

[[nodiscard]] std::uint64_t MissionObjectiveFingerprint(
    const std::string_view text) noexcept {
    std::uint64_t value = 1469598103934665603ULL;
    for (const auto character : text) {
        value ^= static_cast<std::uint8_t>(character);
        value *= 1099511628211ULL;
    }
    return value == 0U ? 1U : value;
}
constexpr std::uint64_t kAppearanceRefreshMilliseconds = 2'000U;
// Story Mode temporarily streams a personal horse out for several seconds around doors, dense towns and cutscene transitions.
// A sub-second absence used to destroy/recreate the proxy and caused duplicate horses and bad IK.
constexpr std::uint64_t kRemoteMountAbsentDebounceMilliseconds = 8'000U;
constexpr std::uint64_t kLocalMountIdentityContinuityMilliseconds = 30'000U;
constexpr std::uint64_t kMotionDiagnosticsIntervalMilliseconds = 5'000U;
constexpr std::uint64_t kWorldMirrorIntervalMilliseconds = 100U;
constexpr std::uint64_t kWorldMirrorCinematicIntervalMilliseconds = 33U;
constexpr std::uint64_t kWorldGraphDiagnosticsIntervalMilliseconds = 5'000U;
constexpr std::size_t kWorldMirrorMaximumEntities = 48U;
constexpr std::size_t kWorldMirrorMaximumCandidates = 96U;
constexpr float kWorldMirrorRadiusMeters = 80.0F;
constexpr float kWorldDamageMaximumMeters = 100.0F;
constexpr float kWorldDamageMaximumPerIntent = 25.0F;
constexpr std::uint64_t kWorldDamageMinimumIntervalMilliseconds = 90U;
constexpr std::uint64_t kLocalShotLatchMilliseconds = 500U;
constexpr std::uint64_t kLocalTraversalLatchMilliseconds = 3'000U;
constexpr std::uint64_t kLocalTraversalLandingLatchMilliseconds = 500U;
constexpr float kGuestTeleportSideOffsetMeters = 1.5F;
constexpr std::uint64_t kTeleportVerificationDelayMilliseconds = 350U;
constexpr float kTeleportVerificationToleranceMeters = 5.0F;
constexpr std::uint64_t kCheckpointRespawnConfirmationMilliseconds = 1'500U;
constexpr float kCheckpointRespawnMinimumHealthFraction = 0.50F;
constexpr std::uint64_t kMissionStateHeartbeatMilliseconds = 1'000U;
constexpr std::uint64_t kMissionCinematicHeartbeatMilliseconds = 1'000U;
constexpr std::uint64_t kMissionCameraSampleMilliseconds = 33U;
constexpr std::uint64_t kAnimSceneSampleMilliseconds = 50U;
constexpr std::uint64_t kRemoteMountMaintainIntervalMilliseconds = 50U;
constexpr std::uint64_t kAnimSceneFreshnessMilliseconds = 750U;
// Disk/resource preparation is bounded but intentionally wider than the old 2500 ms window.
// Large Story dictionaries can cold-load for several seconds; the host Story VM remains nonblocking and the guest converges to its live phase after commit.
// The host keeps two additional seconds for LAN/Hamachi.
constexpr std::uint64_t kAnimSceneHybridGuestPrepareTimeoutMilliseconds =
    8'000U;
constexpr std::uint64_t kAnimSceneHybridHostDecisionTimeoutMilliseconds =
    2'500U;
constexpr std::uint64_t kAnimSceneHybridHostReplyWindowMilliseconds =
    10'000U;
constexpr std::uint64_t kAnimSceneHybridGuestDecisionWindowMilliseconds =
    4'000U;
// Keep the final host frame through the 750ms control-recovery debounce.
// A shorter lease switched the guest to the third-person fallback between the end of the AnimScene and PrepareResume even on a healthy LAN session.
constexpr std::uint64_t kMissionCameraFreshnessMilliseconds = 2'500U;
constexpr std::uint64_t kMissionInitialCameraWaitMilliseconds = 1'500U;
constexpr std::uint64_t kMissionControlRecoveryMilliseconds = 750U;
constexpr std::uint64_t kMissionResumeLongWaitWarningMilliseconds = 30'000U;
constexpr std::uint64_t kMissionHostLossTimeoutMilliseconds = 3'000U;
constexpr std::uint64_t kMissionSkipInjectionMilliseconds = 2'500U;
constexpr std::uint64_t kGuestPostCinematicSkipGraceMilliseconds = 6'000U;
constexpr std::uint64_t kMissionCinematicTerminalClearMilliseconds = 1'500U;
constexpr std::uint64_t kMissionRecoveryWindowMilliseconds = 2'500U;
constexpr std::uint64_t kMissionLoadingDetectionMilliseconds = 350U;
constexpr std::uint64_t kMissionSpectatorControlLockDebounceMilliseconds =
    150U;
constexpr std::uint64_t kMissionIsolationDiagnosticsMilliseconds = 5'000U;
constexpr std::uint64_t kRuntimeDiagnosticsIntervalMilliseconds = 1'000U;
constexpr std::uint64_t kProblemDiagnosticBurstMilliseconds = 15'000U;
constexpr std::uint64_t kProblemDiagnosticSnapshotMilliseconds = 500U;
constexpr std::uint32_t kRuntimeDiagnosticHitchMilliseconds = 100U;
constexpr float kPlayerDivergencePositionThresholdMeters = 1.5F;
constexpr float kPlayerDivergenceRotationThresholdDegrees = 35.0F;
constexpr std::uint64_t kPlayerActionFreshnessThresholdMilliseconds = 750U;
constexpr std::uint64_t kPlayerActionSustainMilliseconds = 500U;
constexpr std::uint64_t kInteractionSustainMilliseconds = 100U;
constexpr std::uint32_t kPersistentPlayerActionDurationMilliseconds =
    3'600'000U;
constexpr std::uint32_t kTransientPlayerActionDurationMilliseconds =
    750U;

[[nodiscard]] PlayerSlot OtherSlot(const PlayerSlot slot) noexcept {
    return slot == PlayerSlot::Host ? PlayerSlot::Guest : PlayerSlot::Host;
}

[[nodiscard]] std::size_t SlotIndex(const PlayerSlot slot) noexcept {
    return slot == PlayerSlot::Host ? 0U : 1U;
}

[[nodiscard]] CommandOpcode MenuOpcode(
    const BridgeCommand command,
    const bool soloOverrideEnabled) noexcept {
    switch (command) {
        case BridgeCommand::ToggleSoloTest:
        case BridgeCommand::ToggleGhostRecord:
        case BridgeCommand::ToggleGhostReplay:
        case BridgeCommand::ToggleGuestWorldView:
        case BridgeCommand::GrantTestPistol:
        case BridgeCommand::GrantTestLasso:
        case BridgeCommand::ProbeRepeatingShotgunShopUnlock:
        case BridgeCommand::EnableRepeatingShotgunShopUnlock:
        case BridgeCommand::ProbePoisonThrowingKnifePamphlet:
        case BridgeCommand::EnablePoisonThrowingKnifePamphlet:
        case BridgeCommand::StopSession:
        case BridgeCommand::SaveProblemMarker:
        case BridgeCommand::SkipCutscene:
        case BridgeCommand::EmergencyRecover:
        case BridgeCommand::ArmHunt1MissionProgression:
        case BridgeCommand::ArmFud1MissionProgression:
        case BridgeCommand::DisarmMissionProgression:
            // This command is carried by the local-only session-menu channel and never reaches MenuOpcode.
            return CommandOpcode::ToggleDiagnostics;
        case BridgeCommand::ToggleSoloOverride:
            return soloOverrideEnabled
                       ? CommandOpcode::SoloOverrideOn
                       : CommandOpcode::SoloOverrideOff;
        case BridgeCommand::TeleportGuest:
        case BridgeCommand::TeleportToPlayer:
            return CommandOpcode::TeleportGuest;
        case BridgeCommand::ResyncEntities:
            return CommandOpcode::Resync;
        case BridgeCommand::ResyncEquipment:
            return CommandOpcode::ResyncEquipment;
        case BridgeCommand::RetryCheckpoint:
            return CommandOpcode::RetryCheckpoint;
        case BridgeCommand::ToggleDiagnostics:
            return CommandOpcode::ToggleDiagnostics;
        case BridgeCommand::Unload:
            return CommandOpcode::Unload;
    }
    return CommandOpcode::Unload;
}

[[nodiscard]] std::uint32_t ElapsedMilliseconds(
    const std::uint64_t previous,
    const std::uint64_t current) noexcept {
    if (current < previous) {
        return 0U;
    }
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(
            current - previous,
            std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] std::uint16_t AdvanceNonZero(
    const std::uint16_t value) noexcept {
    return value == std::numeric_limits<std::uint16_t>::max()
               ? 1U
               : static_cast<std::uint16_t>(value + 1U);
}

[[nodiscard]] std::uint32_t AdvanceNonZero(
    const std::uint32_t value) noexcept {
    return value == std::numeric_limits<std::uint32_t>::max()
               ? 1U
               : value + 1U;
}

[[nodiscard]] float NormalizeHeading(float heading) noexcept {
    if (!std::isfinite(heading)) {
        return 0.0F;
    }
    heading = std::fmod(heading, 360.0F);
    return heading < 0.0F ? heading + 360.0F : heading;
}

[[nodiscard]] std::string_view RoleName(
    const std::optional<PlayerSlot> slot) noexcept {
    if (!slot.has_value()) {
        return "pending";
    }
    return *slot == PlayerSlot::Host ? "host" : "guest";
}

[[nodiscard]] std::string_view AnimScenePrepareStageName(
    const ReplicatedAnimScenePrepareStage stage) noexcept {
    switch (stage) {
        case ReplicatedAnimScenePrepareStage::None:
            return "none";
        case ReplicatedAnimScenePrepareStage::Creating:
            return "creating";
        case ReplicatedAnimScenePrepareStage::WaitingForBindings:
            return "waiting-bindings";
        case ReplicatedAnimScenePrepareStage::WaitingForResource:
            return "waiting-resource";
        case ReplicatedAnimScenePrepareStage::Ready:
            return "ready";
        case ReplicatedAnimScenePrepareStage::Failed:
            return "failed";
    }
    return "unknown";
}

[[nodiscard]] std::uint64_t MakeProblemMarkerCorrelationId(
    const PlayerSlot slot,
    const std::uint32_t markerId,
    const std::uint64_t nowMs) noexcept {
    // Stay below 2^53 so the value remains exact in JSON numeric diagnostics.
    // The session fingerprint disambiguates the intentionally bounded tick.
    const auto roleCode = slot == PlayerSlot::Host ? 1ULL : 2ULL;
    return (roleCode << 48U) |
           ((nowMs & 0x00FF'FFFFULL) << 24U) |
           (static_cast<std::uint64_t>(markerId) & 0x00FF'FFFFULL);
}

[[nodiscard]] std::uint32_t ProblemMarkerLocalId(
    const std::uint64_t correlationId) noexcept {
    return static_cast<std::uint32_t>(correlationId & 0x00FF'FFFFULL);
}

[[nodiscard]] std::string_view ProblemMarkerOriginRole(
    const std::uint64_t correlationId) noexcept {
    switch ((correlationId >> 48U) & 0x0FULL) {
        case 1ULL:
            return "host";
        case 2ULL:
            return "guest";
        default:
            return "unknown";
    }
}

[[nodiscard]] std::string_view MissionPhaseName(
    const MissionPhase phase) noexcept {
    switch (phase) {
        case MissionPhase::Idle:
            return "idle";
        case MissionPhase::Active:
            return "active";
        case MissionPhase::Cutscene:
            return "cutscene";
        case MissionPhase::Loading:
            return "loading";
        case MissionPhase::Recovery:
            return "recovery";
        case MissionPhase::SoloOverride:
            return "solo-override";
    }
    return "unknown";
}

[[nodiscard]] bool IsCinematicPresentationPhase(
    const MissionCinematicPhase phase) noexcept {
    return phase == MissionCinematicPhase::Playing ||
           phase == MissionCinematicPhase::Loading ||
           phase == MissionCinematicPhase::PrepareResume;
}

[[nodiscard]] std::string_view ActionLifecycleTag(
    const PlayerActionKind kind) noexcept {
    if (kind == PlayerActionKind::Crafting) {
        return "[CRAFTING_LIFECYCLE]";
    }
    return kind == PlayerActionKind::Lasso ||
                   kind == PlayerActionKind::Hogtie
               ? "[LASSO_LIFECYCLE]"
               : "[COMBAT_LIFECYCLE]";
}

[[nodiscard]] std::uint64_t DiagnosticAge(
    const std::optional<std::uint64_t> timestamp,
    const std::uint64_t now) noexcept {
    if (!timestamp.has_value() || now < *timestamp) {
        return 0U;
    }
    return now - *timestamp;
}

struct LocalLocomotionIntent final {
    float movementHeading{};
    float forwardSpeed{};
    float rightSpeed{};
    float moveBlend{};
};

[[nodiscard]] LocalLocomotionIntent BuildLocalLocomotionIntent(
    const LocalPlayerSample& sample) noexcept {
    LocalLocomotionIntent result;
    const auto horizontalSpeed = std::hypot(
        sample.velocity.x,
        sample.velocity.y);
    result.movementHeading = MovementHeadingFromVelocity(
        sample.velocity,
        sample.heading);
    const auto relativeVelocity = ComputePedRelativeVelocity(
        sample.velocity,
        sample.heading);
    result.forwardSpeed = relativeVelocity.forward;
    result.rightSpeed = relativeVelocity.right;
    const auto velocityMoveBlend =
        horizontalSpeed < 0.12F
            ? 0.0F
            : horizontalSpeed < 2.20F
                  ? 1.0F
                  : horizontalSpeed < 5.0F
                        ? 2.0F
                        : 3.0F;
    // Camp and mission scripts can cap Arthur's root velocity without changing the locomotion graph.
    // Inferring the gait only from meters per second then advertised Idle while the real ped was walking and the remote replica visibly slid.
    // RDR2's desired blend is the animation source of truth; retain the velocity estimate only as a conservative fallback when that native is unavailable or momentarily reports zero.
    result.moveBlend = sample.desiredMoveBlendValid
                           ? std::max(
                                 std::clamp(
                                     sample.desiredMoveBlend,
                                     0.0F,
                                     3.0F),
                                 velocityMoveBlend)
                           : velocityMoveBlend;
    return result;
}

[[nodiscard]] PlayerLocomotionMode SelectLocalLocomotionMode(
    const LocalPlayerSample& sample,
    const PlayerLifecycle lifecycle) noexcept {
    if (sample.mounted) {
        return PlayerLocomotionMode::Mounted;
    }
    if (sample.ragdoll || sample.gettingUp ||
        lifecycle != PlayerLifecycle::Alive || sample.downed) {
        return PlayerLocomotionMode::Ragdoll;
    }
    if (sample.jumping || sample.climbing) {
        return PlayerLocomotionMode::Traversal;
    }
    if (sample.swimming) {
        // There is no stable public swimming task/clip identifier in the pinned SDK.
        // Keep the root in the grounded native-task lane and let the local water volume select RDR2's swimming graph; a vertical swim velocity must not be misclassified as a ledge fall.
        return PlayerLocomotionMode::Grounded;
    }
    if (sample.falling || std::abs(sample.velocity.z) >= 1.0F) {
        return PlayerLocomotionMode::Airborne;
    }
    if (sample.aiming) {
        return PlayerLocomotionMode::Aiming;
    }
    return PlayerLocomotionMode::Grounded;
}

[[nodiscard]] bool IsWorldMirrorSafeRemoteState(
    const std::optional<PlayerStatePayload>& state) noexcept {
    if (!state.has_value()) {
        return false;
    }
    // Mission actors are exactly the entities the guest needs.
    // The previous free-roam gate discarded the host graph as soon as InMission became true.
    // Only a transition/cutscene is unsafe for proxy mutation.
    constexpr auto kUnsafeFlags = static_cast<std::uint32_t>(
        PlayerStateFlag::InCutscene);
    return (state->flags & kUnsafeFlags) == 0U;
}

}  // namespace

BridgeRuntime::BridgeRuntime(
    IScriptHookFacade& facade,
    IFrameTransport& transport,
    BridgeRuntimeConfig config)
    : facade_(facade),
      transport_(transport),
      config_(config),
      bubble_(config.bubbleAction) {}

bool BridgeRuntime::HasActiveHostAnimSceneDefinition() const noexcept {
    const bool hostAuthority =
        localSlot_ == PlayerSlot::Host ||
        (!localSlot_.has_value() &&
         hostAnimSceneReconnectPending_ &&
         helloExpectedRole_ == PlayerSlot::Host);
    return hostAuthority &&
           localMissionCinematicState_.has_value() &&
           IsCinematicPresentationPhase(
               localMissionCinematicState_->phase) &&
           localAnimSceneDefinition_.has_value();
}

bool BridgeRuntime::BeginHostAnimScenePrepareAttempt(
    const std::string_view reason) noexcept {
    if (!localAnimSceneDefinition_.has_value()) {
        return false;
    }
    try {
        auto refreshed = *localAnimSceneDefinition_;
        refreshed.definitionRevision =
            localAnimSceneDefinitionRevision_;
        refreshed.fingerprintLow = 0U;
        refreshed.fingerprintHigh = 0U;
        const auto fingerprint =
            ComputeAnimSceneDefinitionFingerprint(refreshed);
        if ((fingerprint.low | fingerprint.high) == 0U) {
            throw std::runtime_error{
                "zero AnimScene definition fingerprint"};
        }
        refreshed.fingerprintLow = fingerprint.low;
        refreshed.fingerprintHigh = fingerprint.high;
        localAnimSceneDefinition_ = std::move(refreshed);
        localAnimSceneDefinitionRevision_ =
            AdvanceNonZero(localAnimSceneDefinitionRevision_);
        if (lastLocalAnimSceneState_.has_value()) {
            lastLocalAnimSceneState_->definitionRevision =
                localAnimSceneDefinition_->definitionRevision;
        }
        localAnimSceneDefinitionSentAtMs_ = 0U;
        localAnimSceneGuestReadyAtMs_ = 0U;
        localAnimSceneGuestReady_ = false;
        localAnimSceneCommitSent_ = false;
        localAnimSceneDefinitionTimedOut_ = false;
        remoteAnimSceneControlActionId_ = 0U;
        facade_.Log(
            "[ANIMSCENE_HYBRID][PREPARE_ATTEMPT] reason=" +
            std::string{reason} + ", revision=" +
            std::to_string(
                localAnimSceneDefinition_->definitionRevision) +
            "; stale readiness from every earlier attempt is invalid");
        return true;
    } catch (...) {
        localAnimSceneDefinition_.reset();
        localAnimSceneDefinitionSentAtMs_ = 0U;
        localAnimSceneGuestReadyAtMs_ = 0U;
        localAnimSceneGuestReady_ = false;
        localAnimSceneCommitSent_ = false;
        localAnimSceneDefinitionTimedOut_ = true;
        try {
            facade_.Log(
                "[ANIMSCENE_HYBRID][PREPARE_ATTEMPT][ERROR] could not mint a new definition revision/fingerprint; SAFE_FALLBACK retained");
        } catch (...) {
        }
        return false;
    }
}

bool BridgeRuntime::SetHostAnimSceneStartBarrier(
    const bool active,
    const std::string_view reason) noexcept {
    if (active == localAnimSceneStartBarrierActive_) {
        return true;
    }
    const bool applied =
        facade_.MaintainHostAnimSceneStartBarrier(active);
    if (!applied && active) {
        try {
            facade_.Log(
                "[WARNING][ANIMSCENE_HYBRID][PRELOAD_BARRIER] could not arm the non-invasive prepare marker; exact path was rejected and SAFE_FALLBACK retained");
        } catch (...) {
        }
        return false;
    }
    localAnimSceneStartBarrierActive_ = active;
    try {
        facade_.Log(
            std::string{"[ANIMSCENE_HYBRID][PRELOAD_BARRIER] "} +
            (active ? "armed" : "released") + ", reason=" +
            std::string{reason});
    } catch (...) {
    }
    return true;
}

void BridgeRuntime::ResetAnimSceneHybridState(
    const bool abortFacade,
    const bool preserveHostDefinition) noexcept {
    const bool hostAuthority =
        localSlot_ == PlayerSlot::Host ||
        (!localSlot_.has_value() &&
         hostAnimSceneReconnectPending_ &&
         helloExpectedRole_ == PlayerSlot::Host);
    const bool preserve =
        preserveHostDefinition && hostAuthority &&
        localAnimSceneDefinition_.has_value();
    std::optional<AnimSceneDefinitionPayload> preservedDefinition;
    std::optional<AnimSceneReplicaStatePayload> preservedState;
    const auto preservedNextRevision =
        localAnimSceneDefinitionRevision_;
    const auto preservedCaptureSequence =
        lastCapturedAnimSceneSequence_;
    if (preserve) {
        preservedDefinition = std::move(localAnimSceneDefinition_);
        preservedState = std::move(lastLocalAnimSceneState_);
    }
    if (abortFacade) {
        facade_.AbortReplicatedAnimSceneDefinition();
    }
    (void)SetHostAnimSceneStartBarrier(false, "hybrid-reset");
    remoteAnimSceneDefinitionSequences_.Reset();
    remoteAnimSceneControlSequences_.Reset();
    localAnimSceneDefinition_.reset();
    remoteAnimSceneDefinition_.reset();
    pendingRemoteAnimSceneCommit_.reset();
    lastLocalAnimSceneState_.reset();
    localAnimSceneDefinitionRevision_ = 1U;
    // The local control action ID is session-scoped, not definition-scoped.
    // Keeping it monotonic across same-session teardown/reprepare makes a replayed GuestReady newer than the value already accepted by the peer.
    remoteAnimSceneControlActionId_ = 0U;
    lastCapturedAnimSceneSequence_ = 0U;
    localAnimSceneDefinitionSentAtMs_ = 0U;
    localAnimSceneGuestReadyAtMs_ = 0U;
    remoteAnimSceneDefinitionReceivedAtMs_ = 0U;
    remoteAnimSceneReadySentAtMs_ = 0U;
    nextRemoteAnimScenePrepareDiagnosticsMs_ = 0U;
    localAnimSceneGuestReady_ = false;
    localAnimSceneCommitSent_ = false;
    localAnimSceneDefinitionTimedOut_ = false;
    remoteAnimSceneDefinitionPrepared_ = false;
    remoteAnimSceneDefinitionCommitted_ = false;
    remoteAnimSceneDefinitionResponseSent_ = false;
    remoteAnimScenePrepareStage_ =
        ReplicatedAnimScenePrepareStage::None;
    remoteAnimScenePrepareResolvedRoles_ = 0U;
    remoteAnimScenePrepareRequiredRoles_ = 0U;
    remoteAnimScenePreparePendingEntityId_ = {};
    remoteAnimScenePreparePendingRoleName_.clear();
    if (!preserve) {
        hostAnimSceneReconnectPending_ = false;
        awaitingRestoredGuestStream_ = false;
        forceHostWorldMirrorReplay_ = false;
    }
    if (preserve) {
        localAnimSceneDefinition_ = std::move(preservedDefinition);
        lastLocalAnimSceneState_ = std::move(preservedState);
        localAnimSceneDefinitionRevision_ = preservedNextRevision;
        lastCapturedAnimSceneSequence_ = preservedCaptureSequence;
    }
}

bool BridgeRuntime::Start(
    const GameIdentity& identity,
    std::string& error) {
    error.clear();
    if (active_) {
        return true;
    }

    const auto gate = VersionGate::Evaluate(
        identity,
        facade_.QueryRuntimeMode());
    if (!gate.allowed) {
        error = gate.message;
        facade_.Log(error);
        return false;
    }

    const auto now = facade_.TickMilliseconds();
    sessionEpoch_ = static_cast<std::uint32_t>(now ^ (now >> 32U));
    if (sessionEpoch_ == 0U) {
        sessionEpoch_ = 1U;
    }
    previousTickMs_ = now;
    nextReconnectMs_ = now;
    nextWorldStateMs_ = now;
    nextWorldMirrorSampleMs_ = now;
    nextWorldGraphDiagnosticsMs_ = now;
    nextEquipmentRefreshMs_ = now;
    nextAppearanceRefreshMs_ = now;
    nextMotionDiagnosticsMs_ =
        now + kMotionDiagnosticsIntervalMilliseconds;
    nextMissionStateHeartbeatMs_ = now;
    nextMissionCinematicHeartbeatMs_ = now;
    nextMissionCameraSampleMs_ = now;
    nextAnimSceneSampleMs_ = now;
    nextRemoteMountMaintainMs_ = now;
    nextMissionWorldWarningMs_ = now;
    nextMissionIsolationDiagnosticsMs_ = now;
    nextRuntimeDiagnosticsMs_ = now;
    runtimeDiagnosticTickCount_ = 0U;
    runtimeDiagnosticTickElapsedSumMs_ = 0U;
    runtimeDiagnosticHitchCount_ = 0U;
    runtimeDiagnosticTickMaximumMs_ = 0U;
    motionSampleCounts_.fill(0U);
    motionApplyFailures_ = 0U;
    motionSourceAgeMaximumMs_ = 0U;
    motionArrivalGapMaximumMs_ = 0U;
    shouldUnload_ = false;
    userProblemMarkerId_ = 0U;
    previousQuickMarkerPressed_ = false;
    notificationUntilMs_ = 0U;
    notificationText_.clear();
    problemDiagnosticCorrelationId_ = 0U;
    problemDiagnosticUntilMs_ = 0U;
    nextProblemDiagnosticSnapshotMs_ = 0U;
    problemDiagnosticSampleIndex_ = 0U;
    problemDiagnosticRemoteOrigin_ = false;
    lastWorldDamageIntentMs_ = 0U;
    worldDamageIntentSequences_.Reset();
    remoteMissionSequences_.Reset();
    remoteMissionDialogueCueSequences_.Reset();
    remoteMissionDialogueReadySequences_.Reset();
    remoteAmbientEncounterProposalSequences_.Reset();
    remoteAmbientEncounterStateSequences_.Reset();
    remoteMissionCameraSequences_.Reset();
    remoteMissionCinematicSequences_.Reset();
    remoteMissionCinematicActionSequences_.Reset();
    remoteAnimSceneSequences_.Reset();
    pendingHostWorldDespawns_.clear();
    hostWorldReplayAwaitingGuest_ = false;
    hostWorldReplayGuestDeadlineMs_ = 0U;
    ResetAnimSceneHybridState(false);
    localAnimSceneControlActionId_ = 0U;
    localFireSequence_ = 0U;
    localAnimationSampleSequence_ = 0U;
    localPlayerActionSequence_ = 0U;
    localPlayerActionId_ = 0U;
    localMissionState_.reset();
    facade_.ClearHostMissionDialoguePresentation();
    remoteMissionState_.reset();
    localMissionObjective_.reset();
    remoteMissionObjective_.reset();
    localMissionDialogueCue_.reset();
    remoteMissionDialogueCue_.reset();
    pendingHostMissionDialogueCue_.reset();
    remoteMissionDialogueReady_.reset();
    if (ambientEncounterCoordinator_.Active().has_value()) {
        facade_.ClearAmbientEncounterPresentation(
            ambientEncounterCoordinator_.Active()->instanceId);
    }
    if (remoteAmbientEncounter_.has_value()) {
        facade_.ClearAmbientEncounterPresentation(
            remoteAmbientEncounter_->instanceId);
    }
    ambientEncounterCoordinator_ = AmbientEncounterCoordinator{};
    remoteAmbientEncounter_.reset();
    localAmbientEncounterProposalId_ = 0U;
    localAmbientEncounterProposalExpiresMs_ = 0U;
    localAmbientEncounterInstanceId_ = 0U;
    localAmbientEncounterTerminalAtMs_ = 0U;
    remoteAmbientEncounterTerminalAtMs_ = 0U;
    localExactEncounterPreflightDeadlineMs_ = 0U;
    nextExactEncounterPreflightPublishMs_ = 0U;
    remoteExactEncounterPreflightInstanceId_ = 0U;
    localMissionDialogueSequence_ = 0U;
    lastMissionDialogueCueSentMs_ = 0U;
    pendingHostMissionDialogueDueMs_ = 0U;
    localMissionObjectiveRevision_ = 0U;
    nextMissionObjectiveSampleMs_ = 0U;
    localMissionProgressionOffer_.reset();
    localMissionProgressionCompletion_.reset();
    remoteMissionProgressionOffer_.reset();
    localMissionStartBarrier_.reset();
    remoteMissionStartBarrier_.reset();
    localMissionStartBarrierDeadlineMs_ = 0U;
    remoteMissionStartBarrierDeadlineMs_ = 0U;
    remoteMissionStartBarrierPromptArmedAtMs_ = 0U;
    localMissionStartBarrierGuestStarted_ = false;
    remoteMissionStartBarrierGuestStarted_ = false;
    remoteMissionStartBarrierReleased_ = false;
    remoteMissionStartBarrierRejected_ = false;
    remoteMissionProgressionParticipated_ = false;
    nextGuestMissionInstanceStartedRetryMs_ = 0U;
    remoteMissionProgressionEligible_ = false;
    remoteMissionProgressionAppliedEventId_.reset();
    localProgressionMissionId_ = 0U;
    guestMissionProgressionEligible_ = false;
    guestMissionProgressionCompletionAcknowledged_ = false;
    nextMissionProgressionCompletionRetryMs_ = 0U;
    localMissionCinematicState_.reset();
    remoteMissionCinematicState_.reset();
    remoteMissionCameraState_.reset();
    remoteAnimSceneState_.reset();
    remoteMissionCameraReceivedAtMs_.reset();
    remoteMissionCinematicReceivedAtMs_.reset();
    remoteAnimSceneReceivedAtMs_.reset();
    localMissionEpoch_ = 1U;
    localMissionRevision_ = 1U;
    localMissionCinematicGeneration_ = 0U;
    localMissionCinematicRevision_ = 0U;
    localMissionCinematicActionId_ = 0U;
    remoteMissionCinematicActionId_ = 0U;
    localMissionCameraRevision_ = 1U;
    localAnimSceneRevision_ = 1U;
    localAppearanceRevision_ = 1U;
    localCheckpointGeneration_ = 1U;
    localMissionRecoveryUntilMs_ = 0U;
    localSampleUnavailableSinceMs_ = 0U;
    localCinematicControlRecoveredSinceMs_ = 0U;
    localCinematicPrepareStartedMs_ = 0U;
    localCinematicTerminalClearSinceMs_ = 0U;
    localCinematicCameraWaitStartedMs_ = 0U;
    localCinematicCameraWaitWarned_ = false;
    localCutsceneSkipUntilMs_ = 0U;
    localCutsceneSkipVoteUntilMs_ = 0U;
    remoteCutsceneSkipVoteUntilMs_ = 0U;
    guestQuarantineSkipUntilMs_ = 0U;
    guestPostCinematicSkipUntilMs_ = 0U;
    localCinematicTerminalLatchActive_ = false;
    previousLocalMissionActive_ = false;
    localMissionInitialized_ = false;
    localMissionCameraActive_ = false;
    localAnimSceneActive_ = false;
    remoteNativeAnimSceneActive_ = false;
    localCinematicCameraReady_ = false;
    localCinematicPresentationReady_ = false;
    localCinematicResumeReady_ = false;
    remoteCinematicPresentationReadySent_ = false;
    remoteCinematicResumeReadySent_ = false;
    remoteCinematicResumeFallbackUsed_ = false;
    previousGuestLocalMissionDetected_ = false;
    guestMissionQuarantineActive_ = false;
    guestMissionQuarantineSkipLatched_ = false;
    remoteParticipantSceneIsolated_ = false;
    guestMissionIsolationDetections_ = 0U;
    guestMissionQuarantineTicks_ = 0U;
    remoteParticipantIsolationTransitions_ = 0U;
    lastMissionTimelineState_.clear();
    playerDivergenceStateInitialized_ = false;
    playerDivergenceActive_ = false;
    entityDivergenceStateInitialized_ = false;
    entityDivergenceActive_ = false;
    lastRemotePlayerActionReceivedAtMs_.reset();
    localPlayerActions_.fill(LocalPlayerActionRuntime{});
    remotePlayerActionSequences_.Reset();
    playerActionsSent_ = 0U;
    playerActionsReceived_ = 0U;
    playerActionsDuplicate_ = 0U;
    playerActionsStale_ = 0U;
    playerActionsRejected_ = 0U;
    remoteAnimationSequences_.Reset();
    remoteAnimationPayloadSequences_.Reset();
    latestRemoteAnimationState_.reset();
    latestRemoteAnimationReceivedAtMs_.reset();
    latestRemoteAnimationSenderTickMs_ = 0U;
    motionReplicationMode_ = MotionReplicationWireMode::TaskNavmesh;
    motionReplicationFlags_ = 0U;
    motionReplicationRevision_ = 0U;
    animationSamplesSent_ = 0U;
    animationSamplesReceived_ = 0U;
    animationSamplesRejected_ = 0U;
    animationSamplesExpired_ = 0U;
    previousLocalWeaponHash_ = 0U;
    previousLocalWeaponAmmo_ = 0U;
    previousLocalMountModelHash_ = 0U;
    previousLocalMountHandle_ = 0;
    lastLocalOwnedMountPresentMs_ = 0U;
    localMountGeneration_ = 1U;
    lastLocalMountState_.reset();
    remoteMountState_.reset();
    pendingRemoteMountAbsentState_.reset();
    remoteMountAbsentSinceMs_ = 0U;
    remoteMountAbsenceConfirmed_ = false;
    nextRemoteMountMaintainMs_ = 0U;
    remoteMountSequences_.Reset();
    localShotAimTarget_ = {};
    localShotLatchExpiresMs_ = 0U;
    localTraversalAnchor_ = {};
    localTraversalApproachVelocity_ = {};
    localTraversalObstaclePoint_ = {};
    localTraversalObstacleNormal_ = {};
    localTraversalExpectedLanding_ = {};
    previousLocalPosition_ = {};
    localTraversalHeading_ = 0.0F;
    localTraversalObstacleTopZ_ = 0.0F;
    previousLocalHeading_ = 0.0F;
    localTraversalLatchExpiresMs_ = 0U;
    localLocomotionEpoch_ = 1U;
    localTraversalActionId_ = 0U;
    localTraversalRevision_ = 0U;
    localTraversalSentRevision_ = 0U;
    localTraversalKind_ = PlayerTraversalKind::None;
    localTraversalFlags_ = 0U;
    previousLocalFiring_ = false;
    previousLocalJumping_ = false;
    previousLocalClimbing_ = false;
    localTraversalBecameActive_ = false;
    localTraversalLandingCaptured_ = false;
    previousLocalMounted_ = false;
    hasPreviousLocalTransform_ = false;
    previousLocalLifecycle_.reset();
    lastLocalAppearance_.reset();
    hasPreviousLocalWeaponSample_ = false;
    soloOverride_ = false;
    cutsceneSpectator_ = false;
    hostWorldMirrorActive_ = false;
    guestWorldMirrorActive_ = false;
    soloGuestWorldViewEnabled_ = false;
    guestMissionIsolationLeaseActive_ = false;
    (void)guestWorldGraph_.Reset();
    worldMirrorHost_.emplace(
        sessionEpoch_,
        1'000U,
        kWorldMirrorMaximumEntities);
    active_ = true;

    std::string transportError;
    if (transport_.Connect(transportError)) {
        SendHello();
    } else {
        error =
            "bridge started offline; sidecar reconnect pending: " +
            transportError;
        facade_.Log(error);
        nextReconnectMs_ = now + 1'000U;
    }
    facade_.Log(
        "[BUILD] bridge=" + std::string{kBridgeBuildId} +
        ", protocol=" + std::to_string(kProtocolVersion) +
        "; offline Story Mode enabled");
    return true;
}

void BridgeRuntime::SendHello(const bool reconnect) {
    struct HostCinematicReconnectSnapshot final {
        std::optional<MissionStatePayload> missionState;
        std::optional<MissionCinematicStatePayload> cinematicState;
        std::uint32_t missionEpoch{};
        std::uint32_t missionRevision{};
        std::uint32_t cinematicGeneration{};
        std::uint32_t cinematicRevision{};
        std::uint32_t cinematicActionId{};
        std::uint32_t cameraRevision{};
        std::uint32_t animSceneRevision{};
        std::uint32_t checkpointGeneration{};
        std::uint64_t missionRecoveryUntilMs{};
        std::uint64_t sampleUnavailableSinceMs{};
        std::uint64_t controlRecoveredSinceMs{};
        std::uint64_t prepareStartedMs{};
        std::uint64_t terminalClearSinceMs{};
        std::uint64_t cameraWaitStartedMs{};
        bool cameraWaitWarned{};
        bool previousMissionActive{};
        bool missionInitialized{};
        bool missionCameraActive{};
        bool animSceneActive{};
        bool cameraReady{};
        bool presentationReady{};
        bool resumeReady{};
    };
    struct OutboundReconnectSnapshot final {
        std::uint32_t fireSequence{};
        std::uint32_t animationSampleSequence{};
        std::uint32_t playerActionSequence{};
        std::uint32_t playerActionId{};
        std::uint32_t missionCinematicActionId{};
        std::array<LocalPlayerActionRuntime, 9> playerActions{};
        std::uint32_t appearanceRevision{};
        std::optional<PlayerAppearanceStatePayload> lastAppearance{};
        std::uint32_t mountGeneration{};
        std::uint16_t locomotionEpoch{};
        std::uint16_t traversalActionId{};
        std::uint16_t traversalRevision{};
        std::uint16_t traversalSentRevision{};
        bool synchronizedPaused{};
        bool hostPauseVoted{};
        bool guestPauseVoted{};
        std::uint32_t pauseVoteGeneration{};
    };
    if (reconnect && localSlot_.has_value()) {
        helloExpectedRole_ = localSlot_;
    } else if (!reconnect) {
        helloExpectedRole_.reset();
        localAnimSceneControlActionId_ = 0U;
    }
    const bool reconnectingHostAuthority =
        localSlot_ == PlayerSlot::Host ||
        (reconnect && !localSlot_.has_value() &&
         helloExpectedRole_ == PlayerSlot::Host);
    const bool preserveHostMissionAuthority =
        reconnect && reconnectingHostAuthority;
    const bool preserveHostCinematic =
        reconnect && reconnectingHostAuthority &&
        localMissionCinematicState_.has_value() &&
        IsCinematicPresentationPhase(
            localMissionCinematicState_->phase);
    bool preserveExactHostDefinition =
        preserveHostCinematic &&
        localAnimSceneDefinition_.has_value();
    if (preserveExactHostDefinition &&
        localSlot_ == PlayerSlot::Host) {
        preserveExactHostDefinition =
            BeginHostAnimScenePrepareAttempt("pipe-reconnect");
    }
    const HostCinematicReconnectSnapshot preserved{
        localMissionState_,
        localMissionCinematicState_,
        localMissionEpoch_,
        localMissionRevision_,
        localMissionCinematicGeneration_,
        localMissionCinematicRevision_,
        localMissionCinematicActionId_,
        localMissionCameraRevision_,
        localAnimSceneRevision_,
        localCheckpointGeneration_,
        localMissionRecoveryUntilMs_,
        localSampleUnavailableSinceMs_,
        localCinematicControlRecoveredSinceMs_,
        localCinematicPrepareStartedMs_,
        localCinematicTerminalClearSinceMs_,
        localCinematicCameraWaitStartedMs_,
        localCinematicCameraWaitWarned_,
        previousLocalMissionActive_,
        localMissionInitialized_,
        localMissionCameraActive_,
        localAnimSceneActive_,
        localCinematicCameraReady_,
        localCinematicPresentationReady_,
        localCinematicResumeReady_};
    const OutboundReconnectSnapshot preservedOutbound{
        localFireSequence_,
        localAnimationSampleSequence_,
        localPlayerActionSequence_,
        localPlayerActionId_,
        localMissionCinematicActionId_,
        localPlayerActions_,
        localAppearanceRevision_,
        lastLocalAppearance_,
        localMountGeneration_,
        localLocomotionEpoch_,
        localTraversalActionId_,
        localTraversalRevision_,
        localTraversalSentRevision_,
        synchronizedPaused_,
        hostPauseVoted_,
        guestPauseVoted_,
        pauseVoteGeneration_};
    const bool preserveResumeBarrier =
        preserveHostCinematic &&
        preserved.cinematicState.has_value() &&
        preserved.cinematicState->phase ==
            MissionCinematicPhase::PrepareResume &&
        !preserved.resumeReady;
    const bool guestWasExpected =
        remoteReplicaId_.IsValid() ||
        awaitingRestoredGuestStream_ ||
        hostWorldReplayAwaitingGuest_;
    const bool awaitHostWorldReplayGuest =
        preserveHostMissionAuthority && guestWasExpected &&
        worldMirrorHost_.has_value() &&
        worldMirrorHost_->Size() != 0U;
    const bool awaitRestoredGuestStream =
        preserveHostCinematic && !preserved.resumeReady &&
        guestWasExpected;
    hostAnimSceneReconnectPending_ = preserveHostCinematic;
    facade_.MaintainMissionCompanionPresentation({});
    facade_.MaintainReplicatedMissionCamera(false, std::nullopt);
    (void)facade_.MaintainReplicatedAnimScene(false, std::nullopt);
    facade_.MaintainMissionSpectator(false);
    facade_.MaintainMissionResumeBarrier(preserveResumeBarrier);
    facade_.MaintainRemoteMissionParticipant(false);
    if (!preserveHostMissionAuthority) {
        ResetHostWorldMirror(false);
    } else if (preserveExactHostDefinition) {
        facade_.Log(
            "[ANIMSCENE_HYBRID][PIPE_RECONNECT] retaining active host definition, cinematic generation and stable role graph through Hello renegotiation");
    } else if (preserveHostCinematic) {
        facade_.Log(
            "[ANIMSCENE_HYBRID][PIPE_RECONNECT] retaining SAFE_FALLBACK cinematic generation and resume barrier through Hello renegotiation");
    } else {
        facade_.Log(
            "[MISSION_RECONNECT] retaining host mission cursors and stable world graph through Hello renegotiation");
    }
    ResetGuestWorldMirror();
    DespawnRemoteReplica();
    // DespawnRemoteReplica resets hybrid-only state when no exact definition exists.
    // The mission cinematic reconnect snapshot is independent from that optional definition and must survive repeated Hello attempts too.
    hostAnimSceneReconnectPending_ = preserveHostCinematic;
    awaitingRestoredGuestStream_ = awaitRestoredGuestStream;
    hostWorldReplayAwaitingGuest_ = awaitHostWorldReplayGuest;
    hostWorldReplayGuestDeadlineMs_ =
        awaitHostWorldReplayGuest
            ? previousTickMs_ + kMissionHostLossTimeoutMilliseconds
            : 0U;
    localSlot_.reset();
    localEntityId_ = NetEntityId{};
    playerEntityIds_.fill(NetEntityId{});
    pendingRevive_.reset();
    lastRemoteStateMs_.reset();
    pendingTeleportDestination_.reset();
    pendingMissionGuestTeleport_.reset();
    pendingTeleportRequestedAtMs_ = 0U;
    lastLocalEquipment_.reset();
    lastLocalAppearance_.reset();
    nextWorldStateMs_ = previousTickMs_;
    nextWorldMirrorSampleMs_ = previousTickMs_;
    nextEquipmentRefreshMs_ = previousTickMs_;
    nextAppearanceRefreshMs_ = previousTickMs_;
    nextMotionDiagnosticsMs_ =
        previousTickMs_ +
        kMotionDiagnosticsIntervalMilliseconds;
    nextMissionStateHeartbeatMs_ = previousTickMs_;
    nextMissionCinematicHeartbeatMs_ = previousTickMs_;
    nextMissionCameraSampleMs_ = previousTickMs_;
    nextAnimSceneSampleMs_ = previousTickMs_;
    nextRemoteMountMaintainMs_ = previousTickMs_;
    nextMissionWorldWarningMs_ = previousTickMs_;
    nextMissionIsolationDiagnosticsMs_ = previousTickMs_;
    nextRuntimeDiagnosticsMs_ = previousTickMs_;
    motionSampleCounts_.fill(0U);
    motionApplyFailures_ = 0U;
    motionSourceAgeMaximumMs_ = 0U;
    motionArrivalGapMaximumMs_ = 0U;
    lastWorldDamageIntentMs_ = 0U;
    worldDamageIntentSequences_.Reset();
    remoteMissionSequences_.Reset();
    remoteMissionDialogueCueSequences_.Reset();
    remoteMissionDialogueReadySequences_.Reset();
    remoteAmbientEncounterProposalSequences_.Reset();
    remoteAmbientEncounterStateSequences_.Reset();
    remoteMissionCameraSequences_.Reset();
    remoteMissionCinematicSequences_.Reset();
    remoteMissionCinematicActionSequences_.Reset();
    remoteAnimSceneSequences_.Reset();
    localFireSequence_ = 0U;
    localAnimationSampleSequence_ = 0U;
    localPlayerActionSequence_ = 0U;
    localPlayerActionId_ = 0U;
    localMissionState_.reset();
    facade_.ClearHostMissionDialoguePresentation();
    remoteMissionState_.reset();
    localMissionDialogueCue_.reset();
    remoteMissionDialogueCue_.reset();
    pendingHostMissionDialogueCue_.reset();
    remoteMissionDialogueReady_.reset();
    if (ambientEncounterCoordinator_.Active().has_value()) {
        facade_.ClearAmbientEncounterPresentation(
            ambientEncounterCoordinator_.Active()->instanceId);
    }
    if (remoteAmbientEncounter_.has_value()) {
        facade_.ClearAmbientEncounterPresentation(
            remoteAmbientEncounter_->instanceId);
    }
    ambientEncounterCoordinator_ = AmbientEncounterCoordinator{};
    remoteAmbientEncounter_.reset();
    localAmbientEncounterProposalId_ = 0U;
    localAmbientEncounterProposalExpiresMs_ = 0U;
    localAmbientEncounterInstanceId_ = 0U;
    localAmbientEncounterTerminalAtMs_ = 0U;
    remoteAmbientEncounterTerminalAtMs_ = 0U;
    localExactEncounterPreflightDeadlineMs_ = 0U;
    nextExactEncounterPreflightPublishMs_ = 0U;
    remoteExactEncounterPreflightInstanceId_ = 0U;
    localMissionDialogueSequence_ = 0U;
    lastMissionDialogueCueSentMs_ = 0U;
    pendingHostMissionDialogueDueMs_ = 0U;
    localMissionCinematicState_.reset();
    remoteMissionCinematicState_.reset();
    remoteMissionCameraState_.reset();
    remoteAnimSceneState_.reset();
    remoteMissionCameraReceivedAtMs_.reset();
    remoteMissionCinematicReceivedAtMs_.reset();
    remoteAnimSceneReceivedAtMs_.reset();
    localMissionEpoch_ = 1U;
    localMissionRevision_ = 1U;
    localMissionCinematicGeneration_ = 0U;
    localMissionCinematicRevision_ = 0U;
    localMissionCinematicActionId_ = 0U;
    remoteMissionCinematicActionId_ = 0U;
    localMissionCameraRevision_ = 1U;
    localAnimSceneRevision_ = 1U;
    localAppearanceRevision_ = 1U;
    localCheckpointGeneration_ = 1U;
    localMissionRecoveryUntilMs_ = 0U;
    localSampleUnavailableSinceMs_ = 0U;
    localCinematicControlRecoveredSinceMs_ = 0U;
    localCinematicPrepareStartedMs_ = 0U;
    localCinematicTerminalClearSinceMs_ = 0U;
    localCinematicCameraWaitStartedMs_ = 0U;
    localCinematicCameraWaitWarned_ = false;
    localCutsceneSkipUntilMs_ = 0U;
    localCutsceneSkipVoteUntilMs_ = 0U;
    remoteCutsceneSkipVoteUntilMs_ = 0U;
    guestQuarantineSkipUntilMs_ = 0U;
    guestPostCinematicSkipUntilMs_ = 0U;
    localCinematicTerminalLatchActive_ = false;
    previousLocalMissionActive_ = false;
    localMissionInitialized_ = false;
    localMissionCameraActive_ = false;
    localAnimSceneActive_ = false;
    remoteNativeAnimSceneActive_ = false;
    localCinematicCameraReady_ = false;
    localCinematicPresentationReady_ = false;
    localCinematicResumeReady_ = false;
    remoteCinematicPresentationReadySent_ = false;
    remoteCinematicResumeReadySent_ = false;
    remoteCinematicResumeFallbackUsed_ = false;
    previousGuestLocalMissionDetected_ = false;
    guestMissionQuarantineActive_ = false;
    guestMissionQuarantineSkipLatched_ = false;
    remoteParticipantSceneIsolated_ = false;
    guestMissionIsolationDetections_ = 0U;
    guestMissionQuarantineTicks_ = 0U;
    remoteParticipantIsolationTransitions_ = 0U;
    lastMissionTimelineState_.clear();
    playerDivergenceStateInitialized_ = false;
    playerDivergenceActive_ = false;
    entityDivergenceStateInitialized_ = false;
    entityDivergenceActive_ = false;
    lastRemotePlayerActionReceivedAtMs_.reset();
    localPlayerActions_.fill(LocalPlayerActionRuntime{});
    remotePlayerActionSequences_.Reset();
    playerActionsSent_ = 0U;
    playerActionsReceived_ = 0U;
    playerActionsDuplicate_ = 0U;
    playerActionsStale_ = 0U;
    playerActionsRejected_ = 0U;
    remoteAnimationSequences_.Reset();
    remoteAnimationPayloadSequences_.Reset();
    latestRemoteAnimationState_.reset();
    latestRemoteAnimationReceivedAtMs_.reset();
    latestRemoteAnimationSenderTickMs_ = 0U;
    animationSamplesSent_ = 0U;
    animationSamplesReceived_ = 0U;
    animationSamplesRejected_ = 0U;
    animationSamplesExpired_ = 0U;
    previousLocalWeaponHash_ = 0U;
    previousLocalWeaponAmmo_ = 0U;
    previousLocalMountModelHash_ = 0U;
    previousLocalMountHandle_ = 0;
    lastLocalOwnedMountPresentMs_ = 0U;
    localMountGeneration_ = 1U;
    lastLocalMountState_.reset();
    remoteMountState_.reset();
    pendingRemoteMountAbsentState_.reset();
    remoteMountAbsentSinceMs_ = 0U;
    remoteMountAbsenceConfirmed_ = false;
    nextRemoteMountMaintainMs_ = 0U;
    remoteMountSequences_.Reset();
    remotePlayerActionSequences_.Reset();
    facade_.ClearRemoteMount();
    localShotAimTarget_ = {};
    localShotLatchExpiresMs_ = 0U;
    localTraversalAnchor_ = {};
    localTraversalApproachVelocity_ = {};
    localTraversalObstaclePoint_ = {};
    localTraversalObstacleNormal_ = {};
    localTraversalExpectedLanding_ = {};
    previousLocalPosition_ = {};
    localTraversalHeading_ = 0.0F;
    localTraversalObstacleTopZ_ = 0.0F;
    previousLocalHeading_ = 0.0F;
    localTraversalLatchExpiresMs_ = 0U;
    localLocomotionEpoch_ = 1U;
    localTraversalActionId_ = 0U;
    localTraversalRevision_ = 0U;
    localTraversalSentRevision_ = 0U;
    localTraversalKind_ = PlayerTraversalKind::None;
    localTraversalFlags_ = 0U;
    previousLocalFiring_ = false;
    previousLocalJumping_ = false;
    previousLocalClimbing_ = false;
    localTraversalBecameActive_ = false;
    localTraversalLandingCaptured_ = false;
    previousLocalMounted_ = false;
    hasPreviousLocalTransform_ = false;
    previousLocalLifecycle_.reset();
    hasPreviousLocalWeaponSample_ = false;
    soloOverride_ = false;
    cutsceneSpectator_ = false;
    telemetry_.Reset();
    bubble_.Reset();
    if (preserveHostMissionAuthority) {
        localMissionState_ = preserved.missionState;
        localMissionCinematicState_ = preserved.cinematicState;
        localMissionEpoch_ = preserved.missionEpoch;
        localMissionRevision_ = preserved.missionRevision;
        localMissionCinematicGeneration_ =
            preserved.cinematicGeneration;
        localMissionCinematicRevision_ = preserved.cinematicRevision;
        localMissionCinematicActionId_ = preserved.cinematicActionId;
        localMissionCameraRevision_ = preserved.cameraRevision;
        localAnimSceneRevision_ = preserved.animSceneRevision;
        localCheckpointGeneration_ = preserved.checkpointGeneration;
        localMissionRecoveryUntilMs_ =
            preserved.missionRecoveryUntilMs;
        localSampleUnavailableSinceMs_ =
            preserved.sampleUnavailableSinceMs;
        localCinematicControlRecoveredSinceMs_ =
            preserved.controlRecoveredSinceMs;
        localCinematicPrepareStartedMs_ = preserved.prepareStartedMs;
        localCinematicTerminalClearSinceMs_ =
            preserved.terminalClearSinceMs;
        localCinematicCameraWaitStartedMs_ =
            preserved.cameraWaitStartedMs;
        localCinematicCameraWaitWarned_ = preserved.cameraWaitWarned;
        previousLocalMissionActive_ = preserved.previousMissionActive;
        localMissionInitialized_ = preserved.missionInitialized;
        localMissionCameraActive_ = preserved.missionCameraActive;
        localAnimSceneActive_ = preserved.animSceneActive;
        localCinematicCameraReady_ = preserved.cameraReady;
        localCinematicPresentationReady_ = preserved.presentationReady;
        localCinematicResumeReady_ = preserved.resumeReady;
    }
    if (reconnect) {
        localFireSequence_ = preservedOutbound.fireSequence;
        localAnimationSampleSequence_ =
            preservedOutbound.animationSampleSequence;
        localPlayerActionSequence_ =
            preservedOutbound.playerActionSequence;
        localPlayerActionId_ = preservedOutbound.playerActionId;
        localMissionCinematicActionId_ =
            preservedOutbound.missionCinematicActionId;
        localPlayerActions_ = preservedOutbound.playerActions;
        localAppearanceRevision_ =
            preservedOutbound.appearanceRevision;
        lastLocalAppearance_ = preservedOutbound.lastAppearance;
        localMountGeneration_ = preservedOutbound.mountGeneration;
        localLocomotionEpoch_ = preservedOutbound.locomotionEpoch;
        localTraversalActionId_ = preservedOutbound.traversalActionId;
        localTraversalRevision_ = preservedOutbound.traversalRevision;
        localTraversalSentRevision_ =
            preservedOutbound.traversalSentRevision;
        synchronizedPaused_ = preservedOutbound.synchronizedPaused;
        hostPauseVoted_ = preservedOutbound.hostPauseVoted;
        guestPauseVoted_ = preservedOutbound.guestPauseVoted;
        pauseVoteGeneration_ =
            preservedOutbound.pauseVoteGeneration;
    }
    Frame frame;
    frame.header.type = MessageType::Hello;
    frame.header.sequence = sequencer_.Next();
    frame.header.tick = facade_.TickMilliseconds();
    SendBestEffort(std::move(frame));
}

GuestMissionIsolationStatus
BridgeRuntime::AcquireGuestMissionIsolationLease(
    const std::string_view reason) {
    guestMissionIsolationLeaseActive_ = true;
    const auto status = facade_.MaintainMissionAuthority(
        true,
        false,
        false);
    guestMissionQuarantineActive_ = status.quarantineActive;
    facade_.Log(
        "[MISSION_ISOLATION][LEASE] acquired before guest session: " +
        std::string{reason});
    return status;
}

void BridgeRuntime::ReleaseGuestMissionIsolationLease(
    const std::string_view reason) noexcept {
    if (!guestMissionIsolationLeaseActive_) {
        return;
    }
    guestMissionIsolationLeaseActive_ = false;
    (void)facade_.MaintainMissionAuthority(false, false, false);
    guestMissionQuarantineActive_ = false;
    guestQuarantineSkipUntilMs_ = 0U;
    guestPostCinematicSkipUntilMs_ = 0U;
    guestMissionQuarantineSkipLatched_ = false;
    previousGuestLocalMissionDetected_ = false;
    pendingMissionGuestTeleport_.reset();
    try {
        facade_.Log(
            "[MISSION_ISOLATION][LEASE] released: " +
            std::string{reason});
    } catch (...) {
        // Releasing the native sentinel must remain noexcept during unload.
    }
}

void BridgeRuntime::AcceptHelloAck(
    const std::span<const std::uint8_t> payload) {
    if (payload.size() != 1U ||
        payload.front() >
            static_cast<std::uint8_t>(PlayerSlot::Guest)) {
        facade_.Log(
            "invalid pipe HelloAck; role remains unnegotiated");
        transport_.Disconnect();
        nextReconnectMs_ = facade_.TickMilliseconds() + 1'000U;
        return;
    }

    const auto slot = static_cast<PlayerSlot>(payload.front());
    const auto rejectRoleChange = [this](const std::string_view reason) {
        if (localSlot_.has_value()) {
            helloExpectedRole_ = localSlot_;
        }
        facade_.Log(std::string{reason});
        facade_.MaintainMissionResumeBarrier(false);
        facade_.MaintainMissionSpectator(false);
        facade_.MaintainRemoteMissionParticipant(false);
        facade_.SetAnimSceneCaptureAuthority(false);
        ResetAnimSceneHybridState(true);
        localAnimSceneControlActionId_ = 0U;
        localFireSequence_ = 0U;
        localAnimationSampleSequence_ = 0U;
        localPlayerActionSequence_ = 0U;
        localPlayerActionId_ = 0U;
        localPlayerActions_.fill(LocalPlayerActionRuntime{});
        localAppearanceRevision_ = 1U;
        lastLocalAppearance_.reset();
        localMountGeneration_ = 1U;
        localLocomotionEpoch_ = 1U;
        localTraversalActionId_ = 0U;
        localTraversalRevision_ = 0U;
        localTraversalSentRevision_ = 0U;
        ResetPauseVoteState(false);
        ResetHostWorldMirror(false);
        ResetGuestWorldMirror();
        localMissionState_.reset();
        localMissionCinematicState_.reset();
        localMissionInitialized_ = false;
        previousLocalMissionActive_ = false;
        localMissionCinematicGeneration_ = 0U;
        localMissionCinematicRevision_ = 0U;
        localMissionCinematicActionId_ = 0U;
        localCinematicPrepareStartedMs_ = 0U;
        localCinematicResumeReady_ = false;
        localCinematicPresentationReady_ = false;
        localCinematicCameraReady_ = false;
        localCinematicTerminalLatchActive_ = false;
        transport_.Disconnect();
        localSlot_.reset();
        localEntityId_ = NetEntityId{};
        playerEntityIds_.fill(NetEntityId{});
        nextReconnectMs_ = facade_.TickMilliseconds() + 1'000U;
    };
    if (helloExpectedRole_.has_value() &&
        *helloExpectedRole_ != slot) {
        rejectRoleChange(
            "conflicting pipe HelloAck role after reconnect; preserved host cinematic was released safely and the connection was refused");
        return;
    }
    if (localSlot_.has_value() && *localSlot_ != slot) {
        rejectRoleChange(
            "conflicting duplicate pipe HelloAck role; connection refused");
        return;
    }
    if (!helloExpectedRole_.has_value() &&
        localSlot_.has_value() && *localSlot_ == slot) {
        // A repeated acknowledgement on the already-authenticated pipe is idempotent.
        // In particular, do not clear playerEntityIds_: the next PlayerState must remain bound to the established remote identity, and authenticated control lanes depend on that binding.
        // SendHello sets helloExpectedRole_ for a real reconnect, so its acknowledgement still runs the full role/session restoration below.
        facade_.Log("duplicate same-role pipe HelloAck ignored");
        return;
    }

    const bool completingReconnect = helloExpectedRole_.has_value();
    localSlot_ = slot;
    helloExpectedRole_.reset();
    // Only the authoritative host observes Story VM native calls.
    // The guest still uses the exact-build handler validation to create its own bridge-owned scene, but must never detour game-owned private scenes.
    facade_.SetAnimSceneCaptureAuthority(slot == PlayerSlot::Host);
    if (slot == PlayerSlot::Guest) {
        hostInviteCode_.clear();
        if (!guestMissionIsolationLeaseActive_) {
            (void)AcquireGuestMissionIsolationLease(
                "authenticated guest role");
        }
    } else if (guestMissionIsolationLeaseActive_) {
        // A pending JOIN that is answered with a host role is a failed role negotiation, not a guest session.
        // Do not retain a stale lease.
        ReleaseGuestMissionIsolationLease(
            "authenticated host role replaced pending JOIN");
    }
    localEntityId_ = NetEntityId::Compose(
        sessionEpoch_,
        slot == PlayerSlot::Host ? 1U : 2U);
    playerEntityIds_.fill(NetEntityId{});
    playerEntityIds_[SlotIndex(slot)] = localEntityId_;
    const bool restoredHostCinematic =
        slot == PlayerSlot::Host &&
        hostAnimSceneReconnectPending_ &&
        localMissionCinematicState_.has_value() &&
        IsCinematicPresentationPhase(
            localMissionCinematicState_->phase);
    if (slot == PlayerSlot::Host && completingReconnect) {
        // SendHello(reconnect) preserves the host graph and its stable IDs.
        // Replay those spawns after role restoration so the sidecar/guest can rebuild any state lost with the game pipe before live deltas resume.
        forceHostWorldMirrorReplay_ =
            worldMirrorHost_.has_value() &&
            worldMirrorHost_->Size() != 0U;
    }
    if (restoredHostCinematic) {
        if (HasActiveHostAnimSceneDefinition()) {
            forceHostWorldMirrorReplay_ = true;
            localAnimSceneDefinitionSentAtMs_ = 0U;
            facade_.Log(
                "[ANIMSCENE_HYBRID][PIPE_RECONNECT] host role restored; stable world spawns will precede definition replay");
        } else {
            hostAnimSceneReconnectPending_ = false;
            facade_.Log(
                "[ANIMSCENE_HYBRID][PIPE_RECONNECT] host role restored; SAFE_FALLBACK cinematic snapshot and resume barrier remain authoritative");
        }
    } else if (hostAnimSceneReconnectPending_) {
        facade_.MaintainMissionResumeBarrier(false);
        ResetAnimSceneHybridState(true);
        ResetHostWorldMirror(false);
        localMissionState_.reset();
        localMissionCinematicState_.reset();
        localMissionInitialized_ = false;
        previousLocalMissionActive_ = false;
        localMissionCinematicGeneration_ = 0U;
        localMissionCinematicRevision_ = 0U;
        localCinematicPrepareStartedMs_ = 0U;
        localCinematicResumeReady_ = false;
        facade_.Log(
            "[ANIMSCENE_HYBRID][PIPE_RECONNECT] role/generation changed during Hello; retained exact state was discarded safely");
    }
    players_.SetAlive(PlayerSlot::Host);
    players_.SetAlive(PlayerSlot::Guest);
    telemetry_.Reset();
    facade_.Log(
        slot == PlayerSlot::Host
            ? "pipe role negotiated: host"
            : "pipe role negotiated: guest");
    sessionMenu_.MarkSessionReady(
        slot == PlayerSlot::Host,
        slot == PlayerSlot::Host
            ? "Host code ready. REMOTE NONE means waiting for the guest."
            : "Code accepted locally. Wait for REMOTE STREAMING.");
}

void BridgeRuntime::ApplyRemotePlayerState(
    const PlayerStatePayload& state,
    const std::uint32_t sequence,
    const std::uint64_t senderTickMs) {
    // Ignore pre-role messages and never render our own network state as a second local player replica.
    if (!localSlot_.has_value() || state.slot == *localSlot_) {
        return;
    }

    const auto expectedRemoteId =
        playerEntityIds_[SlotIndex(state.slot)];
    if (expectedRemoteId.IsValid() &&
        expectedRemoteId != state.entityId) {
        facade_.Log(
            "[WARNING][MISSION_RX] rejected PlayerState whose identity disagrees with authenticated mission state");
        return;
    }

    // A new player ID means this is a new copy of that player, not just movement.
    // Remove the old copy before making the new one.
    if (remoteReplicaId_.IsValid() &&
        remoteReplicaId_ != state.entityId) {
        DespawnRemoteReplica();
    }

    // UDP can arrive in the wrong order.
    // Ignore an old update instead of moving the remote player backwards.
    const auto disposition =
        remotePlayerSequences_.Observe(sequence);
    if (disposition == SequenceDisposition::Duplicate ||
        disposition == SequenceDisposition::Stale) {
        return;
    }

    // The first accepted state creates a local RDR2 proxy.
    // Appearance, identity and equipment were received independently and are applied if already known.
    if (!remoteReplicaId_.IsValid()) {
        const CommandPayload spawn{
            CommandOpcode::SpawnReplica,
            0U,
            state.entityId,
            state.position,
            state.heading,
            state.healthFraction};
        if (!facade_.ApplyNetworkCommand(spawn)) {
            if (diagnostics_) {
                facade_.Log("failed to spawn remote player replica");
            }
            return;
        }
        remoteReplicaId_ = state.entityId;
        playerEntityIds_[SlotIndex(state.slot)] = state.entityId;
        remoteLifecycle_.reset();
        remoteRestraintState_.reset();
        if (!remoteParticipantSceneIsolated_ &&
            remoteIdentity_.has_value() &&
            remoteIdentity_->entityId == state.entityId &&
            remoteIdentity_->slot == state.slot) {
            (void)facade_.ApplyRemoteIdentity(*remoteIdentity_);
        }
        if (!remoteParticipantSceneIsolated_ &&
            remoteAppearance_.has_value() &&
            remoteAppearance_->entityId == state.entityId &&
            remoteAppearance_->slot == state.slot) {
            (void)facade_.ApplyRemoteAppearance(*remoteAppearance_);
        }
        if (!remoteParticipantSceneIsolated_ &&
            remoteEquipment_.has_value() &&
            remoteEquipment_->entityId == state.entityId) {
            (void)facade_.ApplyRemoteEquipment(*remoteEquipment_);
        }
    }
    ApplyPendingRestraintState(state.entityId);

    const auto receivedAtMs = previousTickMs_;
    if (lastRemoteStateMs_.has_value() &&
        receivedAtMs >= *lastRemoteStateMs_) {
        motionArrivalGapMaximumMs_ = std::max(
            motionArrivalGapMaximumMs_,
            receivedAtMs - *lastRemoteStateMs_);
    }
    // Save a few positions instead of moving the game player straight away.
    // The game uses those saved positions to move them smoothly.
    if (!remoteSnapshots_.Push(
            state,
            receivedAtMs,
            senderTickMs)) {
        if (diagnostics_) {
            facade_.Log("rejected invalid remote player snapshot");
        }
        return;
    }

    const bool wasIncapacitated =
        remoteLifecycle_.has_value() &&
        (*remoteLifecycle_ == PlayerLifecycle::Downed ||
         *remoteLifecycle_ == PlayerLifecycle::Reviving);
    const bool nowIncapacitated =
        state.lifecycle == PlayerLifecycle::Downed ||
        state.lifecycle == PlayerLifecycle::Reviving;
    if (nowIncapacitated &&
        (!remoteLifecycle_.has_value() || !wasIncapacitated)) {
        (void)facade_.ApplyNetworkCommand(
            CommandPayload{
                CommandOpcode::EnterDowned,
                0U,
                state.entityId,
                state.position,
                state.heading,
                state.healthFraction});
    } else if (
        state.lifecycle == PlayerLifecycle::Alive &&
        wasIncapacitated) {
        (void)facade_.ApplyNetworkCommand(
            CommandPayload{
                CommandOpcode::CompleteRevive,
                0U,
                state.entityId,
                state.position,
                state.heading,
                state.healthFraction});
    }

    remoteLifecycle_ = state.lifecycle;
    latestRemoteState_ = state;
    lastRemoteStateMs_ = receivedAtMs;
    if (awaitingRestoredGuestStream_ &&
        localSlot_ == PlayerSlot::Host &&
        state.slot == PlayerSlot::Guest) {
        awaitingRestoredGuestStream_ = false;
        facade_.Log(
            "[MISSION_RESUME_BARRIER][PIPE_RECONNECT] fresh authenticated guest stream restored; normal ResumeReady/no-guest policy resumed");
    }
    if (hostWorldReplayAwaitingGuest_ &&
        localSlot_ == PlayerSlot::Host &&
        state.slot == PlayerSlot::Guest) {
        hostWorldReplayAwaitingGuest_ = false;
        hostWorldReplayGuestDeadlineMs_ = 0U;
        facade_.Log(
            "[ENTITY_GRAPH_HOST][PIPE_RECONNECT] fresh guest stream restored; stable world replay may proceed");
    }
    if (nowIncapacitated) {
        players_.SetDowned(state.slot);
    } else if (state.lifecycle == PlayerLifecycle::Alive) {
        players_.SetAlive(state.slot, state.healthFraction);
    }
}

void BridgeRuntime::DespawnRemoteReplica() noexcept {
    // Remove the remote player copy and clear its saved network data.
    // This is used when reconnecting or when the copy breaks.
    facade_.MaintainMissionCompanionPresentation({});
    facade_.MaintainReplicatedMissionCamera(false, std::nullopt);
    (void)facade_.MaintainReplicatedAnimScene(false, std::nullopt);
    const bool preserveHostDefinition =
        HasActiveHostAnimSceneDefinition();
    ResetAnimSceneHybridState(true, preserveHostDefinition);
    ResetPauseVoteState(false);
    worldDamageIntentSequences_.Reset();
    lastWorldDamageIntentMs_ = 0U;
    remoteSnapshots_.Reset();
    latestRemoteState_.reset();
    latestRemoteAnimationState_.reset();
    latestRemoteAnimationReceivedAtMs_.reset();
    latestRemoteAnimationSenderTickMs_ = 0U;
    lastRemotePlayerActionReceivedAtMs_.reset();
    remoteIdentity_.reset();
    remoteAppearance_.reset();
    remoteEquipment_.reset();
    remoteMountState_.reset();
    pendingRemoteMountAbsentState_.reset();
    remoteMountAbsentSinceMs_ = 0U;
    remoteMountAbsenceConfirmed_ = false;
    nextRemoteMountMaintainMs_ = 0U;
    remoteMountSequences_.Reset();
    facade_.ClearRemoteMount();
    remoteLifecycle_.reset();
    remoteRestraintState_.reset();
    localInteraction_ = LocalInteractionRuntime{};
    interactionResultSequences_.Reset();
    restraintStateSequences_.Reset();
    remotePlayerSequences_.Reset();
    remoteMissionCameraSequences_.Reset();
    remoteMissionCameraState_.reset();
    remoteMissionCameraReceivedAtMs_.reset();
    remoteAnimSceneSequences_.Reset();
    remoteAnimSceneState_.reset();
    remoteAnimSceneReceivedAtMs_.reset();
    remoteNativeAnimSceneActive_ = false;
    remoteAnimationSequences_.Reset();
    remoteAnimationPayloadSequences_.Reset();
    lastRemoteStateMs_.reset();
    consecutiveMotionApplyFailures_ = 0U;
    motionSourceAgeMaximumMs_ = 0U;
    motionArrivalGapMaximumMs_ = 0U;
    if (!remoteReplicaId_.IsValid()) {
        return;
    }
    (void)facade_.ApplyNetworkCommand(
        CommandPayload{
            CommandOpcode::DespawnReplica,
            0U,
            remoteReplicaId_,
            {},
            0.0F,
            0.0F});
    if (localSlot_.has_value()) {
        playerEntityIds_[SlotIndex(OtherSlot(*localSlot_))] =
            NetEntityId{};
    }
    remoteReplicaId_ = NetEntityId{};
}

void BridgeRuntime::Tick() {
    if (!active_ || shouldUnload_) {
        return;
    }

    pauseStateChangedByRemoteThisTick_ = false;

    lastTickStage_ = "clock-and-mode";
    const auto now = facade_.TickMilliseconds();
    const auto elapsed = ElapsedMilliseconds(previousTickMs_, now);
    previousTickMs_ = now;
    ++runtimeDiagnosticTickCount_;
    runtimeDiagnosticTickElapsedSumMs_ += elapsed;
    runtimeDiagnosticTickMaximumMs_ = std::max(
        runtimeDiagnosticTickMaximumMs_,
        elapsed);
    if (elapsed >= kRuntimeDiagnosticHitchMilliseconds) {
        ++runtimeDiagnosticHitchCount_;
    }

    const auto mode = facade_.QueryRuntimeMode();
    if (mode.onlineSessionActive || !mode.storyModeKnown ||
        !mode.isStoryMode) {
        facade_.Log(
            "Story Mode guard changed state; bridge is stopping fail-closed");
        Stop("offline Story Mode guard failed");
        shouldUnload_ = true;
        return;
    }

    lastTickStage_ = "sidecar-transport";
    if (!transport_.IsConnected() && now >= nextReconnectMs_) {
        std::string connectError;
        if (transport_.Connect(connectError)) {
            SendHello(true);
            facade_.Log("sidecar named pipe reconnected");
        } else {
            nextReconnectMs_ = now + 1'000U;
            if (diagnostics_) {
                facade_.Log(connectError);
            }
        }
    }

    if (transport_.IsConnected()) {
        std::string receiveError;
        auto frames = transport_.Poll(receiveError);
        for (const auto& frame : frames) {
            // HelloAck and forwarded LAN frames use independent sequence domains.
            // A global window here would drop valid remote frames.
            HandleInboundFrame(frame);
        }
        if (!receiveError.empty()) {
            facade_.Log(receiveError);
            nextReconnectMs_ = now + 1'000U;
        }
    }

    lastTickStage_ = "menus-and-input";
    sessionMenu_.SetSidecarConnected(transport_.IsConnected());
    const bool sessionMenuWasOpen = sessionMenu_.IsOpen();
    const bool commandMenuWasOpen = menu_.IsOpen();
    const auto input = facade_.ReadMenuInput();
    const bool quickMarkerPressedEdge =
        input.f7 && !previousQuickMarkerPressed_;
    previousQuickMarkerPressed_ = input.f7;
    if (quickMarkerPressedEdge) {
        HandleMenuCommand(BridgeCommand::SaveProblemMarker);
    }
    const bool escapePressed = input.cancel;
    const bool escapePressedEdge =
        escapePressed && !previousEscapePressed_;
    previousEscapePressed_ = escapePressed;
    const auto sessionUpdate = sessionMenu_.Update(input);
    if (sessionUpdate.action.has_value()) {
        HandleSessionOverlayAction(*sessionUpdate.action);
    }
    facade_.DrawSessionMenu(sessionMenu_.View());

    if (sessionMenu_.IsOpen()) {
        menu_.Close();
    } else {
        const auto menuUpdate = menu_.Update(input);
        if (menuUpdate.command.has_value()) {
            HandleMenuCommand(*menuUpdate.command);
        }
    }
    facade_.DrawMenu(
        menu_.IsOpen(),
        menu_.Commands(),
        menu_.Selection());
    if (!notificationText_.empty() && now <= notificationUntilMs_) {
        facade_.DrawNotification(notificationText_, true);
    } else if (notificationUntilMs_ != 0U) {
        notificationUntilMs_ = 0U;
        notificationText_.clear();
    }
    lastTickStage_ = "remote-mount";
    if (pendingRemoteMountAbsentState_.has_value() &&
        remoteMountAbsentSinceMs_ != 0U &&
        (now < remoteMountAbsentSinceMs_ ||
             now - remoteMountAbsentSinceMs_ >=
             kRemoteMountAbsentDebounceMilliseconds)) {
        const auto absentGeneration =
            pendingRemoteMountAbsentState_->generation;
        facade_.ClearRemoteMount();
        remoteMountState_.reset();
        pendingRemoteMountAbsentState_.reset();
        remoteMountAbsentSinceMs_ = 0U;
        remoteMountAbsenceConfirmed_ = true;
        nextRemoteMountMaintainMs_ = 0U;
        facade_.Log(
            "[MOUNT_LIFECYCLE] direction=rx, state=absent, correlation=mount-" +
            std::to_string(absentGeneration) +
            ", generation=" +
            std::to_string(absentGeneration) +
            ", reason=remote-absence-debounce");
        if (diagnostics_) {
            facade_.Log(
                "remote player mount absence confirmed after debounce");
        }
    }
    lastTickStage_ = "remote-mount-presentation";
    if (remoteReplicaId_.IsValid() &&
        !remoteParticipantSceneIsolated_ &&
        remoteMountState_.has_value() &&
        (nextRemoteMountMaintainMs_ == 0U ||
         now >= nextRemoteMountMaintainMs_) &&
        !facade_.MaintainRemoteMount(
            *remoteMountState_,
            lastLocalMountState_) &&
        diagnostics_) {
        facade_.Log(
            "remote player mount state could not be maintained");
    }
    if (remoteReplicaId_.IsValid() &&
        !remoteParticipantSceneIsolated_ &&
        remoteMountState_.has_value() &&
        (nextRemoteMountMaintainMs_ == 0U ||
         now >= nextRemoteMountMaintainMs_)) {
        nextRemoteMountMaintainMs_ =
            now + kRemoteMountMaintainIntervalMilliseconds;
    }
    if (remoteReplicaId_.IsValid() &&
        !remoteParticipantSceneIsolated_) {
        if (const auto rendered = remoteSnapshots_.Sample(now);
            rendered.has_value()) {
            const auto modeIndex =
                static_cast<std::size_t>(rendered->mode);
            if (modeIndex < motionSampleCounts_.size()) {
                ++motionSampleCounts_[modeIndex];
            }
            motionSourceAgeMaximumMs_ = std::max(
                motionSourceAgeMaximumMs_,
                rendered->sourceAgeMs);
            if (latestRemoteAnimationReceivedAtMs_.has_value() &&
                (now < *latestRemoteAnimationReceivedAtMs_ ||
                 now - *latestRemoteAnimationReceivedAtMs_ >
                     kRemoteAnimationStateCacheTtlMs)) {
                latestRemoteAnimationState_.reset();
                latestRemoteAnimationReceivedAtMs_.reset();
                latestRemoteAnimationSenderTickMs_ = 0U;
                ++animationSamplesExpired_;
            }
            const bool animationFresh =
                motionReplicationMode_ ==
                    MotionReplicationWireMode::AnimGraphReplica &&
                latestRemoteAnimationState_.has_value() &&
                latestRemoteAnimationReceivedAtMs_.has_value() &&
                latestRemoteAnimationState_->entityId ==
                    rendered->state.entityId &&
                latestRemoteAnimationState_->locomotionEpoch ==
                    rendered->state.locomotionEpoch &&
                IsRemoteAnimationStateFresh(
                    *latestRemoteAnimationReceivedAtMs_,
                    latestRemoteAnimationSenderTickMs_,
                    rendered->senderTickMs,
                    now);
            if (animationFresh) {
                lastTickStage_ = "remote-animation-presentation";
                if (!facade_.ApplyRemoteAnimationState(
                        *latestRemoteAnimationState_)) {
                    ++animationSamplesRejected_;
                }
            }
            lastTickStage_ = "remote-transform-presentation";
            if (!facade_.ApplyRemoteTransform(rendered->state)) {
                ++motionApplyFailures_;
                ++consecutiveMotionApplyFailures_;
                if (consecutiveMotionApplyFailures_ >= 3U) {
                    const auto cachedIdentity = remoteIdentity_;
                    const auto cachedEquipment = remoteEquipment_;
                    facade_.Log(
                        "remote replica became unavailable; scheduling automatic respawn");
                    DespawnRemoteReplica();
                    remoteIdentity_ = cachedIdentity;
                    remoteEquipment_ = cachedEquipment;
                }
            } else {
                consecutiveMotionApplyFailures_ = 0U;
            }
        }
    }
    if (now >= nextMotionDiagnosticsMs_) {
        if (remoteReplicaId_.IsValid()) {
            facade_.Log(
                "motion v13.1/sender-timeline/5s: hold=" +
                std::to_string(motionSampleCounts_[0]) +
                ", interpolated=" +
                std::to_string(motionSampleCounts_[1]) +
                ", extrapolated=" +
                std::to_string(motionSampleCounts_[2]) +
                ", frozen=" +
                std::to_string(motionSampleCounts_[3]) +
                ", apply-failed=" +
                std::to_string(motionApplyFailures_) +
                ", source-age-max-ms=" +
                std::to_string(motionSourceAgeMaximumMs_) +
                ", arrival-gap-max-ms=" +
                std::to_string(motionArrivalGapMaximumMs_) +
                ", interpolation-delay-ms=" +
                std::to_string(
                    remoteSnapshots_.InterpolationDelayMs()) +
                ", arrival-jitter-ms=" +
                std::to_string(
                    remoteSnapshots_.ArrivalJitterMs()) +
                ", sender-timeline-resets=" +
                std::to_string(
                    remoteSnapshots_.SenderTimelineResets()) +
                ", motion-engine=" +
                std::string{
                    motionReplicationMode_ ==
                            MotionReplicationWireMode::AnimGraphReplica
                        ? "animgraph-direct"
                        : "task-navmesh"} +
                ", anim-sent=" +
                std::to_string(animationSamplesSent_) +
                ", anim-received=" +
                std::to_string(animationSamplesReceived_) +
                ", anim-rejected=" +
                std::to_string(animationSamplesRejected_) +
                ", anim-expired=" +
                std::to_string(animationSamplesExpired_));
        }
        facade_.Log(
            "[ACTION_TX] v20/reliable/5s sent=" +
            std::to_string(playerActionsSent_) +
            ", received=" +
            std::to_string(playerActionsReceived_) +
            ", duplicate=" +
            std::to_string(playerActionsDuplicate_) +
            ", stale=" +
            std::to_string(playerActionsStale_) +
            ", rejected=" +
            std::to_string(playerActionsRejected_));
        motionSampleCounts_.fill(0U);
        motionApplyFailures_ = 0U;
        motionSourceAgeMaximumMs_ = 0U;
        motionArrivalGapMaximumMs_ = 0U;
        animationSamplesSent_ = 0U;
        animationSamplesReceived_ = 0U;
        animationSamplesRejected_ = 0U;
        animationSamplesExpired_ = 0U;
        playerActionsSent_ = 0U;
        playerActionsReceived_ = 0U;
        playerActionsDuplicate_ = 0U;
        playerActionsStale_ = 0U;
        playerActionsRejected_ = 0U;
        nextMotionDiagnosticsMs_ =
            now + kMotionDiagnosticsIntervalMilliseconds;
    }
    if (remoteReplicaId_.IsValid() &&
        lastRemoteStateMs_.has_value() &&
        ElapsedMilliseconds(*lastRemoteStateMs_, now) >
            kRemotePlayerDespawnTimeoutMilliseconds) {
        const bool awaitGuestAfterStreamTimeout =
            localSlot_ == PlayerSlot::Host &&
            localMissionCinematicState_.has_value() &&
            IsCinematicPresentationPhase(
                localMissionCinematicState_->phase) &&
            !localCinematicResumeReady_ &&
            (remoteReplicaId_.IsValid() ||
             awaitingRestoredGuestStream_);
        facade_.Log(
            "remote stream timed out; despawning stale replica and decorations");
        if (localSlot_ == PlayerSlot::Host) {
            if (HasActiveHostAnimSceneDefinition()) {
                forceHostWorldMirrorReplay_ = true;
                (void)BeginHostAnimScenePrepareAttempt(
                    "remote-stream-timeout");
                facade_.Log(
                    "[ANIMSCENE_HYBRID][STREAM_TIMEOUT] retained the active host world graph and scheduled stable spawn replay so cached role NetEntityIds remain valid after stream recovery");
            } else {
                ResetHostWorldMirror(true);
            }
        } else {
            ResetGuestWorldMirror();
        }
        DespawnRemoteReplica();
        awaitingRestoredGuestStream_ =
            awaitGuestAfterStreamTimeout;
    }
    const bool remoteStreaming =
        remoteReplicaId_.IsValid() &&
        lastRemoteStateMs_.has_value() &&
        ElapsedMilliseconds(*lastRemoteStateMs_, now) <=
            kRemotePlayerFreshnessMilliseconds;
    if (remoteStreaming && !previousRemoteStreaming_ &&
        localSlot_ == PlayerSlot::Host &&
        localMissionCinematicState_.has_value() &&
        IsCinematicPresentationPhase(
            localMissionCinematicState_->phase)) {
        // Rebuild authority in dependency order on the first recovered stream tick: mission/cinematic identity, live presentation samples, stable world roles, then the cached exact definition.
        nextMissionStateHeartbeatMs_ = now;
        nextMissionCinematicHeartbeatMs_ = now;
        nextMissionCameraSampleMs_ = now;
        nextAnimSceneSampleMs_ = now;
        facade_.Log(
            "[ANIMSCENE_HYBRID][STREAM_RECOVERY] forcing mission/cinematic authority before stable world spawns and definition replay");
    }
    if (!remoteStreaming && previousRemoteStreaming_) {
        ResetPauseVoteState(false);
    } else if (
        remoteStreaming &&
        escapePressedEdge &&
        !pauseStateChangedByRemoteThisTick_ &&
        !sessionMenuWasOpen &&
        !commandMenuWasOpen) {
        HandleLocalPauseToggle();
    }
    previousRemoteStreaming_ = remoteStreaming;

    lastTickStage_ = "local-player-sample";
    auto sample = facade_.SampleLocalPlayer();
    if (!sample.has_value() &&
        localSlot_ == PlayerSlot::Host) {
        TickMissionLoadingAuthority(now);
    } else if (sample.has_value()) {
        localSampleUnavailableSinceMs_ = 0U;
    }
    if (!sample.has_value() && localSlot_.has_value()) {
        TickMissionCinematic(sample, *localSlot_, now);
    }
    const bool hostMissionActive =
        remoteMissionState_.has_value()
            ? (remoteMissionState_->flags &
               static_cast<std::uint8_t>(
                   MissionStateFlag::MissionActive)) != 0U
            : latestRemoteState_.has_value() &&
                  (latestRemoteState_->flags &
                   static_cast<std::uint32_t>(
                       PlayerStateFlag::InMission)) != 0U;
    const bool guestMissionIsolationEnabled =
        guestMissionIsolationLeaseActive_;
    // This is intentionally narrower than the lease: only a host-issued, exact-ID start barrier can let a guest reach their own vanilla mission prompt.
    // The regular guard resumes automatically on timeout, rejection, or after that exact local MissionData entry ends.
    const bool guestMatchingMissionStartPermitted =
        localSlot_ == PlayerSlot::Guest &&
        remoteMissionStartBarrier_.has_value() &&
        !remoteMissionStartBarrierRejected_ &&
        remoteMissionStartBarrierDeadlineMs_ != 0U &&
        remoteMissionStartBarrierPromptArmedAtMs_ != 0U &&
        now >= remoteMissionStartBarrierPromptArmedAtMs_ &&
        now <= remoteMissionStartBarrierDeadlineMs_;
    const bool hostPresentationActive =
        (remoteMissionCinematicState_.has_value() &&
         IsCinematicPresentationPhase(
             remoteMissionCinematicState_->phase)) ||
        (remoteMissionState_.has_value() &&
         (remoteMissionState_->phase == MissionPhase::Recovery ||
          remoteMissionState_->phase == MissionPhase::SoloOverride ||
          (!remoteMissionCinematicState_.has_value() &&
           (remoteMissionState_->phase == MissionPhase::Cutscene ||
            remoteMissionState_->phase == MissionPhase::Loading))));
    lastTickStage_ = "mission-isolation";
    const auto missionIsolation = facade_.MaintainMissionAuthority(
        guestMissionIsolationEnabled,
        hostMissionActive,
        hostPresentationActive,
        guestMatchingMissionStartPermitted);
    guestMissionQuarantineActive_ =
        guestMissionIsolationEnabled &&
        missionIsolation.quarantineActive;
    if (guestMissionIsolationEnabled &&
        missionIsolation.localMissionDetected &&
        !previousGuestLocalMissionDetected_) {
        ++guestMissionIsolationDetections_;
        facade_.Log(
            "[WARNING][MISSION_ISOLATION][MISSION_FSM] guest-local Story mission transition detected; local mission state quarantined instead of being presented as the host mission");
    }
    previousGuestLocalMissionDetected_ =
        guestMissionIsolationEnabled &&
        missionIsolation.localMissionDetected;
    if (guestMissionQuarantineActive_) {
        ++guestMissionQuarantineTicks_;
    }
    // The guest's process-local Story state is never network authority.
    // Do not advertise an accidental local mission/camera transition to the host.
    if (sample.has_value() && guestMissionIsolationEnabled &&
        !guestMatchingMissionStartPermitted) {
        sample->missionActive = false;
        sample->cutsceneActive = false;
    }
    if (guestMissionIsolationEnabled &&
        now >= nextMissionIsolationDiagnosticsMs_) {
        facade_.Log(
            "[MISSION_ISOLATION][MISSION_PREFLIGHT] full-lease-guard host-mission-active=" +
            std::to_string(hostMissionActive ? 1 : 0) +
            ", local-transition=" +
            std::to_string(missionIsolation.localMissionDetected ? 1 : 0) +
            ", quarantine=" +
            std::to_string(guestMissionQuarantineActive_ ? 1 : 0) +
            ", prompt-guard=" +
            std::to_string(
                missionIsolation.localStoryInteractionSuppressed ? 1 : 0) +
            ", mission-gate-asserted=" +
            std::to_string(
                missionIsolation.missionGateAsserted ? 1 : 0) +
            ", detections=" +
            std::to_string(guestMissionIsolationDetections_) +
            ", quarantine-ticks=" +
            std::to_string(guestMissionQuarantineTicks_));
        nextMissionIsolationDiagnosticsMs_ =
            now + kMissionIsolationDiagnosticsMilliseconds;
    }

    const auto maintainRemoteSceneIsolation = [&]() {
        bool required{};
        if (remoteStreaming &&
            localSlot_ == PlayerSlot::Host &&
            localMissionState_.has_value()) {
            required =
                (localMissionCinematicState_.has_value() &&
                 IsCinematicPresentationPhase(
                     localMissionCinematicState_->phase)) ||
                localMissionState_->phase == MissionPhase::Recovery ||
                localMissionState_->phase == MissionPhase::SoloOverride;
        }
        const bool changed =
            required != remoteParticipantSceneIsolated_;
        if (changed) {
            remoteParticipantSceneIsolated_ = required;
            ++remoteParticipantIsolationTransitions_;
            facade_.Log(
                required
                    ? "[MISSION_SPECTATOR][MISSION_FSM] host scene isolated the remote guest proxy"
                    : "[MISSION_SPECTATOR][MISSION_FSM] host scene released the remote guest proxy");
        }
        facade_.MaintainRemoteMissionParticipant(required);
        if (changed && !required && remoteReplicaId_.IsValid()) {
            if (remoteIdentity_.has_value()) {
                (void)facade_.ApplyRemoteIdentity(*remoteIdentity_);
            }
            if (remoteAppearance_.has_value()) {
                (void)facade_.ApplyRemoteAppearance(*remoteAppearance_);
            }
            if (remoteEquipment_.has_value()) {
                (void)facade_.ApplyRemoteEquipment(*remoteEquipment_);
            }
            if (latestRemoteState_.has_value()) {
                (void)facade_.ApplyRemoteTransform(*latestRemoteState_);
            }
        }
    };
    maintainRemoteSceneIsolation();
    facade_.DrawPauseVoteStatus(
        PauseVoteView{
            remoteStreaming,
            synchronizedPaused_,
            hostPauseVoted_,
            guestPauseVoted_});
    facade_.MaintainRealtimeSession(
        remoteStreaming &&
        localSlot_.has_value() &&
        !soloOverride_ &&
        !cutsceneSpectator_ &&
        (!sample.has_value() ||
         !sample->cutsceneActive),
        synchronizedPaused_);
    if (remoteStreaming && localSlot_.has_value() && localEntityId_.IsValid()) {
        // Every bridge-owned encounter is outside the map-pickup/capability lanes.
        // Corpse interaction is local vanilla behavior, so even the itemless positive collection telemetry is discarded while a scene is preparing, active, or retained for corpse cleanup.
        // This applies equally to the catalog profiles and the exact Extortion adaptation.
        const auto bridgeEncounterLootWindowActive = [&]() noexcept {
            return ambientEncounterCoordinator_.Active().has_value() ||
                remoteAmbientEncounter_.has_value();
        }();
        const auto collections = facade_.DrainVanillaPickupCollections();
        if (bridgeEncounterLootWindowActive) {
            if (!collections.empty()) {
                facade_.Log(
                    "[AMBIENT_ENCOUNTER] discarded pickup telemetry; local vanilla loot is never synchronized");
            }
        } else {
            for (const auto& collection : collections) {
                try {
                    Frame frame;
                    frame.header.type = MessageType::PickupCollected;
                    frame.header.sequence = sequencer_.Next();
                    frame.header.tick = now;
                    frame.payload = EncodePickupCollected(PickupCollectedPayload{
                        localEntityId_, collection.collectionId, collection.pickupHash});
                    SendBestEffort(std::move(frame));
                } catch (...) {
                    facade_.Log("[PICKUP_OBSERVED] discarded malformed collection event");
                }
            }
        }
        if (*localSlot_ == PlayerSlot::Host) {
            const auto capabilities =
                facade_.DrainCampaignCapabilityObservations();
            if (bridgeEncounterLootWindowActive && !capabilities.empty()) {
                facade_.Log(
                    "[AMBIENT_ENCOUNTER] discarded capability telemetry; encounter rewards are disabled");
            }
            if (!bridgeEncounterLootWindowActive) {
                for (const auto& observation : capabilities) {
                    if (observation.recordHash == 0U) {
                        continue;
                    }
                    const auto grantedAtUnixMilliseconds =
                        static_cast<std::int64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
                    const auto sequence = static_cast<std::uint16_t>(
                        static_cast<std::uint16_t>(localCapabilityEventId_) + 1U);
                    localCapabilityEventId_ =
                        (static_cast<std::uint64_t>(grantedAtUnixMilliseconds)
                         << 16U) |
                        static_cast<std::uint64_t>(
                            sequence == 0U ? 1U : sequence);
                    Frame frame;
                    frame.header.type = MessageType::CampaignCapability;
                    frame.header.sequence = sequencer_.Next();
                    frame.header.tick = now;
                    frame.payload = EncodeCampaignCapability(
                        CampaignCapabilityPayload{
                            observation.kind,
                            observation.recordHash,
                            localCapabilityEventId_,
                            grantedAtUnixMilliseconds});
                    SendBestEffort(std::move(frame));
                    facade_.Log(
                        "[CAPABILITY] host acquisition observation forwarded");
                }
            }
        } else {
            (void)facade_.DrainCampaignCapabilityObservations();
        }
    } else {
        (void)facade_.DrainVanillaPickupCollections();
        (void)facade_.DrainCampaignCapabilityObservations();
    }
    VerifyPendingTeleport(now);
    if (sessionMenu_.IsHudVisible()) {
        bool reviveAvailable{};
        float reviveProgress{};
        if (localSlot_.has_value()) {
            const auto remoteSlot = OtherSlot(*localSlot_);
            const auto remoteDowned =
                players_.State(remoteSlot).lifecycle == PlayerLifecycle::Downed ||
                players_.State(remoteSlot).lifecycle == PlayerLifecycle::Reviving;
            const auto distance = facade_.HostGuestDistanceMeters().value_or(
                std::numeric_limits<float>::infinity());
            const auto localAlive =
                players_.State(*localSlot_).lifecycle == PlayerLifecycle::Alive;
            reviveAvailable = localAlive && remoteDowned && std::isfinite(distance) &&
                distance <= 2.0F;
            if (localInteraction_.active &&
                localInteraction_.kind == InteractionKind::Revive &&
                localInteraction_.startedAtMs != 0U && now >=
                    localInteraction_.startedAtMs) {
                reviveProgress = std::clamp(
                    static_cast<float>(now - localInteraction_.startedAtMs) /
                        4'000.0F,
                    0.0F,
                    1.0F);
            }
        }
        facade_.DrawBridgeHud(
            BridgeHudState{
                active_,
                transport_.IsConnected(),
                localSlot_,
                remoteStreaming,
                diagnostics_,
                soloOverride_,
                reviveAvailable,
                reviveProgress,
                guestMissionQuarantineActive_});
    }

    lastTickStage_ = "local-player-actions-and-mission";
    if (sample.has_value() && localSlot_.has_value()) {
        const auto localSlot = *localSlot_;
        sample->entityId = localEntityId_;
        sample->slot = localSlot;
        TickLocalPlayerActions(*sample, localSlot, now);

        const bool weaponChanged =
            hasPreviousLocalWeaponSample_ &&
            sample->weaponHash != previousLocalWeaponHash_;
        const bool ammoChanged =
            hasPreviousLocalWeaponSample_ &&
            sample->weaponHash == previousLocalWeaponHash_ &&
            sample->weaponAmmo != previousLocalWeaponAmmo_;
        const bool firearmFiring =
            sample->firing && !sample->weaponIsLasso;
        const bool observedShot =
            firearmFiring &&
            (!previousLocalFiring_ ||
             weaponChanged ||
             ammoChanged);
        if (observedShot) {
            localFireSequence_ =
                localFireSequence_ == std::numeric_limits<std::uint32_t>::max()
                    ? 1U
                    : localFireSequence_ + 1U;
            if (sample->aimTargetValid) {
                localShotAimTarget_ = sample->aimTarget;
                localShotLatchExpiresMs_ =
                    now + kLocalShotLatchMilliseconds;
            }
        } else if (sample->weaponIsLasso) {
            // RDR2 reports a lasso throw through the shooting predicate.
            // Do not carry an earlier firearm latch across the weapon switch.
            localShotLatchExpiresMs_ = 0U;
            localShotAimTarget_ = {};
        }
        previousLocalWeaponHash_ = sample->weaponHash;
        previousLocalWeaponAmmo_ = sample->weaponAmmo;
        previousLocalFiring_ = firearmFiring;
        hasPreviousLocalWeaponSample_ = true;

        const bool startedClimb =
            sample->climbing && !previousLocalClimbing_;
        const bool startedJump =
            sample->jumping && !previousLocalJumping_;
        const bool jumpInputEdge =
            sample->jumpPressed && !sample->mounted;
        const bool traversalOpen =
            localTraversalKind_ != PlayerTraversalKind::None &&
            localTraversalLatchExpiresMs_ != 0U &&
            now <= localTraversalLatchExpiresMs_;
        if (jumpInputEdge ||
            (!traversalOpen && (startedClimb || startedJump))) {
            localTraversalActionId_ =
                AdvanceNonZero(localTraversalActionId_);
            localLocomotionEpoch_ =
                AdvanceNonZero(localLocomotionEpoch_);
            localTraversalKind_ =
                startedClimb
                    ? PlayerTraversalKind::Climb
                    : PlayerTraversalKind::Jump;
            localTraversalAnchor_ =
                jumpInputEdge
                    ? sample->position
                    : hasPreviousLocalTransform_
                    ? previousLocalPosition_
                    : sample->position;
            localTraversalHeading_ =
                jumpInputEdge
                    ? sample->heading
                    : hasPreviousLocalTransform_
                    ? previousLocalHeading_
                    : sample->heading;
            localTraversalApproachVelocity_ = sample->velocity;
            localTraversalObstaclePoint_ = {};
            localTraversalObstacleNormal_ = {};
            localTraversalObstacleTopZ_ = 0.0F;
            localTraversalExpectedLanding_ = {};
            localTraversalFlags_ =
                jumpInputEdge
                    ? static_cast<std::uint32_t>(
                          PlayerTraversalFlag::InputEdgeDetected)
                    : 0U;
            if (sample->traversalObstacleValid) {
                localTraversalObstaclePoint_ =
                    sample->traversalObstaclePoint;
                localTraversalObstacleNormal_ =
                    sample->traversalObstacleNormal;
                localTraversalObstacleTopZ_ =
                    sample->traversalObstacleTopZ;
                localTraversalFlags_ |=
                    static_cast<std::uint32_t>(
                        PlayerTraversalFlag::ObstacleValid);
            }
            localTraversalRevision_ = 1U;
            localTraversalSentRevision_ = 0U;
            localTraversalBecameActive_ =
                sample->jumping || sample->climbing || sample->falling;
            localTraversalLandingCaptured_ = false;
            localTraversalLatchExpiresMs_ =
                now + kLocalTraversalLatchMilliseconds;
        } else if (traversalOpen) {
            bool traversalChanged{};
            if (startedClimb &&
                localTraversalKind_ != PlayerTraversalKind::Climb) {
                localTraversalKind_ = PlayerTraversalKind::Climb;
                traversalChanged = true;
            }
            const bool obstacleWasMissing =
                (localTraversalFlags_ &
                 static_cast<std::uint32_t>(
                     PlayerTraversalFlag::ObstacleValid)) == 0U;
            if (obstacleWasMissing &&
                sample->traversalObstacleValid) {
                localTraversalObstaclePoint_ =
                    sample->traversalObstaclePoint;
                localTraversalObstacleNormal_ =
                    sample->traversalObstacleNormal;
                localTraversalObstacleTopZ_ =
                    sample->traversalObstacleTopZ;
                localTraversalFlags_ |=
                    static_cast<std::uint32_t>(
                        PlayerTraversalFlag::ObstacleValid);
                traversalChanged = true;
            }
            if (sample->jumping || sample->climbing || sample->falling) {
                localTraversalBecameActive_ = true;
            } else if (localTraversalBecameActive_ &&
                       !localTraversalLandingCaptured_) {
                localTraversalExpectedLanding_ = sample->position;
                localTraversalFlags_ |=
                    static_cast<std::uint32_t>(
                        PlayerTraversalFlag::ExpectedLandingValid);
                localTraversalLandingCaptured_ = true;
                localTraversalLatchExpiresMs_ =
                    now + kLocalTraversalLandingLatchMilliseconds;
                traversalChanged = true;
            }
            if (traversalChanged) {
                localTraversalRevision_ =
                    AdvanceNonZero(localTraversalRevision_);
            }
        }
        if (sample->mounted != previousLocalMounted_) {
            localLocomotionEpoch_ =
                AdvanceNonZero(localLocomotionEpoch_);
        }
        previousLocalJumping_ = sample->jumping;
        previousLocalClimbing_ = sample->climbing;
        previousLocalMounted_ = sample->mounted;
        previousLocalPosition_ = sample->position;
        previousLocalHeading_ = sample->heading;
        hasPreviousLocalTransform_ = true;
        if (localTraversalLatchExpiresMs_ != 0U &&
            now > localTraversalLatchExpiresMs_) {
            localTraversalKind_ = PlayerTraversalKind::None;
            localTraversalLatchExpiresMs_ = 0U;
        }

        if (transport_.IsConnected() &&
            localTraversalKind_ != PlayerTraversalKind::None &&
            localTraversalRevision_ != 0U &&
            localTraversalSentRevision_ != localTraversalRevision_) {
            PlayerTraversalPayload traversal;
            traversal.entityId = localEntityId_;
            traversal.slot = localSlot;
            traversal.kind = localTraversalKind_;
            traversal.actionId = localTraversalActionId_;
            traversal.revision = localTraversalRevision_;
            traversal.locomotionEpoch = localLocomotionEpoch_;
            traversal.flags = localTraversalFlags_;
            traversal.takeoffHeading = localTraversalHeading_;
            traversal.takeoffPosition = localTraversalAnchor_;
            traversal.approachVelocity =
                localTraversalApproachVelocity_;
            traversal.obstaclePoint = localTraversalObstaclePoint_;
            traversal.obstacleNormal = localTraversalObstacleNormal_;
            traversal.obstacleTopZ = localTraversalObstacleTopZ_;
            traversal.expectedLanding =
                localTraversalExpectedLanding_;
            Frame traversalFrame;
            traversalFrame.header.type = MessageType::PlayerTraversal;
            traversalFrame.header.sequence = sequencer_.Next();
            traversalFrame.header.tick = now;
            traversalFrame.payload = EncodePlayerTraversal(traversal);
            SendBestEffort(std::move(traversalFrame));
            localTraversalSentRevision_ = localTraversalRevision_;
        }

        auto& playerState = players_.State(localSlot);
        if (sample->downed &&
            playerState.lifecycle == PlayerLifecycle::Alive) {
            localRespawnCandidateSinceMs_.reset();
            players_.SetDowned(localSlot);
            SendLifecycleState(
                MessageType::DownedState,
                localSlot,
                PlayerLifecycle::Downed,
                0.0F);
        } else if (
            !sample->downed &&
            (playerState.lifecycle == PlayerLifecycle::Downed ||
             playerState.lifecycle == PlayerLifecycle::Reviving) &&
            sample->healthFraction >=
                kCheckpointRespawnMinimumHealthFraction) {
            if (!localRespawnCandidateSinceMs_.has_value() ||
                now < *localRespawnCandidateSinceMs_) {
                localRespawnCandidateSinceMs_ = now;
            } else if (
                now - *localRespawnCandidateSinceMs_ >=
                kCheckpointRespawnConfirmationMilliseconds) {
                players_.SetAlive(
                    localSlot,
                    sample->healthFraction);
                SendLifecycleState(
                    MessageType::DownedState,
                    localSlot,
                    PlayerLifecycle::Alive,
                    sample->healthFraction);
                localRespawnCandidateSinceMs_.reset();
                facade_.Log(
                    "vanilla checkpoint respawn detected; local and remote lifecycle restored");
            }
        } else if (
            playerState.lifecycle == PlayerLifecycle::Alive ||
            sample->downed ||
            sample->healthFraction <
                kCheckpointRespawnMinimumHealthFraction) {
            localRespawnCandidateSinceMs_.reset();
        }

        const auto currentLocalLifecycle =
            players_.State(localSlot).lifecycle;
        const bool checkpointRespawnConfirmed =
            previousLocalLifecycle_.has_value() &&
            (*previousLocalLifecycle_ == PlayerLifecycle::Downed ||
             *previousLocalLifecycle_ == PlayerLifecycle::Reviving) &&
            currentLocalLifecycle == PlayerLifecycle::Alive &&
            sample->healthFraction >=
                kCheckpointRespawnMinimumHealthFraction;
        TickMissionAuthority(
            *sample,
            localSlot,
            now,
            checkpointRespawnConfirmed);
        TickMissionProgression(*sample, now);
        TickMissionObjective(*sample, now);
        TickMissionDialogue(localSlot, now);
        TickAmbientEncounter(*sample, localSlot, now);
        TickMissionCinematic(sample, localSlot, now);
        // TickMissionAuthority may have entered or left a host cutscene in this same frame.
        // Re-evaluate after publishing the new phase so the remote guest never leaks into the vanilla camera for one full tick.
        maintainRemoteSceneIsolation();
        if (previousLocalLifecycle_.has_value() &&
            *previousLocalLifecycle_ != currentLocalLifecycle) {
            localLocomotionEpoch_ =
                AdvanceNonZero(localLocomotionEpoch_);
        }
        previousLocalLifecycle_ = currentLocalLifecycle;
        facade_.MaintainLocalDownedState(
            currentLocalLifecycle == PlayerLifecycle::Downed ||
                currentLocalLifecycle == PlayerLifecycle::Reviving,
            players_.State(localSlot).healthFraction);
        TickLocalInteractions(*sample, localSlot, now);

        if (localSlot == PlayerSlot::Guest) {
            const bool hostRequiresSpectator =
                (remoteMissionCinematicState_.has_value() &&
                 IsCinematicPresentationPhase(
                     remoteMissionCinematicState_->phase)) ||
                (remoteMissionState_.has_value() &&
                 (remoteMissionState_->phase == MissionPhase::Recovery ||
                  remoteMissionState_->phase ==
                      MissionPhase::SoloOverride ||
                  (!remoteMissionCinematicState_.has_value() &&
                   (remoteMissionState_->phase == MissionPhase::Cutscene ||
                    remoteMissionState_->phase == MissionPhase::Loading))));
            // During the authenticated matching-instance barrier the guest must retain its own vanilla conversation/camera controls long enough to enter the exact local mission.
            // Outside that narrow window host presentation remains authoritative as before.
            const bool spectatorRequired =
                (!guestMatchingMissionStartPermitted &&
                 hostRequiresSpectator) ||
                guestMissionQuarantineActive_;
            if (spectatorRequired != cutsceneSpectator_) {
                cutsceneSpectator_ = spectatorRequired;
                (void)players_.SetSpectator(
                    PlayerSlot::Guest,
                    spectatorRequired || soloOverride_);
                facade_.Log(
                    spectatorRequired
                        ? (guestMissionQuarantineActive_
                               ? "[MISSION_SPECTATOR][MISSION_ISOLATION] entered to quarantine a guest-local Story transition"
                               : "[MISSION_SPECTATOR] entered from host mission phase")
                        : "[MISSION_SPECTATOR] host phase released spectator");
            }
            facade_.MaintainMissionSpectator(
                spectatorRequired || soloOverride_);
            if (!spectatorRequired &&
                !soloOverride_ &&
                pendingMissionGuestTeleport_.has_value()) {
                const auto deferredTeleport =
                    *pendingMissionGuestTeleport_;
                pendingMissionGuestTeleport_.reset();
                HandleInboundTeleportGuest(deferredTeleport);
            }
        } else {
            facade_.MaintainMissionSpectator(false);
        }
        if (const auto distance =
                localSlot == PlayerSlot::Host &&
                        localMissionState_.has_value() &&
                        localMissionState_->phase == MissionPhase::Active
                    ? facade_.HostGuestDistanceMeters()
                    : std::nullopt;
            distance.has_value()) {
            const auto decision =
                bubble_.Evaluate(sample->missionActive, *distance);
            if (decision.showWarning) {
                facade_.ShowMissionBubbleWarning(*distance);
            }
            if (decision.executeOverLimitAction) {
                if (decision.action ==
                    MissionBubbleOverLimitAction::TeleportGuest) {
                    HandleMenuCommand(BridgeCommand::TeleportGuest);
                } else {
                    soloOverride_ = false;
                    HandleMenuCommand(
                        BridgeCommand::ToggleSoloOverride);
                }
            }
        } else {
            bubble_.Reset();
        }

        if (telemetry_.ShouldEmit(now) && transport_.IsConnected()) {
            const bool shotLatched =
                localFireSequence_ != 0U &&
                localShotLatchExpiresMs_ != 0U &&
                now <= localShotLatchExpiresMs_ &&
                IsFinite(localShotAimTarget_);
            const bool effectiveAimTargetValid =
                sample->aimTargetValid ||
                shotLatched;
            const auto effectiveAimTarget =
                sample->aimTargetValid
                    ? sample->aimTarget
                    : localShotAimTarget_;
            std::uint32_t flags{};
            flags |= sample->missionActive
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::InMission)
                         : 0U;
            flags |= sample->cutsceneActive
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::InCutscene)
                         : 0U;
            flags |= sample->mounted
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::Mounted)
                         : 0U;
            flags |= sample->aiming
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::Aiming)
                         : 0U;
            flags |= (firearmFiring || shotLatched)
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::Firing)
                         : 0U;
            flags |= sample->meleeCombat
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::MeleeCombat)
                         : 0U;
            flags |= sample->peerCombatTarget
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::PeerCombatTarget)
                         : 0U;
            flags |= sample->peerLassoActive
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::PeerLassoActive)
                         : 0U;
            flags |= sample->meleeBlocking
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::MeleeBlocking)
                         : 0U;
            flags |= sample->meleeGrappling
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::MeleeGrappling)
                         : 0U;
            flags |= sample->peerKnockdown
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::PeerKnockdown)
                         : 0U;
            flags |= sample->stealthMovement
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::StealthMovement)
                         : 0U;
            flags |= sample->inCover
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::InCover)
                         : 0U;
            flags |= sample->goingIntoCover
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::GoingIntoCover)
                         : 0U;
            flags |= sample->coverFacingLeft
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::CoverFacingLeft)
                         : 0U;
            flags |= sample->aimingFromCover
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::AimingFromCover)
                         : 0U;
            flags |= sample->jumping
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::Jumping)
                         : 0U;
            flags |= sample->climbing
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::Climbing)
                         : 0U;
            flags |= sample->inWater
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::InWater)
                         : 0U;
            flags |= sample->swimming
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::Swimming)
                         : 0U;
            flags |= sample->swimmingUnderwater
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::SwimmingUnderwater)
                         : 0U;
            flags |= effectiveAimTargetValid
                         ? static_cast<std::uint32_t>(
                               PlayerStateFlag::AimTargetValid)
                         : 0U;
            const auto& runtimeState = players_.State(localSlot);
            const auto locomotionIntent =
                BuildLocalLocomotionIntent(*sample);
            PlayerStatePayload payload{
                localEntityId_,
                localSlot,
                runtimeState.lifecycle,
                sample->position,
                sample->velocity,
                sample->heading,
                runtimeState.lifecycle == PlayerLifecycle::Alive
                    ? sample->healthFraction
                    : runtimeState.healthFraction,
                flags,
                effectiveAimTargetValid
                    ? effectiveAimTarget
                    : Vec3{},
                localFireSequence_,
                locomotionIntent.movementHeading,
                locomotionIntent.forwardSpeed,
                locomotionIntent.rightSpeed,
                locomotionIntent.moveBlend,
                localLocomotionEpoch_,
                localTraversalActionId_,
                localTraversalKind_,
                SelectLocalLocomotionMode(
                    *sample,
                    runtimeState.lifecycle),
                localTraversalKind_ != PlayerTraversalKind::None
                    ? localTraversalAnchor_
                    : Vec3{},
                localTraversalKind_ != PlayerTraversalKind::None
                    ? localTraversalHeading_
                    : 0.0F};
            SendBestEffort(
                MakePlayerStateFrame(payload, sequencer_.Next(), now));

            if (motionReplicationMode_ ==
                MotionReplicationWireMode::AnimGraphReplica) {
                localAnimationSampleSequence_ =
                    localAnimationSampleSequence_ ==
                            std::numeric_limits<std::uint32_t>::max()
                        ? 1U
                        : localAnimationSampleSequence_ + 1U;
                if (const auto animation =
                        facade_.SampleLocalAnimationState(
                            localEntityId_,
                            localSlot,
                            localLocomotionEpoch_,
                            localAnimationSampleSequence_);
                    animation.has_value()) {
                    Frame animationFrame;
                    animationFrame.header.type =
                        MessageType::PlayerAnimationState;
                    animationFrame.header.sequence = sequencer_.Next();
                    animationFrame.header.tick = now;
                    animationFrame.payload =
                        EncodePlayerAnimationState(*animation);
                    SendBestEffort(std::move(animationFrame));
                    ++animationSamplesSent_;
                }
            }

            const auto localOwnedMountEntityId =
                NetEntityId::Compose(
                    sessionEpoch_,
                    sample->mount.has_value() && sample->mount->vehicle
                        ? (localSlot == PlayerSlot::Host ? 12U : 13U)
                        : (localSlot == PlayerSlot::Host ? 10U : 11U));
            const bool borrowedPeerMount =
                sample->mount.has_value() &&
                sample->mount->borrowedPeerMount &&
                sample->mount->sharedEntityId.IsValid() &&
                sample->mount->sharedGeneration != 0U;
            const bool mountPresent =
                sample->mount.has_value() &&
                (!sample->mount->borrowedPeerMount ||
                 borrowedPeerMount);
            const bool localOwnedMountPresent =
                mountPresent && !borrowedPeerMount;
            const bool localMountContinuityFresh =
                lastLocalOwnedMountPresentMs_ != 0U &&
                now >= lastLocalOwnedMountPresentMs_ &&
                now - lastLocalOwnedMountPresentMs_ <=
                    kLocalMountIdentityContinuityMilliseconds;
            const bool localMountModelChanged =
                localOwnedMountPresent &&
                previousLocalMountModelHash_ != 0U &&
                previousLocalMountModelHash_ !=
                    sample->mount->modelHash;
            const bool localMountHandleChangedAfterLongAbsence =
                localOwnedMountPresent &&
                previousLocalMountHandle_ != 0 &&
                previousLocalMountHandle_ !=
                    sample->mount->localHandle &&
                !localMountContinuityFresh;
            if (localOwnedMountPresent &&
                (localMountModelChanged ||
                 localMountHandleChangedAfterLongAbsence)) {
                localMountGeneration_ =
                    localMountGeneration_ ==
                            std::numeric_limits<
                                std::uint32_t>::max()
                        ? 1U
                        : localMountGeneration_ + 1U;
            }

            PlayerMountStatePayload mountState;
            mountState.playerEntityId =
                localEntityId_;
            mountState.mountEntityId =
                borrowedPeerMount
                    ? sample->mount->sharedEntityId
                    : localOwnedMountEntityId;
            mountState.slot = localSlot;
            mountState.generation =
                borrowedPeerMount
                    ? sample->mount->sharedGeneration
                    : localMountGeneration_;
            if (mountPresent) {
                constexpr auto kPresent =
                    static_cast<std::uint8_t>(
                        PlayerMountStateFlag::Present);
                constexpr auto kMounted =
                    static_cast<std::uint8_t>(
                        PlayerMountStateFlag::Mounted);
                constexpr auto kDead =
                    static_cast<std::uint8_t>(
                        PlayerMountStateFlag::Dead);
                constexpr auto kBorrowedPeerMount =
                    static_cast<std::uint8_t>(
                        PlayerMountStateFlag::BorrowedPeerMount);
                constexpr auto kVehicle =
                    static_cast<std::uint8_t>(
                        PlayerMountStateFlag::Vehicle);
                constexpr auto kVehicleDriver =
                    static_cast<std::uint8_t>(
                        PlayerMountStateFlag::VehicleDriver);
                constexpr auto kVehiclePassenger =
                    static_cast<std::uint8_t>(
                        PlayerMountStateFlag::VehiclePassenger);
                mountState.flags = kPresent;
                mountState.flags |=
                    sample->mount->mounted
                        ? kMounted
                        : 0U;
                mountState.flags |=
                    sample->mount->dead
                        ? kDead
                        : 0U;
                mountState.flags |=
                    borrowedPeerMount
                        ? kBorrowedPeerMount
                        : 0U;
                mountState.flags |= sample->mount->vehicle ? kVehicle : 0U;
                mountState.flags |= sample->mount->vehicleDriver
                    ? kVehicleDriver
                    : 0U;
                mountState.flags |= sample->mount->vehiclePassenger
                    ? kVehiclePassenger
                    : 0U;
                mountState.modelHash =
                    sample->mount->modelHash;
                mountState.position =
                    sample->mount->position;
                mountState.velocity =
                    sample->mount->velocity;
                mountState.heading =
                    sample->mount->heading;
                mountState.healthFraction =
                    sample->mount->healthFraction;
                if (!borrowedPeerMount) {
                    previousLocalMountHandle_ =
                        sample->mount->localHandle;
                    previousLocalMountModelHash_ =
                        sample->mount->modelHash;
                    lastLocalOwnedMountPresentMs_ = now;
                }
            }
            Frame mountFrame;
            mountFrame.header.type =
                MessageType::PlayerMountState;
            mountFrame.header.sequence =
                sequencer_.Next();
            mountFrame.header.tick = now;
            mountFrame.payload =
                EncodePlayerMountState(mountState);
            SendBestEffort(std::move(mountFrame));
            if (!lastLocalMountState_.has_value() ||
                lastLocalMountState_->mountEntityId !=
                    mountState.mountEntityId ||
                lastLocalMountState_->flags != mountState.flags ||
                lastLocalMountState_->modelHash !=
                    mountState.modelHash ||
                lastLocalMountState_->generation !=
                    mountState.generation) {
                facade_.Log(
                    "[MOUNT_LOCAL] id=" +
                    std::to_string(
                        mountState.mountEntityId.Value()) +
                    ", generation=" +
                    std::to_string(mountState.generation) +
                    ", flags=" +
                    std::to_string(mountState.flags) +
                    ", borrowed=" +
                    (borrowedPeerMount
                         ? std::string{"true"}
                         : std::string{"false"}) +
                    ", vehicle=" +
                    (sample->mount.has_value() && sample->mount->vehicle
                         ? std::string{"true"}
                         : std::string{"false"}));
                constexpr auto kMountedFlag =
                    static_cast<std::uint8_t>(
                        PlayerMountStateFlag::Mounted);
                constexpr auto kDeadFlag =
                    static_cast<std::uint8_t>(
                        PlayerMountStateFlag::Dead);
                facade_.Log(
                    "[MOUNT_LIFECYCLE] direction=tx, state=" +
                    std::string{
                        !mountPresent
                            ? "absent"
                            : (mountState.flags & kDeadFlag) != 0U
                                  ? "dead"
                                  : (mountState.flags & kMountedFlag) != 0U
                                        ? "mounted"
                                        : "present"} +
                    ", correlation=mount-" +
                    std::to_string(mountState.generation) +
                    ", generation=" +
                    std::to_string(mountState.generation) +
                    ", reason=local-state-change");
            }
            lastLocalMountState_ = mountState;
        }

        if (localSlot == PlayerSlot::Host &&
            now >= nextWorldStateMs_ &&
            transport_.IsConnected() &&
            !soloOverride_ &&
            !cutsceneSpectator_ &&
            (!localMissionCinematicState_.has_value() ||
             !IsCinematicPresentationPhase(
                 localMissionCinematicState_->phase)) &&
            !sample->cutsceneActive) {
            if (const auto world = facade_.SampleWorldState();
                world.has_value()) {
                Frame frame;
                frame.header.type = MessageType::WorldState;
                frame.header.sequence = sequencer_.Next();
                frame.header.tick = now;
                frame.payload = EncodeWorldState(*world);
                SendBestEffort(std::move(frame));
            }
            nextWorldStateMs_ =
                now + kWorldStateIntervalMilliseconds;
        }

        std::uint32_t equipmentFlags{};
        if (sample->weaponHash != 0U) {
            equipmentFlags |=
                static_cast<std::uint32_t>(
                    EquipmentStateFlag::Equipped);
        }
        if (sample->reloading) {
            equipmentFlags |=
                static_cast<std::uint32_t>(
                    EquipmentStateFlag::Reloading);
        }
        const EquipmentStatePayload equipment{
            localEntityId_,
            sample->weaponHash,
            sample->weaponAmmo,
            equipmentFlags};
        if (transport_.IsConnected() &&
            (!lastLocalEquipment_.has_value() ||
             *lastLocalEquipment_ != equipment ||
             now >= nextEquipmentRefreshMs_)) {
            Frame frame;
            frame.header.type = MessageType::EquipmentState;
            frame.header.sequence = sequencer_.Next();
            frame.header.tick = now;
            frame.payload = EncodeEquipmentState(equipment);
            SendBestEffort(std::move(frame));
            lastLocalEquipment_ = equipment;
            nextEquipmentRefreshMs_ =
                now + kEquipmentRefreshMilliseconds;
        }

        if (transport_.IsConnected() &&
            now >= nextAppearanceRefreshMs_) {
            auto appearance = facade_.SampleLocalAppearance(
                localEntityId_,
                localSlot,
                localAppearanceRevision_);
            if (!appearance.has_value()) {
                nextAppearanceRefreshMs_ = now + 500U;
            } else {
                const bool changed =
                    !lastLocalAppearance_.has_value() ||
                    lastLocalAppearance_->modelHash !=
                        appearance->modelHash ||
                    lastLocalAppearance_->fingerprint !=
                        appearance->fingerprint ||
                    lastLocalAppearance_->flags !=
                        appearance->flags ||
                    lastLocalAppearance_->componentHashes !=
                        appearance->componentHashes;
                if (changed && lastLocalAppearance_.has_value()) {
                    localAppearanceRevision_ =
                        AdvanceNonZero(localAppearanceRevision_);
                }
                appearance->revision = localAppearanceRevision_;
                Frame frame;
                frame.header.type =
                    MessageType::PlayerAppearanceState;
                frame.header.sequence = sequencer_.Next();
                frame.header.tick = now;
                frame.payload =
                    EncodePlayerAppearanceState(*appearance);
                SendBestEffort(std::move(frame));
                if (changed) {
                    facade_.Log(
                        "[METAPED_APPEARANCE][TX] revision=" +
                        std::to_string(appearance->revision) +
                        ", model=" +
                        std::to_string(appearance->modelHash) +
                        ", components=" +
                        std::to_string(
                            appearance->componentHashes.size()) +
                        ", fingerprint=" +
                        std::to_string(appearance->fingerprint));
                }
                lastLocalAppearance_ = std::move(appearance);
                nextAppearanceRefreshMs_ =
                    now + kAppearanceRefreshMilliseconds;
            }
        }
    }

    lastTickStage_ = "mission-camera";
    if (localSlot_.has_value()) {
        TickMissionCamera(*localSlot_, now);
    }
    lastTickStage_ = "mission-presentation";
    MaintainMissionPresentation(sample, remoteStreaming, now);
    lastTickStage_ = "world-mirror";
    TickWorldMirror(now, sample, remoteStreaming);
    lastTickStage_ = "animscene-hybrid";
    if (localSlot_.has_value()) {
        TickAnimSceneHybridDefinition(*localSlot_, sample, now);
    }
    lastTickStage_ = "runtime-diagnostics";
    EmitRuntimeDiagnostics(remoteStreaming, now);
    EmitProblemDiagnosticSnapshot(sample, remoteStreaming, now);

    if (pendingRevive_.has_value()) {
        if (now > reviveRequestExpiresMs_) {
            pendingRevive_.reset();
        } else {
            pendingRevive_->distanceMeters =
                facade_.HostGuestDistanceMeters().value_or(
                    std::numeric_limits<float>::infinity());
        }
    }
    lastTickStage_ = "player-lifecycle";
    auto signals = players_.Tick(elapsed, pendingRevive_);
    HandlePlayerSignals(signals);
    lastTickStage_ = "idle";
}

void BridgeRuntime::AbortAfterNativeException() noexcept {
    // A native AV means another native call during cleanup may hit the same stale RDR2 handle.
    // Preserve diagnostics and disconnect the sidecar, but deliberately perform no facade/world/ped/camera cleanup in this path.
    active_ = false;
    shouldUnload_ = true;
    transport_.Disconnect();
    pendingHostWorldDespawns_.clear();
    hostWorldReplayAwaitingGuest_ = false;
    hostWorldReplayGuestDeadlineMs_ = 0U;
}

void BridgeRuntime::TickWorldMirror(
    const std::uint64_t nowMs,
    const std::optional<LocalPlayerSample>& localSample,
    const bool remoteStreaming) {
    if (localSlot_ == PlayerSlot::Host && !transport_.IsConnected()) {
        // A game-pipe outage is not a session/world-authority boundary.
        // The sidecar still owns the previous graph, so silently destroying stable host IDs here would leave orphan NPC/horse proxies after reconnect.
        // Explicit Stop/Waiting/role-mismatch paths perform the real reset.
        return;
    }
    // The guest only shows host NPC copies when it is safe.
    // Do not mix them into a cutscene or a different mission state.
    const bool authorityAllowsWorldMirror =
        localSlot_.has_value() &&
        localSample.has_value() &&
        (*localSlot_ == PlayerSlot::Host
             ? true
             : IsMissionWorldMirrorSafe());
    const bool hostCinematicMirrorWindow =
        localSlot_ == PlayerSlot::Host &&
        localSample.has_value() && remoteStreaming &&
        localMissionCinematicState_.has_value() &&
        IsCinematicPresentationPhase(
            localMissionCinematicState_->phase);
    const bool guestCinematicMirrorWindow =
        localSlot_ == PlayerSlot::Guest &&
        localSample.has_value() && remoteStreaming &&
        cutsceneSpectator_ &&
        remoteMissionCinematicState_.has_value() &&
        IsCinematicPresentationPhase(
            remoteMissionCinematicState_->phase);
    const bool normalMirrorWindow =
        localSlot_.has_value() &&
        localSample.has_value() &&
        transport_.IsConnected() &&
        remoteStreaming &&
        !soloOverride_ &&
        !cutsceneSpectator_ &&
        (!localMissionCinematicState_.has_value() ||
         !IsCinematicPresentationPhase(
             localMissionCinematicState_->phase)) &&
        !localSample->cutsceneActive &&
        authorityAllowsWorldMirror;
    const bool safeMirrorWindow =
        normalMirrorWindow || hostCinematicMirrorWindow ||
        guestCinematicMirrorWindow;
    // Leaving the safe window removes or retains graph state according to role; a full guest reset deletes proxy NPCs and restores suppressed ambient peds.
    if (!safeMirrorWindow) {
        if (localSlot_ == PlayerSlot::Host) {
            if (localSample.has_value() && localSample->cutsceneActive &&
                !hostCinematicMirrorWindow) {
                // The Story frontend briefly uses a cutscene camera while loading an ordinary save.
                // It is unsafe to sample/update the graph in that window, but it is equally unsafe to destroy the stable graph: no mission cinematic authority exists yet to recreate it for the guest.
                return;
            }
            if (forceHostWorldMirrorReplay_ &&
                hostWorldReplayAwaitingGuest_ &&
                !remoteStreaming) {
                if (hostWorldReplayGuestDeadlineMs_ == 0U ||
                    nowMs < hostWorldReplayGuestDeadlineMs_) {
                    // HelloAck may legally precede the first restored LAN PlayerState.
                    // Keep the stable graph until that stream arrives; resetting it here would orphan the old IDs in the sidecar/guest cache before replay can run.
                    return;
                }
                hostWorldReplayAwaitingGuest_ = false;
                hostWorldReplayGuestDeadlineMs_ = 0U;
                facade_.Log(
                    "[ENTITY_GRAPH_HOST][PIPE_RECONNECT] no guest stream returned before the bounded replay grace; retiring the retained graph causally");
            }
            if (!HasActiveHostAnimSceneDefinition()) {
                ResetHostWorldMirror(transport_.IsConnected());
            }
        } else if (!(localSlot_ == PlayerSlot::Guest &&
                     remoteStreaming &&
                     cutsceneSpectator_)) {
            ResetGuestWorldMirror();
        }
        // During a host scene the spectator facade keeps the already-hidden guest population suppressed.
        // Restoring it here exposed the guest's unrelated mission cast directly into the host camera.
        return;
    }

    if (*localSlot_ == PlayerSlot::Host) {
        // Build the bounded host graph lazily, once there is a connected safe session to own the stable entity IDs it allocates.
        if (!worldMirrorHost_.has_value()) {
            worldMirrorHost_.emplace(
                sessionEpoch_ == 0U ? 1U : sessionEpoch_,
                1'000U,
                kWorldMirrorMaximumEntities);
        }
        if (!hostWorldMirrorActive_) {
            facade_.Log(
                "entity graph v10.1 enabled: host authority, priority admission, 12m hysteresis, stable IDs, radius=80m, candidates=96, nodes=48");
        }
        hostWorldMirrorActive_ = true;
        if (soloGuestWorldViewEnabled_) {
            guestWorldMirrorActive_ = true;
            facade_.MaintainWorldMirrorGuest(
                true,
                true,
                kWorldMirrorRadiusMeters,
                true);
        }
        if (!FlushPendingHostWorldDespawns()) {
            facade_.Log(
                "[ENTITY_GRAPH_HOST][REPLAY] pending despawn tombstones were not fully delivered; new graph traffic remains paused to prevent orphan proxies");
            return;
        }
    // After reconnecting, resend NPCs in the right order.
    // This is a repair job, not something that should run every frame.
        if (forceHostWorldMirrorReplay_) {
            const auto replaySignals =
                worldMirrorHost_->ReplayStableSpawns();
            bool replaySent = true;
            for (const auto& signal : replaySignals) {
                if (!SendWorldMirrorSignal(signal)) {
                    replaySent = false;
                }
            }
            if (!replaySent) {
                facade_.Log(
                    "[ANIMSCENE_HYBRID][PIPE_RECONNECT] stable world spawn replay was not fully sent; current prepare attempt remains closed");
                return;
            }
            forceHostWorldMirrorReplay_ = false;
            hostAnimSceneReconnectPending_ = false;
            facade_.Log(
                "[ANIMSCENE_HYBRID][PIPE_RECONNECT] replayed " +
                std::to_string(replaySignals.size()) +
                " stable world spawns before the cached definition");
        }
        if (nowMs < nextWorldMirrorSampleMs_) {
            return;
        }

    // Ten times a second, look for nearby NPCs/objects.
    // Pick the important ones and send make/remove/update messages.
        const auto samples =
            facade_.SampleWorldEntities(
                kWorldMirrorRadiusMeters,
                kWorldMirrorMaximumCandidates);
        const auto signals =
            worldMirrorHost_->Update(samples, nowMs);
        for (const auto& signal : signals) {
            SendWorldMirrorSignal(signal);
        }
        if (nowMs >= nextWorldGraphDiagnosticsMs_) {
            const auto stats = worldMirrorHost_->Stats();
            facade_.Log(
                "[ENTITY_GRAPH_HOST] v10.1 nodes=" +
                std::to_string(stats.nodeCount) +
                ", edges=" +
                std::to_string(stats.edgeCount) +
                ", revision=" +
                std::to_string(stats.graphRevision) +
                ", emitted-this-sample=" +
                std::to_string(signals.size()) +
                ", capacity-evictions=" +
                std::to_string(stats.capacityEvictions) +
                ", selection-deferred=" +
                std::to_string(stats.selectionDeferred) +
                ", grace-retained=" +
                std::to_string(stats.graceRetained));
            nextWorldGraphDiagnosticsMs_ =
                nowMs + kWorldGraphDiagnosticsIntervalMilliseconds;
        }
        nextWorldMirrorSampleMs_ =
            nowMs +
            (hostCinematicMirrorWindow
                 ? kWorldMirrorCinematicIntervalMilliseconds
                 : kWorldMirrorIntervalMilliseconds);
        return;
    }

    if (!guestWorldMirrorActive_) {
        facade_.Log(
            "entity graph v10.1 enabled: guest desired-state registry, deferred mount dependencies and reversible ambient suppression");
    }
    guestWorldMirrorActive_ = true;
    facade_.MaintainWorldMirrorGuest(
        true,
        guestWorldAuthorityConfirmed_ ||
            guestCinematicMirrorWindow,
        kWorldMirrorRadiusMeters);
    if (nowMs >= nextWorldGraphDiagnosticsMs_) {
        const auto stats = guestWorldGraph_.Stats();
        facade_.Log(
            "[ENTITY_GRAPH_GUEST] v10.1 desired=" +
            std::to_string(stats.nodeCount) +
            ", active=" +
            std::to_string(stats.activeCount) +
            ", pending-parent=" +
            std::to_string(stats.pendingCount) +
            ", edges=" +
            std::to_string(stats.edgeCount) +
            ", accepted=" +
            std::to_string(stats.acceptedMessages) +
            ", duplicate=" +
            std::to_string(stats.duplicateMessages) +
            ", stale=" +
            std::to_string(stats.staleMessages) +
            ", capacity-rejected=" +
            std::to_string(stats.capacityRejectedMessages) +
            ", cascaded-despawn=" +
            std::to_string(stats.cascadedDespawns));
        nextWorldGraphDiagnosticsMs_ =
            nowMs + kWorldGraphDiagnosticsIntervalMilliseconds;
    }
    // When the guest shoots a copied NPC, ask the host to check the shot.
    // Only the host is allowed to damage the real NPC.
    if (const auto intent =
            facade_.SampleWorldDamageIntent(
                localEntityId_);
        intent.has_value()) {
        try {
            Frame frame;
            frame.header.type = MessageType::DamageIntent;
            frame.header.sequence = sequencer_.Next();
            frame.header.tick = nowMs;
            frame.payload = EncodeDamageIntent(*intent);
            SendBestEffort(std::move(frame));
        } catch (const std::exception&) {
            if (diagnostics_) {
                facade_.Log(
                    "guest damage intent was malformed and was not sent");
            }
        }
    }
}

bool BridgeRuntime::SendWorldMirrorSignal(
    const WorldMirrorSignal& signal) {
    Frame frame;
    frame.header.sequence = sequencer_.Next();
    frame.header.tick = facade_.TickMilliseconds();
    switch (signal.kind) {
        case WorldMirrorSignalKind::Spawn:
            frame.header.type = MessageType::EntitySpawn;
            frame.payload =
                EncodeWorldEntityState(signal.state);
            break;
        case WorldMirrorSignalKind::Update:
            frame.header.type = MessageType::EntityUpdate;
            frame.payload =
                EncodeWorldEntityState(signal.state);
            break;
        case WorldMirrorSignalKind::Despawn:
            frame.header.type = MessageType::EntityDespawn;
            frame.payload = EncodeEntityDespawn(
                EntityDespawnPayload{
                    signal.state.entityId});
            break;
    }
    // Send this NPC/object message to the sidecar.
    // If a remove message fails, remember it so the guest does not keep a leftover NPC copy.
    const bool delivered = SendBestEffort(std::move(frame));
    if (signal.kind == WorldMirrorSignalKind::Despawn &&
        signal.state.entityId.IsValid()) {
        if (delivered) {
            pendingHostWorldDespawns_.erase(signal.state.entityId);
        } else {
            pendingHostWorldDespawns_.insert_or_assign(
                signal.state.entityId,
                true);
        }
    }
    return delivered;
}

bool BridgeRuntime::FlushPendingHostWorldDespawns() {
    if (pendingHostWorldDespawns_.empty()) {
        return true;
    }
    std::vector<NetEntityId> pending;
    pending.reserve(pendingHostWorldDespawns_.size());
    for (const auto& [entityId, ignored] :
         pendingHostWorldDespawns_) {
        (void)ignored;
        pending.push_back(entityId);
    }
    std::ranges::sort(
        pending,
        [](const NetEntityId lhs, const NetEntityId rhs) {
            return lhs.Value() < rhs.Value();
        });
    bool deliveredAll = true;
    for (const auto entityId : pending) {
        WorldMirrorSignal tombstone;
        tombstone.kind = WorldMirrorSignalKind::Despawn;
        tombstone.state.entityId = entityId;
        if (!SendWorldMirrorSignal(tombstone)) {
            deliveredAll = false;
            break;
        }
    }
    return deliveredAll && pendingHostWorldDespawns_.empty();
}

void BridgeRuntime::ResetHostWorldMirror(
    const bool notifyPeer) {
    forceHostWorldMirrorReplay_ = false;
    hostAnimSceneReconnectPending_ = false;
    hostWorldReplayAwaitingGuest_ = false;
    hostWorldReplayGuestDeadlineMs_ = 0U;
    const bool wasActive = hostWorldMirrorActive_;
    const auto entityCount =
        worldMirrorHost_.has_value()
            ? worldMirrorHost_->Size()
            : 0U;
    hostWorldMirrorActive_ = false;
    nextWorldMirrorSampleMs_ = previousTickMs_;
    nextWorldGraphDiagnosticsMs_ = previousTickMs_;
    if (!notifyPeer) {
        pendingHostWorldDespawns_.clear();
    }
    if (!worldMirrorHost_.has_value()) {
        if (notifyPeer && localSlot_ == PlayerSlot::Host &&
            transport_.IsConnected()) {
            (void)FlushPendingHostWorldDespawns();
        }
        return;
    }
    // Reset creates explicit world despawns.
    // When connected they are sent so the guest can delete proxies instead of waiting for its own streaming.
    const auto signals = worldMirrorHost_->Reset();
    if (wasActive || entityCount != 0U) {
        facade_.Log(
            "world mirror alpha host cleanup: released " +
            std::to_string(entityCount) +
            " entities");
    }
    if (!notifyPeer ||
        localSlot_ != PlayerSlot::Host ||
        !transport_.IsConnected()) {
        return;
    }
    (void)FlushPendingHostWorldDespawns();
    for (const auto& signal : signals) {
        SendWorldMirrorSignal(signal);
    }
}

void BridgeRuntime::ResetGuestWorldMirror(
    const bool preserveSequenceTombstones) noexcept {
    const bool wasActive = guestWorldMirrorActive_;
    const auto entityCount = guestWorldGraph_.Stats().nodeCount;
    try {
        facade_.MaintainWorldMirrorGuest(
            false,
            false,
            kWorldMirrorRadiusMeters);
    } catch (...) {
        // The facade method is noexcept; retain a fail-safe for ABI changes.
    }
    guestWorldMirrorActive_ = false;
    if (!preserveSequenceTombstones) {
        guestWorldAuthorityConfirmed_ = false;
    }
    // Clear all guest NPC copies and their saved list.
    // This is why using entity resync makes NPCs disappear for a moment.
    (void)guestWorldGraph_.Reset(preserveSequenceTombstones);
    nextWorldGraphDiagnosticsMs_ = previousTickMs_;
    try {
        if (wasActive || entityCount != 0U) {
            facade_.Log(
                "world mirror alpha guest cleanup: removed " +
                std::to_string(entityCount) +
                " proxies and restored ambient peds");
        }
    } catch (...) {
        // Cleanup and unload remain noexcept even if diagnostics allocate.
    }
}

void BridgeRuntime::HandleEntitySpawn(
    const Frame& frame,
    const WorldEntityStatePayload& state) {
    if (localSlot_ != PlayerSlot::Guest &&
        !soloGuestWorldViewEnabled_) {
        return;
    }
    const auto localSample = facade_.SampleLocalPlayer();
    const bool cinematicMirrorWindow =
        remoteMissionCinematicState_.has_value() &&
        IsCinematicPresentationPhase(
            remoteMissionCinematicState_->phase);
    const bool safeMirrorWindow = soloGuestWorldViewEnabled_ ||
        localSample.has_value() &&
        !soloOverride_ &&
        ((!cutsceneSpectator_ && !localSample->cutsceneActive &&
          IsMissionWorldMirrorSafe()) || cinematicMirrorWindow) &&
        lastRemoteStateMs_.has_value() &&
        ElapsedMilliseconds(
            *lastRemoteStateMs_,
            previousTickMs_) <=
            kRemotePlayerFreshnessMilliseconds;
    if (!safeMirrorWindow) {
        if (previousTickMs_ >= nextMissionWorldWarningMs_) {
            facade_.Log(
                "[WARNING][MISSION_WORLD] entity spawn deferred: host mission window is transitioning");
            nextMissionWorldWarningMs_ = previousTickMs_ + 2'000U;
        }
        return;
    }
    // The pure graph admits the desired state first; only its ordered signals are allowed to create RDR2 proxies through the facade.
    const auto signals = guestWorldGraph_.ApplyState(
        state,
        frame.header.sequence);
    ApplyGuestWorldGraphSignals(signals);
}

void BridgeRuntime::HandleEntityUpdate(
    const Frame& frame,
    const WorldEntityStatePayload& state) {
    if (localSlot_ != PlayerSlot::Guest &&
        !soloGuestWorldViewEnabled_) {
        return;
    }
    const auto localSample = facade_.SampleLocalPlayer();
    const bool cinematicMirrorWindow =
        remoteMissionCinematicState_.has_value() &&
        IsCinematicPresentationPhase(
            remoteMissionCinematicState_->phase);
    const bool safeMirrorWindow = soloGuestWorldViewEnabled_ ||
        localSample.has_value() &&
        !soloOverride_ &&
        ((!cutsceneSpectator_ && !localSample->cutsceneActive &&
          IsMissionWorldMirrorSafe()) || cinematicMirrorWindow) &&
        lastRemoteStateMs_.has_value() &&
        ElapsedMilliseconds(
            *lastRemoteStateMs_,
            previousTickMs_) <=
            kRemotePlayerFreshnessMilliseconds;
    if (!safeMirrorWindow) {
        if (previousTickMs_ >= nextMissionWorldWarningMs_) {
            facade_.Log(
                "[WARNING][MISSION_WORLD] entity update deferred: host mission window is transitioning");
            nextMissionWorldWarningMs_ = previousTickMs_ + 2'000U;
        }
        return;
    }
    // The graph treats an update as an upsert, but does not expose a mounted child to the facade before its authoritative parent is registered.
    const auto signals = guestWorldGraph_.ApplyState(
        state,
        frame.header.sequence);
    ApplyGuestWorldGraphSignals(signals);
}

void BridgeRuntime::HandleEntityDespawn(
    const Frame& frame,
    const NetEntityId entityId) {
    if (localSlot_ != PlayerSlot::Guest &&
        !soloGuestWorldViewEnabled_) {
        return;
    }
    const auto signals = guestWorldGraph_.ApplyDespawn(
        entityId,
        frame.header.sequence);
    ApplyGuestWorldGraphSignals(signals);
}

void BridgeRuntime::ApplyGuestWorldGraphSignals(
    const std::span<const WorldMirrorSignal> signals) {
    // Keep the graph layer handle-free.
    // This switch is the narrow boundary where a replicated instruction becomes an RDR2 SDK side effect.
    for (const auto& signal : signals) {
        switch (signal.kind) {
            case WorldMirrorSignalKind::Spawn:
                (void)facade_.SpawnWorldEntityProxy(signal.state);
                break;
            case WorldMirrorSignalKind::Update:
                (void)facade_.UpdateWorldEntityProxy(signal.state);
                break;
            case WorldMirrorSignalKind::Despawn:
                facade_.DespawnWorldEntityProxy(
                    signal.state.entityId);
                break;
        }
    }
}

// Host samples the active Story mission and sends a small shared mission view.
// The guest uses that view for presentation; it never runs the host's scripts.
void BridgeRuntime::TickMissionAuthority(
    const LocalPlayerSample& sample,
    const PlayerSlot localSlot,
    const std::uint64_t nowMs,
    const bool checkpointRespawnConfirmed) {
    if (localSlot != PlayerSlot::Host ||
        !localEntityId_.IsValid() ||
        !transport_.IsConnected()) {
        return;
    }
    localSampleUnavailableSinceMs_ = 0U;

    const bool missionStarted =
        sample.missionActive &&
        (!localMissionInitialized_ ||
         !previousLocalMissionActive_);
    const auto completedMissionProbe =
        !sample.missionActive && previousLocalMissionActive_ &&
            localProgressionMissionId_ != 0U
        ? facade_.ProbeCampaignMission(localProgressionMissionId_)
        : std::optional<CampaignMissionProbe>{};
    const bool missionCompleted = completedMissionProbe.has_value() &&
        completedMissionProbe->missionId == localProgressionMissionId_ &&
        completedMissionProbe->wasCompleted;
    if (!localMissionInitialized_) {
        localMissionEpoch_ = sessionEpoch_ == 0U ? 1U : sessionEpoch_;
        localMissionRevision_ = 1U;
        localCheckpointGeneration_ = 1U;
    } else if (missionStarted) {
        localMissionEpoch_ = AdvanceNonZero(localMissionEpoch_);
        localMissionRevision_ = 1U;
        localCheckpointGeneration_ = 1U;
    }

    bool checkpointChanged{};
    if (checkpointRespawnConfirmed && sample.missionActive) {
        localCheckpointGeneration_ =
            AdvanceNonZero(localCheckpointGeneration_);
        localMissionRecoveryUntilMs_ =
            nowMs + kMissionRecoveryWindowMilliseconds;
        checkpointChanged = true;
        ResetHostWorldMirror(true);
        nextWorldMirrorSampleMs_ = 0U;
        facade_.Log(
            "[MISSION_CHECKPOINT][MISSION_FSM] vanilla checkpoint recovery confirmed; host graph and guest anchor resync requested");
    }

    const bool recovering =
        localMissionRecoveryUntilMs_ != 0U &&
        nowMs <= localMissionRecoveryUntilMs_;
    if (!recovering) {
        localMissionRecoveryUntilMs_ = 0U;
    }
    const bool cinematicPresentationLatched =
        localMissionCinematicState_.has_value() &&
        IsCinematicPresentationPhase(
            localMissionCinematicState_->phase);
    // RDR2 reports a short-lived cutscene/loading camera while an ordinary Story save is loading.
    // That is not mission authority and must never start the two-player cinematic resume barrier.
    // Once a real mission scene has been latched we retain it through its terminal hand-off, including the frame where the game has already cleared missionActive.
    const bool immediatePresentation =
        cinematicPresentationLatched ||
        (sample.missionActive &&
         ((sample.cutsceneActive && !localCinematicTerminalLatchActive_) ||
          sample.minigameActive));
    // A mission-owned loss of control covers forced AnimScenes, QTE/button prompts, and scripted vehicle/horse entry.
    // Do not trigger on those raw observations in free roam: ordinary mounting and scenarios remain co-op gameplay.
    // A brief debounce also filters frontend hand-offs.
    const bool debouncedMissionPresentation =
        sample.missionActive &&
        (sample.controlLocked ||
         (sample.scenarioActive && sample.controlLocked) ||
         (sample.vehicleEntryTransition && sample.controlLocked));
    bool classifiedPresentation = immediatePresentation;
    if (immediatePresentation) {
        spectatorClassifierCandidateSinceMs_.reset();
    } else if (debouncedMissionPresentation) {
        if (!spectatorClassifierCandidateSinceMs_.has_value()) {
            spectatorClassifierCandidateSinceMs_ = nowMs;
        }
        classifiedPresentation =
            nowMs - *spectatorClassifierCandidateSinceMs_ >=
            kMissionSpectatorControlLockDebounceMilliseconds;
    } else {
        spectatorClassifierCandidateSinceMs_.reset();
    }
    if (classifiedPresentation) {
        spectatorClassifierReleaseUntilMs_ = nowMs + 650U;
    }
    const bool effectiveCutsceneActive = classifiedPresentation ||
        (spectatorClassifierReleaseUntilMs_ != 0U &&
         nowMs < spectatorClassifierReleaseUntilMs_);
    const auto phase =
        soloOverride_
            ? MissionPhase::SoloOverride
            : recovering
                  ? MissionPhase::Recovery
                  : effectiveCutsceneActive
                        ? MissionPhase::Cutscene
                        : sample.missionActive
                              ? MissionPhase::Active
                              : MissionPhase::Idle;

    const Vec3 anchor = sample.position;
    const float heading = NormalizeHeading(sample.heading);
    std::uint8_t flags = static_cast<std::uint8_t>(
        MissionStateFlag::AnchorValid);
    if (sample.missionActive) {
        flags |= static_cast<std::uint8_t>(
            MissionStateFlag::MissionActive);
    }
    if (recovering) {
        flags |= static_cast<std::uint8_t>(
            MissionStateFlag::CheckpointRecovery);
    }
    if (effectiveCutsceneActive && sample.controlLocked) {
        flags |= static_cast<std::uint8_t>(
            MissionStateFlag::ScriptedControlLock);
    }
    if (effectiveCutsceneActive && sample.screenTransition) {
        flags |= static_cast<std::uint8_t>(
            MissionStateFlag::ScreenTransition);
    }
    if (effectiveCutsceneActive && sample.scenarioActive) {
        flags |= static_cast<std::uint8_t>(
            MissionStateFlag::ScenarioActivity);
    }
    if (effectiveCutsceneActive && sample.vehicleEntryTransition) {
        flags |= static_cast<std::uint8_t>(
            MissionStateFlag::ScriptedVehicleTransition);
    }
    if (effectiveCutsceneActive && sample.minigameActive) {
        flags |= static_cast<std::uint8_t>(
            MissionStateFlag::MinigameActivity);
    }

    MissionStatePayload next{
        localEntityId_,
        localMissionEpoch_,
        localMissionRevision_,
        localCheckpointGeneration_,
        phase,
        flags,
        anchor,
        heading};
    const bool controlChanged =
        !localMissionState_.has_value() ||
        localMissionState_->missionEpoch != next.missionEpoch ||
        localMissionState_->checkpointGeneration !=
            next.checkpointGeneration ||
        localMissionState_->phase != next.phase ||
        localMissionState_->flags != next.flags;
    const bool heartbeatDue =
        nowMs >= nextMissionStateHeartbeatMs_;
    bool anchorMateriallyChanged{};
    if (localMissionState_.has_value()) {
        const auto headingDelta = std::abs(
            std::fmod(
                next.hostHeading -
                    localMissionState_->hostHeading +
                    540.0F,
                360.0F) -
            180.0F);
        anchorMateriallyChanged =
            Distance(
                localMissionState_->hostAnchor,
                next.hostAnchor) >= 1.0F ||
            headingDelta >= 10.0F;
    }
    const bool publishNewAnchor =
        controlChanged ||
        (heartbeatDue && anchorMateriallyChanged);
    if (localMissionState_.has_value() &&
        !publishNewAnchor) {
        // Equal-revision heartbeats must be byte-equivalent for the sidecar cache.
        // Keep the last published anchor until movement is material and the one-second heartbeat is due.
        next.hostAnchor = localMissionState_->hostAnchor;
        next.hostHeading = localMissionState_->hostHeading;
    }
    if (localMissionState_.has_value() &&
        !missionStarted && publishNewAnchor) {
        localMissionRevision_ = AdvanceNonZero(localMissionRevision_);
        next.revision = localMissionRevision_;
    }
    localMissionInitialized_ = true;
    previousLocalMissionActive_ = sample.missionActive;
    if (!controlChanged && !heartbeatDue) {
        return;
    }
    localMissionState_ = next;

    Frame frame;
    frame.header.type = MessageType::MissionState;
    frame.header.sequence = sequencer_.Next();
    frame.header.tick = nowMs;
    frame.payload = EncodeMissionState(next);
    SendBestEffort(std::move(frame));
    if (missionCompleted && localMissionProgressionOffer_.has_value()) {
        const auto& offer = *localMissionProgressionOffer_;
        Frame completion;
        completion.header.type = MessageType::MissionProgression;
        completion.header.sequence = sequencer_.Next();
        completion.header.tick = nowMs;
        // Recognition does not imply save-write authority.
        // A catalog entry must explicitly be marked verified after a controlled two-save test before this bit can be emitted.
        const auto completionRating = completedMissionProbe.has_value() &&
                completedMissionProbe->wasCompleted
            ? completedMissionProbe->rating
            : 0U;
        const auto completionFlags = guestMissionProgressionEligible_ &&
                localMissionStartBarrierGuestStarted_ &&
                PropagatesCampaignMissionDerivedUnlocks(offer.missionId) &&
                completionRating >= 2U && completionRating <= 5U
            ? static_cast<std::uint8_t>(
                  MissionProgressionFlag::VerifiedCompletionMapping)
            : static_cast<std::uint8_t>(0U);
        completion.payload = EncodeMissionProgression(MissionProgressionPayload{
            offer.missionId, offer.missionEpoch, offer.eventId,
            MissionProgressionPhase::Completion, completionFlags,
            static_cast<std::uint8_t>(
                completionFlags != 0U ? completionRating : 0U),
            0});
        const auto completionPayload = DecodeMissionProgression(
            completion.payload);
        SendBestEffort(std::move(completion));
        if (completionFlags != 0U) {
            localMissionProgressionCompletion_ = completionPayload;
            guestMissionProgressionCompletionAcknowledged_ = false;
            nextMissionProgressionCompletionRetryMs_ =
                nowMs + kMissionProgressionCompletionRetryMilliseconds;
        }
        const auto definition = FindCampaignMission(offer.missionId);
        const auto name = definition.has_value() ? definition->scriptName : "unknown";
        facade_.Log(completionFlags != 0U
            ? "[MISSION_PROGRESSION] " + std::string{name} + " completion sent with verified mapping"
            : guestMissionProgressionEligible_
                ? "[MISSION_PROGRESSION] " + std::string{name} + " completion sent as audit-only; verified save mapping disabled"
                : "[MISSION_PROGRESSION] " + std::string{name} + " completed with no eligible guest; companion-only retained");
        localProgressionMissionId_ = 0U;
    }
    if (missionStarted || checkpointChanged) {
        // MissionState is queued first.
        // The guest can therefore quarantine a competing local mission or defer a recovery teleport before the host anchor command reaches it.
        HandleTeleportGuestRequest();
        facade_.Log(
            missionStarted
                ? "[MISSION_ANCHOR][MISSION_FSM] mission start requested a host-authoritative guest catch-up"
                : "[MISSION_ANCHOR][MISSION_CHECKPOINT] recovery requested a host-authoritative guest catch-up");
    }
    nextMissionStateHeartbeatMs_ =
        nowMs + kMissionStateHeartbeatMilliseconds;
    facade_.Log(
        "[MISSION_TX][MISSION_FSM] epoch=" +
        std::to_string(next.missionEpoch) +
        ", revision=" + std::to_string(next.revision) +
        ", phase=" +
        std::to_string(static_cast<std::uint8_t>(next.phase)) +
        ", checkpoint-gen=" +
        std::to_string(next.checkpointGeneration) +
        ", mission-active=" +
        std::to_string(sample.missionActive ? 1 : 0));
}

// Runs the "can both saves enter this mission?" handshake and keeps pending completion messages until the guest confirms it saved/applied them.
void BridgeRuntime::TickMissionProgression(
    const LocalPlayerSample& sample,
    const std::uint64_t nowMs) {
    if (!localSlot_.has_value() || !transport_.IsConnected()) return;

    if (*localSlot_ == PlayerSlot::Host) {
        if (localMissionProgressionCompletion_.has_value() &&
            !guestMissionProgressionCompletionAcknowledged_ &&
            nowMs >= nextMissionProgressionCompletionRetryMs_) {
            Frame retry;
            retry.header.type = MessageType::MissionProgression;
            retry.header.sequence = sequencer_.Next();
            retry.header.tick = nowMs;
            retry.payload = EncodeMissionProgression(
                *localMissionProgressionCompletion_);
            SendBestEffort(std::move(retry));
            nextMissionProgressionCompletionRetryMs_ =
                nowMs + kMissionProgressionCompletionRetryMilliseconds;
            facade_.Log("[MISSION_PROGRESSION] completion retransmitted awaiting guest acknowledgement");
        }
        for (const auto& definition : kCampaignMissionCatalog) {
            const auto probe = facade_.ProbeCampaignMission(definition.missionId);
            if (!sample.missionActive || !probe.has_value() || !probe->active ||
                probe->missionId != definition.missionId) {
                continue;
            }
            localProgressionMissionId_ = probe->missionId;
            if (!localMissionProgressionOffer_.has_value() ||
                localMissionProgressionOffer_->missionEpoch != localMissionEpoch_) {
                const auto unixMilliseconds = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
                // Mission epochs restart with the bridge process, so they are not a durable transaction identity.
                // Keep wall-clock time in the high bits and a mission/epoch discriminator below it.
                const auto eventId =
                    ((unixMilliseconds & 0x0000FFFFFFFFFFFFULL) << 16U) |
                    static_cast<std::uint64_t>(
                        (probe->missionId ^ localMissionEpoch_) & 0xFFFFU);
                MissionProgressionPayload offer{
                    probe->missionId, localMissionEpoch_, eventId,
                    MissionProgressionPhase::Offer, 0U};
                Frame frame;
                frame.header.type = MessageType::MissionProgression;
                frame.header.sequence = sequencer_.Next();
                frame.header.tick = nowMs;
                frame.payload = EncodeMissionProgression(offer);
                SendBestEffort(std::move(frame));
                localMissionProgressionOffer_ = offer;
                guestMissionProgressionEligible_ = false;
                localMissionStartBarrier_.reset();
                localMissionStartBarrierDeadlineMs_ = 0U;
                localMissionStartBarrierGuestStarted_ = false;
                localMissionProgressionCompletion_.reset();
                guestMissionProgressionCompletionAcknowledged_ = false;
                nextMissionProgressionCompletionRetryMs_ = 0U;
                facade_.Log("[MISSION_PROGRESSION] " +
                    std::string{definition.scriptName} +
                    " offer sent; awaiting guest startability confirmation");
            }
            if (guestMissionProgressionEligible_ &&
                localMissionProgressionOffer_.has_value() &&
                !localMissionStartBarrier_.has_value()) {
                MissionProgressionPayload barrier{
                    localMissionProgressionOffer_->missionId,
                    localMissionProgressionOffer_->missionEpoch,
                    localMissionProgressionOffer_->eventId,
                    MissionProgressionPhase::StartBarrierOpen, 0U};
                Frame frame;
                frame.header.type = MessageType::MissionProgression;
                frame.header.sequence = sequencer_.Next();
                frame.header.tick = nowMs;
                frame.payload = EncodeMissionProgression(barrier);
                SendBestEffort(std::move(frame));
                localMissionStartBarrier_ = barrier;
                localMissionStartBarrierDeadlineMs_ =
                    nowMs + kMissionStartBarrierMilliseconds;
                localMissionStartBarrierGuestStarted_ = false;
                facade_.Log("[MISSION_START_BARRIER] host mission active; exact guest start window opened for " +
                    std::string{definition.scriptName});
            }
            break;
        }
        if (localMissionStartBarrier_.has_value() &&
            !localMissionStartBarrierGuestStarted_ &&
            localMissionStartBarrierDeadlineMs_ != 0U &&
            nowMs > localMissionStartBarrierDeadlineMs_) {
            const auto& barrier = *localMissionStartBarrier_;
            Frame abort;
            abort.header.type = MessageType::MissionProgression;
            abort.header.sequence = sequencer_.Next();
            abort.header.tick = nowMs;
            abort.payload = EncodeMissionProgression(MissionProgressionPayload{
                barrier.missionId, barrier.missionEpoch, barrier.eventId,
                MissionProgressionPhase::StartBarrierAborted, 0U});
            SendBestEffort(std::move(abort));
            localMissionStartBarrier_.reset();
            localMissionStartBarrierDeadlineMs_ = 0U;
            guestMissionProgressionEligible_ = false;
            localMissionStartBarrierGuestStarted_ = false;
            facade_.Log("[MISSION_START_BARRIER] guest did not enter the exact mission before timeout; companion-only retained");
        }
        return;
    }

    // The guest never launches a script for the host.
    // It may only report the exact MissionData entry that its own vanilla Story prompt activated.
    if (!remoteMissionStartBarrier_.has_value() ||
        remoteMissionStartBarrierRejected_) {
        return;
    }
    const auto sendRejection = [&](const char* reason) {
        if (remoteMissionStartBarrierRejected_) return;
        const auto& barrier = *remoteMissionStartBarrier_;
        Frame rejection;
        rejection.header.type = MessageType::MissionProgression;
        rejection.header.sequence = sequencer_.Next();
        rejection.header.tick = nowMs;
        rejection.payload = EncodeMissionProgression(MissionProgressionPayload{
            barrier.missionId, barrier.missionEpoch, barrier.eventId,
            MissionProgressionPhase::GuestInstanceRejected, 0U});
        SendBestEffort(std::move(rejection));
        remoteMissionStartBarrierRejected_ = true;
        remoteMissionProgressionEligible_ = false;
        remoteMissionProgressionParticipated_ = false;
        facade_.Log(std::string{"[MISSION_START_BARRIER] guest rejected matching mission instance: "} + reason);
    };
    if (remoteMissionStartBarrierDeadlineMs_ == 0U ||
        nowMs > remoteMissionStartBarrierDeadlineMs_) {
        sendRejection("start window expired");
        return;
    }
    const auto& barrier = *remoteMissionStartBarrier_;
    const auto exactProbe = facade_.ProbeCampaignMission(barrier.missionId);
    const bool exactInstanceActive = exactProbe.has_value() &&
        exactProbe->missionId == barrier.missionId && exactProbe->active;
    if (nowMs < remoteMissionStartBarrierPromptArmedAtMs_) {
        if (exactInstanceActive) {
            sendRejection("matching local mission was already entering before the host barrier armed");
        }
        return;
    }
    if (!exactInstanceActive) {
        if (remoteMissionStartBarrierGuestStarted_ &&
            remoteMissionStartBarrierReleased_) {
            if (!exactProbe.has_value() || !exactProbe->wasCompleted) {
                sendRejection("matching local mission ended without completion");
                return;
            }
            remoteMissionStartBarrier_.reset();
            remoteMissionStartBarrierDeadlineMs_ = 0U;
            remoteMissionStartBarrierPromptArmedAtMs_ = 0U;
            remoteMissionStartBarrierGuestStarted_ = false;
            remoteMissionStartBarrierReleased_ = false;
            nextGuestMissionInstanceStartedRetryMs_ = 0U;
            facade_.Log("[MISSION_START_BARRIER] matching guest mission ended; normal guest mission isolation restored");
            return;
        }
        // The facade has no generic "start this exact MissionData" native.
        // Detect a different known Story mission immediately and restore the normal quarantine on the next tick instead of accepting it.
        for (const auto& definition : kCampaignMissionCatalog) {
            if (definition.missionId == barrier.missionId) continue;
            const auto other = facade_.ProbeCampaignMission(definition.missionId);
            if (other.has_value() && other->missionId == definition.missionId &&
                other->active) {
                sendRejection("a different local mission became active");
                return;
            }
        }
        return;
    }
    if (!remoteMissionStartBarrierGuestStarted_ ||
        nowMs >= nextGuestMissionInstanceStartedRetryMs_) {
        Frame started;
        started.header.type = MessageType::MissionProgression;
        started.header.sequence = sequencer_.Next();
        started.header.tick = nowMs;
        started.payload = EncodeMissionProgression(MissionProgressionPayload{
            barrier.missionId, barrier.missionEpoch, barrier.eventId,
            MissionProgressionPhase::GuestInstanceStarted, 0U});
        SendBestEffort(std::move(started));
        remoteMissionStartBarrierGuestStarted_ = true;
        nextGuestMissionInstanceStartedRetryMs_ =
            nowMs + kMissionStartBarrierRetryMilliseconds;
        facade_.Log("[MISSION_START_BARRIER] guest entered exact local MissionData instance; awaiting host release");
    }
    // Once the exact instance is observed, keep the narrow permission for its lifetime.
    // The next inactive probe above restores ordinary isolation.
    remoteMissionStartBarrierDeadlineMs_ = ~std::uint64_t{0U};
    (void)sample;
}

// Handles the other player's answer in the mission progression handshake.
// Every branch matches mission ID, epoch, and event ID before changing state.
void BridgeRuntime::HandleRemoteMissionProgression(
    const MissionProgressionPayload& payload) {
    if (!localSlot_.has_value()) return;
    constexpr auto kGuestCanStart = static_cast<std::uint8_t>(
        MissionProgressionFlag::GuestCanStart);
    constexpr auto kVerifiedMapping = static_cast<std::uint8_t>(
        MissionProgressionFlag::VerifiedCompletionMapping);
    if (*localSlot_ == PlayerSlot::Guest) {
        const auto sendAppliedAcknowledgement = [&]() {
            Frame acknowledgement;
            acknowledgement.header.type = MessageType::MissionProgression;
            acknowledgement.header.sequence = sequencer_.Next();
            acknowledgement.header.tick = facade_.TickMilliseconds();
            acknowledgement.payload = EncodeMissionProgression(
                MissionProgressionPayload{
                    payload.missionId, payload.missionEpoch, payload.eventId,
                    MissionProgressionPhase::Applied, 0U});
            SendBestEffort(std::move(acknowledgement));
        };
        if (payload.phase == MissionProgressionPhase::Offer) {
            const auto probe = facade_.ProbeCampaignMission(payload.missionId);
            const bool eligible = probe.has_value() &&
                probe->missionId == payload.missionId && probe->canStart &&
                !probe->active;
            remoteMissionProgressionOffer_ = payload;
            remoteMissionProgressionEligible_ = eligible;
            remoteMissionProgressionParticipated_ = false;
            remoteMissionProgressionAppliedEventId_.reset();
            Frame reply;
            reply.header.type = MessageType::MissionProgression;
            reply.header.sequence = sequencer_.Next();
            reply.header.tick = facade_.TickMilliseconds();
            reply.payload = EncodeMissionProgression(MissionProgressionPayload{
                payload.missionId, payload.missionEpoch, payload.eventId,
                MissionProgressionPhase::Eligibility,
                eligible ? kGuestCanStart : 0U});
            SendBestEffort(std::move(reply));
            facade_.Log(eligible
                ? "[MISSION_PROGRESSION] guest confirmed matching mission is startable"
                : "[MISSION_PROGRESSION] guest rejected progression: matching mission is not startable in this save");
        } else if (payload.phase == MissionProgressionPhase::StartBarrierOpen) {
            const bool matchedOffer = remoteMissionProgressionOffer_.has_value() &&
                remoteMissionProgressionOffer_->missionId == payload.missionId &&
                remoteMissionProgressionOffer_->missionEpoch == payload.missionEpoch &&
                remoteMissionProgressionOffer_->eventId == payload.eventId;
            const auto probe = facade_.ProbeCampaignMission(payload.missionId);
            const bool stillStartable = probe.has_value() &&
                probe->missionId == payload.missionId && probe->canStart &&
                !probe->active;
            if (!matchedOffer || !remoteMissionProgressionEligible_ ||
                !stillStartable) {
                Frame rejection;
                rejection.header.type = MessageType::MissionProgression;
                rejection.header.sequence = sequencer_.Next();
                rejection.header.tick = facade_.TickMilliseconds();
                rejection.payload = EncodeMissionProgression(
                    MissionProgressionPayload{
                        payload.missionId, payload.missionEpoch, payload.eventId,
                        MissionProgressionPhase::GuestInstanceRejected, 0U});
                SendBestEffort(std::move(rejection));
                remoteMissionProgressionEligible_ = false;
                remoteMissionProgressionParticipated_ = false;
                facade_.Log("[MISSION_START_BARRIER] refused host start window: exact mission is no longer locally startable");
            } else {
                remoteMissionStartBarrier_ = payload;
                const auto nowMs = facade_.TickMilliseconds();
                remoteMissionStartBarrierDeadlineMs_ = nowMs +
                    kMissionStartBarrierMilliseconds;
                remoteMissionStartBarrierPromptArmedAtMs_ = nowMs +
                    kMissionStartBarrierPromptArmMilliseconds;
                remoteMissionStartBarrierGuestStarted_ = false;
                remoteMissionStartBarrierReleased_ = false;
                remoteMissionStartBarrierRejected_ = false;
                nextGuestMissionInstanceStartedRetryMs_ = 0U;
                facade_.Log("[MISSION_START_BARRIER] exact guest mission start window received; verifying idle state before opening the matching vanilla prompt");
            }
        } else if (payload.phase == MissionProgressionPhase::StartBarrierReleased) {
            const bool matched = remoteMissionStartBarrier_.has_value() &&
                remoteMissionStartBarrier_->missionId == payload.missionId &&
                remoteMissionStartBarrier_->missionEpoch == payload.missionEpoch &&
                remoteMissionStartBarrier_->eventId == payload.eventId;
            if (matched && remoteMissionStartBarrierGuestStarted_) {
                remoteMissionStartBarrierReleased_ = true;
                remoteMissionProgressionParticipated_ = true;
                facade_.Log("[MISSION_START_BARRIER] host acknowledged matching guest mission instance");
            }
        } else if (payload.phase == MissionProgressionPhase::StartBarrierAborted) {
            const bool matched = remoteMissionStartBarrier_.has_value() &&
                remoteMissionStartBarrier_->missionId == payload.missionId &&
                remoteMissionStartBarrier_->missionEpoch == payload.missionEpoch &&
                remoteMissionStartBarrier_->eventId == payload.eventId;
            if (matched) {
                remoteMissionStartBarrier_.reset();
                remoteMissionStartBarrierDeadlineMs_ = 0U;
                remoteMissionStartBarrierPromptArmedAtMs_ = 0U;
                remoteMissionStartBarrierGuestStarted_ = false;
                remoteMissionStartBarrierReleased_ = false;
                remoteMissionStartBarrierRejected_ = true;
                remoteMissionProgressionEligible_ = false;
                remoteMissionProgressionParticipated_ = false;
                facade_.Log("[MISSION_START_BARRIER] host closed start window; companion-only mission isolation restored");
            }
        } else if (payload.phase == MissionProgressionPhase::Completion) {
            const bool matched = remoteMissionProgressionOffer_.has_value() &&
                remoteMissionProgressionOffer_->missionId == payload.missionId &&
                remoteMissionProgressionOffer_->missionEpoch == payload.missionEpoch &&
                remoteMissionProgressionOffer_->eventId == payload.eventId;
            if (!matched || !remoteMissionProgressionEligible_ ||
                !remoteMissionProgressionParticipated_ ||
                (payload.flags & kVerifiedMapping) == 0U ||
                payload.completionCashAward != 0) {
                facade_.Log("[MISSION_PROGRESSION] completion retained as audit-only; no verified guest save mapping");
            } else if (remoteMissionProgressionAppliedEventId_.has_value() &&
                       *remoteMissionProgressionAppliedEventId_ ==
                           payload.eventId) {
                facade_.Log("[MISSION_PROGRESSION] duplicate completion ignored after successful guest mapping");
                // The host may have missed the first acknowledgement.
                // Repeat it for every idempotent completion replay.
                sendAppliedAcknowledgement();
            } else if (!facade_.ApplyCampaignMissionCompletion(
                           payload.missionId, payload.eventId,
                           payload.completionRating)) {
                facade_.Log("[MISSION_PROGRESSION] verified completion mapping failed closed");
            } else if (payload.completionCashAward > 0 &&
                       !facade_.ApplyCampaignMissionCashAward(
                           payload.eventId, payload.completionCashAward)) {
                // Do not consume the event.
                // The completion and catalog rewards are idempotent, so a retransmission can retry the one missing award instead of marking a partial result done.
                facade_.Log("[MISSION_PROGRESSION] guest cash award failed; completion event remains retryable");
            } else {
                remoteMissionProgressionAppliedEventId_ = payload.eventId;
                facade_.Log("[MISSION_PROGRESSION] verified guest completion mapping applied exactly once");
                sendAppliedAcknowledgement();
            }
        }
        return;
    }
    if (payload.phase == MissionProgressionPhase::Eligibility &&
        localMissionProgressionOffer_.has_value() &&
        payload.missionId == localMissionProgressionOffer_->missionId &&
        payload.missionEpoch == localMissionProgressionOffer_->missionEpoch &&
        payload.eventId == localMissionProgressionOffer_->eventId) {
        guestMissionProgressionEligible_ = (payload.flags & kGuestCanStart) != 0U;
        facade_.Log(guestMissionProgressionEligible_
            ? "[MISSION_PROGRESSION] host accepted guest mission eligibility"
            : "[MISSION_PROGRESSION] host retained companion-only mode for this guest save");
    } else if (payload.phase == MissionProgressionPhase::GuestInstanceStarted &&
        localMissionStartBarrier_.has_value() &&
        payload.missionId == localMissionStartBarrier_->missionId &&
        payload.missionEpoch == localMissionStartBarrier_->missionEpoch &&
        payload.eventId == localMissionStartBarrier_->eventId) {
        localMissionStartBarrierGuestStarted_ = true;
        Frame release;
        release.header.type = MessageType::MissionProgression;
        release.header.sequence = sequencer_.Next();
        release.header.tick = facade_.TickMilliseconds();
        release.payload = EncodeMissionProgression(MissionProgressionPayload{
            payload.missionId, payload.missionEpoch, payload.eventId,
            MissionProgressionPhase::StartBarrierReleased, 0U});
        SendBestEffort(std::move(release));
        facade_.Log("[MISSION_START_BARRIER] host released matching guest mission instance; objective/checkpoint authority remains host-owned");
    } else if (payload.phase == MissionProgressionPhase::GuestInstanceRejected &&
        localMissionStartBarrier_.has_value() &&
        payload.missionId == localMissionStartBarrier_->missionId &&
        payload.missionEpoch == localMissionStartBarrier_->missionEpoch &&
        payload.eventId == localMissionStartBarrier_->eventId) {
        Frame abort;
        abort.header.type = MessageType::MissionProgression;
        abort.header.sequence = sequencer_.Next();
        abort.header.tick = facade_.TickMilliseconds();
        abort.payload = EncodeMissionProgression(MissionProgressionPayload{
            payload.missionId, payload.missionEpoch, payload.eventId,
            MissionProgressionPhase::StartBarrierAborted, 0U});
        SendBestEffort(std::move(abort));
        localMissionStartBarrier_.reset();
        localMissionStartBarrierDeadlineMs_ = 0U;
        localMissionStartBarrierGuestStarted_ = false;
        guestMissionProgressionEligible_ = false;
        facade_.Log("[MISSION_START_BARRIER] guest rejected matching mission instance; companion-only retained");
    } else if (payload.phase == MissionProgressionPhase::Applied &&
        localMissionProgressionCompletion_.has_value() &&
        payload.missionId == localMissionProgressionCompletion_->missionId &&
        payload.missionEpoch == localMissionProgressionCompletion_->missionEpoch &&
        payload.eventId == localMissionProgressionCompletion_->eventId) {
        guestMissionProgressionCompletionAcknowledged_ = true;
        facade_.Log("[MISSION_PROGRESSION] guest acknowledged durable completion mapping");
    }
}

// Host reads the current Story objective text and sends it only when it changes.
void BridgeRuntime::TickMissionObjective(
    const LocalPlayerSample& sample,
    const std::uint64_t nowMs) {
    if (localSlot_ != PlayerSlot::Host || !transport_.IsConnected() ||
        !localEntityId_.IsValid() || !sample.missionActive ||
        nowMs < nextMissionObjectiveSampleMs_) {
        return;
    }
    nextMissionObjectiveSampleMs_ = nowMs +
        kMissionObjectiveSampleMilliseconds;
    const auto objective = facade_.SampleMissionObjective();
    if (!objective.has_value() || objective->text.empty()) {
        return;
    }
    const auto fingerprint = MissionObjectiveFingerprint(objective->text);
    if (localMissionObjective_.has_value() &&
        localMissionObjective_->missionEpoch == localMissionEpoch_ &&
        localMissionObjective_->fingerprint == fingerprint) {
        return;
    }
    localMissionObjectiveRevision_ =
        AdvanceNonZero(localMissionObjectiveRevision_);
    MissionObjectivePayload payload{
        localEntityId_, localMissionEpoch_, localMissionObjectiveRevision_,
        fingerprint, objective->text};
    try {
        Frame frame;
        frame.header.type = MessageType::MissionObjective;
        frame.header.sequence = sequencer_.Next();
        frame.header.tick = nowMs;
        frame.payload = EncodeMissionObjective(payload);
        SendBestEffort(std::move(frame));
        localMissionObjective_ = std::move(payload);
        facade_.Log("[MISSION_OBJECTIVE] host objective published");
    } catch (const std::exception&) {
        facade_.Log("[MISSION_OBJECTIVE] rejected invalid native objective text");
    }
}

// Guest receives host objective text for display.
// It is never used to drive a mission script or change local Story Mode progress.
void BridgeRuntime::HandleRemoteMissionObjective(
    const Frame& frame,
    const MissionObjectivePayload& objective) {
    if (localSlot_ != PlayerSlot::Guest ||
        !remoteMissionState_.has_value() ||
        objective.hostEntityId != remoteMissionState_->hostEntityId ||
        objective.missionEpoch != remoteMissionState_->missionEpoch) {
        facade_.Log("[MISSION_OBJECTIVE] rejected objective outside active host mission authority");
        return;
    }
    if (remoteMissionObjective_.has_value() &&
        objective.missionEpoch == remoteMissionObjective_->missionEpoch &&
        objective.revision <= remoteMissionObjective_->revision) {
        return;
    }
    remoteMissionObjective_ = objective;
    facade_.Log("[MISSION_OBJECTIVE] accepted host objective, sender-tick=" +
        std::to_string(frame.header.tick));
}

// Sends the host's current state for a bridge-owned ambient encounter so both players see the same encounter phase, place, roster, and outcome.
void BridgeRuntime::PublishAmbientEncounterState(
    const AmbientEncounterInstance& instance,
    const std::uint64_t nowMs) {
    if (!localEntityId_.IsValid()) return;
    try {
        Frame frame;
        frame.header.type = MessageType::AmbientEncounterState;
        frame.header.sequence = sequencer_.Next();
        frame.header.tick = nowMs;
        frame.payload = EncodeAmbientEncounterState(AmbientEncounterStatePayload{
            localEntityId_, instance.instanceId, instance.profile, instance.phase,
            AmbientEncounterRejection::None, instance.anchor, instance.radiusMeters,
            instance.rosterSeed, instance.rosterCount, instance.hostStartTick,
            instance.exactEventId, instance.guestDisposition});
        SendBestEffort(std::move(frame));
    } catch (...) {
        facade_.Log("[AMBIENT_ENCOUNTER] failed to serialize host state");
    }
}

// Creates a host-approved ambient encounter after the guest suggestion passed all safety/distance checks.
// The host owns the spawned NPC graph from here.
void BridgeRuntime::StartPreparedAmbientEncounter(const std::uint64_t nowMs) {
    if (localSlot_ != PlayerSlot::Host ||
        !ambientEncounterCoordinator_.Active().has_value()) {
        return;
    }
    auto& instance = *ambientEncounterCoordinator_.Active();
    if (instance.phase != AmbientEncounterPhase::Preparing) {
        return;
    }
    localExactEncounterPreflightDeadlineMs_ = 0U;
    nextExactEncounterPreflightPublishMs_ = 0U;
    if (facade_.BeginAmbientEncounterPresentation(instance)) {
        (void)ambientEncounterCoordinator_.Advance(
            AmbientEncounterPhase::Active);
        PublishAmbientEncounterState(
            *ambientEncounterCoordinator_.Active(), nowMs);
        return;
    }
    (void)ambientEncounterCoordinator_.Advance(
        AmbientEncounterPhase::Abandoned);
    PublishAmbientEncounterState(
        *ambientEncounterCoordinator_.Active(), nowMs);
    localAmbientEncounterTerminalAtMs_ = nowMs;
    facade_.Log("[AMBIENT_ENCOUNTER] host could not materialize prepared scene");
}

// Host receives a guest suggestion for an ambient encounter.
// It validates the request instead of allowing the guest to create arbitrary local NPC events.
void BridgeRuntime::HandleRemoteAmbientEncounterProposal(
    const Frame& frame, const AmbientEncounterProposalPayload& payload) {
    if (localSlot_ != PlayerSlot::Host || !remoteReplicaId_.IsValid() ||
        payload.guestEntityId != remoteReplicaId_) {
        facade_.Log("[AMBIENT_ENCOUNTER] rejected unauthoritative guest proposal");
        return;
    }
    const auto host = facade_.SampleLocalPlayer();
    const auto distance = facade_.HostGuestDistanceMeters();
    AmbientEncounterProposal proposal{payload.proposalId, payload.profile, payload.anchor,
        payload.radiusMeters, payload.localEvidenceHash, payload.suggestedRosterSeed};
    // A host-originated exact event starts in Preparing.
    // The guest answers this authenticated preflight with the same instance ID, either the exact script ID (participant) or the companion sentinel.
    // It is not a second event proposal and cannot replace the host's anchor or outcome.
    if (ambientEncounterCoordinator_.Active().has_value()) {
        auto& active = *ambientEncounterCoordinator_.Active();
        if (active.phase == AmbientEncounterPhase::Preparing &&
            active.exactEventId == kExtortionEncounter.scriptId &&
            payload.proposalId == active.instanceId) {
            const bool validPreflight =
                payload.profile == kExtortionEncounter.profile &&
                payload.suggestedRosterSeed == active.rosterSeed &&
                (payload.localEvidenceHash == kExtortionEncounter.scriptId ||
                 payload.localEvidenceHash ==
                    kExtortionCompanionPreflightEvidence);
            if (!validPreflight) {
                facade_.Log("[EXACT_ENCOUNTER] rejected malformed Extortion guest preflight");
                return;
            }
            active.guestDisposition =
                payload.localEvidenceHash == kExtortionEncounter.scriptId
                    ? AmbientEncounterPeerDisposition::Participant
                    : AmbientEncounterPeerDisposition::Companion;
            facade_.Log("[EXACT_ENCOUNTER] guest preflight=" +
                std::string{active.guestDisposition ==
                    AmbientEncounterPeerDisposition::Participant
                        ? "participant"
                        : "companion"});
            StartPreparedAmbientEncounter(facade_.TickMilliseconds());
            return;
        }
    }
    const bool exactExtortionEvidence =
        payload.profile == kExtortionEncounter.profile &&
        payload.localEvidenceHash == kExtortionEncounter.scriptId;
    const auto* catalogEvidence =
        FindBridgeOwnedEncounter(payload.localEvidenceHash);
    if (!exactExtortionEvidence &&
        (catalogEvidence == nullptr ||
         catalogEvidence->profile != payload.profile)) {
        try {
            Frame reply;
            reply.header.type = MessageType::AmbientEncounterState;
            reply.header.sequence = sequencer_.Next();
            reply.header.tick = facade_.TickMilliseconds();
            reply.payload = EncodeAmbientEncounterState(
                AmbientEncounterStatePayload{
                    localEntityId_, payload.proposalId, payload.profile,
                    AmbientEncounterPhase::Proposed,
                    AmbientEncounterRejection::UnsupportedProfile,
                    {}, 0.0F, 0U, 0U, 0U});
            SendBestEffort(std::move(reply));
        } catch (...) {}
        facade_.Log(
            "[AMBIENT_ENCOUNTER] rejected unreviewed or mismatched guest evidence");
        return;
    }
    // Extortion is an exact-ID adaptation, not a generic hostage heuristic.
    // A guest becomes a participant only when both save processes currently observe the same script-owned beat.
    // Otherwise the host-owned bridge scene still starts as companion-only; it cannot change the guest save.
    if (exactExtortionEvidence) {
        const auto hostExact = facade_.SampleExactEncounterObservation();
        if (!hostExact.has_value() || !hostExact->locallyEligible ||
            hostExact->scriptId != kExtortionEncounter.scriptId) {
            try {
                Frame reply;
                reply.header.type = MessageType::AmbientEncounterState;
                reply.header.sequence = sequencer_.Next();
                reply.header.tick = facade_.TickMilliseconds();
                reply.payload = EncodeAmbientEncounterState(AmbientEncounterStatePayload{
                    localEntityId_, payload.proposalId, payload.profile,
                    AmbientEncounterPhase::Proposed,
                    AmbientEncounterRejection::HostUnavailable,
                    {}, 0.0F, 0U, 0U, 0U});
                SendBestEffort(std::move(reply));
            } catch (...) {}
            facade_.Log("[EXACT_ENCOUNTER] Extortion proposal retained as guest-local; host exact ID is absent");
            return;
        }
    }
    const AmbientEncounterHostContext context{
        transport_.IsConnected() && host.has_value(),
        !host.has_value() || host->missionActive || host->cutsceneActive || host->screenTransition,
        host.has_value() && !host->downed && !host->ragdoll && !host->inWater,
        distance.value_or(std::numeric_limits<float>::infinity())};
    const auto instanceId = ++localAmbientEncounterInstanceId_ == 0U
        ? ++localAmbientEncounterInstanceId_ : localAmbientEncounterInstanceId_;
    const auto result = ambientEncounterCoordinator_.ProposeFromGuest(
        proposal, context, instanceId, frame.header.tick == 0U ? previousTickMs_ : frame.header.tick);
    if (result != AmbientEncounterRejection::None) {
        try {
            Frame reply;
            reply.header.type = MessageType::AmbientEncounterState;
            reply.header.sequence = sequencer_.Next();
            reply.header.tick = facade_.TickMilliseconds();
            reply.payload = EncodeAmbientEncounterState(AmbientEncounterStatePayload{
                localEntityId_, payload.proposalId, payload.profile, AmbientEncounterPhase::Proposed,
                result, {}, 0.0F, 0U, 0U, 0U});
            SendBestEffort(std::move(reply));
        } catch (...) {}
        facade_.Log("[AMBIENT_ENCOUNTER] host declined guest proposal");
        return;
    }
    auto& instance = *ambientEncounterCoordinator_.Active();
    if (exactExtortionEvidence) {
        // The guest found Extortion first.
        // The host's exact-ID check above already proved both local saves are eligible.
        instance.exactEventId = kExtortionEncounter.scriptId;
        instance.guestDisposition =
            AmbientEncounterPeerDisposition::Participant;
    }
    PublishAmbientEncounterState(instance, facade_.TickMilliseconds());
    StartPreparedAmbientEncounter(facade_.TickMilliseconds());
    facade_.Log("[AMBIENT_ENCOUNTER] guest proposal adopted by host");
}

void BridgeRuntime::HandleRemoteAmbientEncounterState(
    const Frame& frame, const AmbientEncounterStatePayload& state) {
    if (localSlot_ != PlayerSlot::Guest || !remoteReplicaId_.IsValid() ||
        state.hostEntityId != remoteReplicaId_) {
        facade_.Log("[AMBIENT_ENCOUNTER] rejected non-host state");
        return;
    }
    if (state.phase == AmbientEncounterPhase::Proposed) {
        if (state.rejection != AmbientEncounterRejection::None &&
            state.instanceId == localAmbientEncounterProposalId_) {
            localAmbientEncounterProposalExpiresMs_ = 0U;
            notificationText_ = "Host unavailable — encounter remains local";
            notificationUntilMs_ = facade_.TickMilliseconds() + 3'000U;
            facade_.Log("[AMBIENT_ENCOUNTER] host rejected guest proposal");
        }
        return;
    }
    if (remoteAmbientEncounter_.has_value() &&
        state.instanceId < remoteAmbientEncounter_->instanceId) return;
    remoteAmbientEncounter_ = state;
    AmbientEncounterInstance presentation{state.instanceId, state.profile, state.phase,
        state.anchor, state.radiusMeters, state.rosterSeed, state.rosterCount,
        state.hostStartTick, true, false, state.exactEventId,
        state.guestDisposition};
    if (state.phase == AmbientEncounterPhase::Preparing) {
        remoteAmbientEncounterTerminalAtMs_ = 0U;
        if (state.exactEventId == kExtortionEncounter.scriptId) {
            if (remoteExactEncounterPreflightInstanceId_ != state.instanceId) {
                const auto observed = facade_.SampleExactEncounterObservation();
                const bool locallyEligible = observed.has_value() &&
                    observed->locallyEligible &&
                    observed->scriptId == kExtortionEncounter.scriptId;
                try {
                    Frame preflight;
                    preflight.header.type = MessageType::AmbientEncounterProposal;
                    preflight.header.sequence = sequencer_.Next();
                    preflight.header.tick = facade_.TickMilliseconds();
                    preflight.payload = EncodeAmbientEncounterProposal(
                        AmbientEncounterProposalPayload{
                            localEntityId_, state.instanceId,
                            kExtortionEncounter.profile, state.anchor,
                            state.radiusMeters,
                            locallyEligible
                                ? kExtortionEncounter.scriptId
                                : kExtortionCompanionPreflightEvidence,
                            state.rosterSeed});
                    SendBestEffort(std::move(preflight));
                    remoteExactEncounterPreflightInstanceId_ = state.instanceId;
                    notificationText_ = locallyEligible
                        ? "Extortion matched — joining as participant"
                        : "Extortion unavailable — joining as companion";
                    notificationUntilMs_ = facade_.TickMilliseconds() + 3'000U;
                    facade_.Log(
                        std::string{"[EXACT_ENCOUNTER] guest preflight="} +
                        (locallyEligible ? "participant" : "companion") +
                        ", loot-policy=local-vanilla-generic-only");
                } catch (...) {
                    facade_.Log("[EXACT_ENCOUNTER] guest preflight could not be sent");
                }
            }
            // The host starts only after it has recorded this reply (or a bounded timeout).
            // A guest does not create a speculative roster.
            return;
        }
        if (!facade_.BeginAmbientEncounterPresentation(presentation)) {
            facade_.Log("[AMBIENT_ENCOUNTER] guest presentation unavailable; host state remains authoritative");
        } else {
            notificationText_ = "Shared encounter started";
            notificationUntilMs_ = facade_.TickMilliseconds() + 3'000U;
        }
    } else if (state.phase == AmbientEncounterPhase::Active) {
        remoteAmbientEncounterTerminalAtMs_ = 0U;
        remoteExactEncounterPreflightInstanceId_ = 0U;
        if (state.exactEventId == kExtortionEncounter.scriptId &&
            state.guestDisposition == AmbientEncounterPeerDisposition::Unknown) {
            facade_.Log("[EXACT_ENCOUNTER] rejected active state without guest disposition");
            return;
        }
        if (!facade_.BeginAmbientEncounterPresentation(presentation)) {
            facade_.Log("[AMBIENT_ENCOUNTER] guest presentation unavailable; host state remains authoritative");
        } else {
            notificationText_ = state.exactEventId ==
                    kExtortionEncounter.scriptId &&
                    state.guestDisposition ==
                        AmbientEncounterPeerDisposition::Companion
                ? "Shared Extortion started — companion loot is generic only"
                : "Shared encounter started";
            notificationUntilMs_ = facade_.TickMilliseconds() + 3'000U;
            if (state.exactEventId == kExtortionEncounter.scriptId) {
                facade_.Log(
                    std::string{"[EXACT_ENCOUNTER] guest active="} +
                    (state.guestDisposition ==
                        AmbientEncounterPeerDisposition::Participant
                        ? "participant"
                        : "companion") +
                    ", scene=world-mirror, loot-policy=local-vanilla-generic-only");
            }
        }
    } else if (IsTerminalAmbientEncounterPhase(state.phase)) {
        // The host retains defeated bridge peds for the bounded loot window.
        // Their normal world-entity despawns are the guest's cleanup signal; deleting presentation here would remove a corpse before the guest can use the ordinary local loot prompt.
        notificationText_ = "Shared encounter resolved";
        notificationUntilMs_ = facade_.TickMilliseconds() + 3'000U;
        remoteAmbientEncounterTerminalAtMs_ = facade_.TickMilliseconds();
    }
    (void)frame;
}

// Advances the host-owned ambient encounter and sends its state as it starts, runs, succeeds, fails, or is abandoned.
void BridgeRuntime::TickAmbientEncounter(const LocalPlayerSample& sample,
    const PlayerSlot localSlot, const std::uint64_t nowMs) {
    if (!transport_.IsConnected() || !localEntityId_.IsValid()) return;
    const bool unsafeForAmbientActivity = sample.missionActive ||
        sample.cutsceneActive || sample.screenTransition || sample.downed ||
        sample.ragdoll;
    if (unsafeForAmbientActivity) {
        // A terminal encounter has no further player interaction or outcome work to do.
        // Do not let a subsequent mission/loading transition pin its bridge-owned actors forever; the normal retention window still applies before host cleanup/despawn.
        if (localSlot == PlayerSlot::Host &&
            ambientEncounterCoordinator_.Active().has_value()) {
            const auto& instance = *ambientEncounterCoordinator_.Active();
            if (IsTerminalAmbientEncounterPhase(instance.phase) &&
                nowMs >= localAmbientEncounterTerminalAtMs_ +
                    kAmbientEncounterTerminalRetentionMilliseconds) {
                facade_.ClearAmbientEncounterPresentation(instance.instanceId);
                ambientEncounterCoordinator_.ClearTerminal();
            }
        }
        return;
    }
    // A guest's own Story state may be quarantined by mission isolation, so also inspect the authoritative remote mission state before permitting a free-roam activity.
    // This prevents a stale local observation from opening an encounter during the host's Story sequence.
    if (remoteMissionState_.has_value() &&
        (remoteMissionState_->flags & static_cast<std::uint8_t>(
            MissionStateFlag::MissionActive)) != 0U) return;
    if (remoteMissionCinematicState_.has_value() &&
        IsCinematicPresentationPhase(remoteMissionCinematicState_->phase)) return;
    if (localSlot == PlayerSlot::Host) {
        if (ambientEncounterCoordinator_.Active().has_value()) {
            auto& instance = *ambientEncounterCoordinator_.Active();
            if (instance.phase == AmbientEncounterPhase::Active) {
                const auto outcome = facade_.SampleAmbientEncounterOutcome(instance.instanceId);
                const auto distance = facade_.HostGuestDistanceMeters();
                if (outcome.has_value() && IsTerminalAmbientEncounterPhase(*outcome)) {
                    (void)ambientEncounterCoordinator_.Advance(*outcome);
                } else if (distance.has_value() && *distance > kAmbientEncounterAbandonDistanceMeters) {
                    (void)ambientEncounterCoordinator_.Advance(AmbientEncounterPhase::Abandoned);
                }
                if (IsTerminalAmbientEncounterPhase(instance.phase)) {
                    PublishAmbientEncounterState(instance, nowMs);
                    localAmbientEncounterTerminalAtMs_ = nowMs;
                }
            } else if (instance.phase == AmbientEncounterPhase::Preparing &&
                instance.exactEventId == kExtortionEncounter.scriptId &&
                localExactEncounterPreflightDeadlineMs_ != 0U &&
                nowMs >= localExactEncounterPreflightDeadlineMs_) {
                // A missing guest reply is not an error.
                // Start the same bridge scene as companion-only so the pair can still fight together without exposing a save reward.
                instance.guestDisposition =
                    AmbientEncounterPeerDisposition::Companion;
                facade_.Log("[EXACT_ENCOUNTER] guest preflight timed out; companion scene selected");
                StartPreparedAmbientEncounter(nowMs);
            } else if (instance.phase == AmbientEncounterPhase::Preparing &&
                instance.exactEventId == kExtortionEncounter.scriptId &&
                nowMs >= nextExactEncounterPreflightPublishMs_) {
                PublishAmbientEncounterState(instance, nowMs);
                nextExactEncounterPreflightPublishMs_ =
                    nowMs + kExactEncounterPreflightRepublishMilliseconds;
            } else if (IsTerminalAmbientEncounterPhase(instance.phase) &&
                nowMs >= localAmbientEncounterTerminalAtMs_ + kAmbientEncounterTerminalRetentionMilliseconds) {
                facade_.ClearAmbientEncounterPresentation(instance.instanceId);
                ambientEncounterCoordinator_.ClearTerminal();
            }
            return;
        }
        // Host detection starts an Extortion scene even when the guest does not have the encounter available.
        // The eventual guest presentation is companion-only in that case: it can fight and loot only generic bridge-bandit supplies, never an exact-event reward.
        if (const auto exact = facade_.SampleExactEncounterObservation();
            exact.has_value() && exact->locallyEligible &&
            exact->scriptId == kExtortionEncounter.scriptId) {
            const AmbientEncounterProposal proposal{++localAmbientEncounterInstanceId_,
                kExtortionEncounter.profile, exact->anchor, 25.0F,
                exact->scriptId, exact->scriptId ^ static_cast<std::uint32_t>(nowMs)};
            const AmbientEncounterHostContext context{true, false, !sample.inWater,
                facade_.HostGuestDistanceMeters().value_or(std::numeric_limits<float>::infinity())};
            if (ambientEncounterCoordinator_.StartFromHost(proposal, context,
                proposal.proposalId, nowMs) == AmbientEncounterRejection::None) {
                auto& instance = *ambientEncounterCoordinator_.Active();
                instance.exactEventId = kExtortionEncounter.scriptId;
                instance.guestDisposition =
                    AmbientEncounterPeerDisposition::Unknown;
                localExactEncounterPreflightDeadlineMs_ =
                    nowMs + kExactEncounterPreflightTimeoutMilliseconds;
                nextExactEncounterPreflightPublishMs_ =
                    nowMs + kExactEncounterPreflightRepublishMilliseconds;
                PublishAmbientEncounterState(instance, nowMs);
                facade_.Log("[EXACT_ENCOUNTER] host detected Extortion; awaiting guest preflight");
                return;
            }
        }
        const auto observed = facade_.SampleAmbientEncounterObservation();
        if (!observed.has_value()) return;
        const AmbientEncounterProposal proposal{++localAmbientEncounterInstanceId_, observed->profile,
            observed->anchor, observed->radiusMeters, observed->localEvidenceHash, observed->suggestedRosterSeed};
        const AmbientEncounterHostContext context{true, false, !sample.inWater,
            facade_.HostGuestDistanceMeters().value_or(std::numeric_limits<float>::infinity())};
        if (ambientEncounterCoordinator_.StartFromHost(proposal, context, proposal.proposalId, nowMs) != AmbientEncounterRejection::None) return;
        auto& instance = *ambientEncounterCoordinator_.Active();
        PublishAmbientEncounterState(instance, nowMs);
        StartPreparedAmbientEncounter(nowMs);
        return;
    }
    if (remoteAmbientEncounter_.has_value()) {
        if (!IsTerminalAmbientEncounterPhase(remoteAmbientEncounter_->phase) ||
            remoteAmbientEncounterTerminalAtMs_ == 0U ||
            nowMs < remoteAmbientEncounterTerminalAtMs_ +
                kAmbientEncounterTerminalRetentionMilliseconds) {
            return;
        }
        // Host WorldMirror despawns arrive while the terminal window is held.
        // Clear the local state afterwards so one completed encounter cannot permanently block a later guest proposal.
        facade_.ClearAmbientEncounterPresentation(
            remoteAmbientEncounter_->instanceId);
        remoteAmbientEncounter_.reset();
        remoteAmbientEncounterTerminalAtMs_ = 0U;
    }
    if (localAmbientEncounterProposalExpiresMs_ > nowMs) return;
    if (const auto exact = facade_.SampleExactEncounterObservation();
        exact.has_value() && exact->locallyEligible &&
        exact->scriptId == kExtortionEncounter.scriptId) {
        localAmbientEncounterProposalId_ = ++localAmbientEncounterInstanceId_;
        localAmbientEncounterProposalExpiresMs_ = nowMs + kAmbientEncounterProposalTimeoutMilliseconds;
        try {
            Frame proposal;
            proposal.header.type = MessageType::AmbientEncounterProposal;
            proposal.header.sequence = sequencer_.Next(); proposal.header.tick = nowMs;
            proposal.payload = EncodeAmbientEncounterProposal(AmbientEncounterProposalPayload{
                localEntityId_, localAmbientEncounterProposalId_, kExtortionEncounter.profile,
                exact->anchor, 25.0F, exact->scriptId,
                exact->scriptId ^ static_cast<std::uint32_t>(nowMs)});
            SendBestEffort(std::move(proposal));
            notificationText_ = "Extortion spotted — checking host";
            notificationUntilMs_ = nowMs + 3'000U;
        } catch (...) { localAmbientEncounterProposalExpiresMs_ = 0U; }
        return;
    }
    const auto observed = facade_.SampleAmbientEncounterObservation();
    if (!observed.has_value()) return;
    localAmbientEncounterProposalId_ = ++localAmbientEncounterInstanceId_;
    localAmbientEncounterProposalExpiresMs_ = nowMs + kAmbientEncounterProposalTimeoutMilliseconds;
    try {
        Frame proposal;
        proposal.header.type = MessageType::AmbientEncounterProposal;
        proposal.header.sequence = sequencer_.Next(); proposal.header.tick = nowMs;
        proposal.payload = EncodeAmbientEncounterProposal(AmbientEncounterProposalPayload{
            localEntityId_, localAmbientEncounterProposalId_, observed->profile, observed->anchor,
            observed->radiusMeters, observed->localEvidenceHash, observed->suggestedRosterSeed});
        SendBestEffort(std::move(proposal));
        notificationText_ = "Encounter spotted — waiting for host";
        notificationUntilMs_ = nowMs + 3'000U;
    } catch (...) { localAmbientEncounterProposalExpiresMs_ = 0U; }
}

// Host watches admitted Story dialogue and sends only approved catalogue cues.
// This shares presentation, not private mission script control.
void BridgeRuntime::TickMissionDialogue(
    const PlayerSlot localSlot,
    const std::uint64_t nowMs) {
    if (localSlot == PlayerSlot::Guest) {
        if (!pendingHostMissionDialogueCue_.has_value() ||
            nowMs < pendingHostMissionDialogueDueMs_) {
            return;
        }
        const auto cue = *pendingHostMissionDialogueCue_;
        pendingHostMissionDialogueCue_.reset();
        pendingHostMissionDialogueDueMs_ = 0U;
        const auto missionId = remoteMissionProgressionOffer_.has_value()
            ? remoteMissionProgressionOffer_->missionId
            : 0U;
        const auto state = facade_.PresentHostMissionDialogue(
            missionId, cue.rootId)
            ? MissionDialogueReadyState::Ready
            : MissionDialogueReadyState::ProfileUnavailable;
        SendMissionDialogueReady(cue, state);
        return;
    }
    if (localSlot != PlayerSlot::Host || !localMissionState_.has_value() ||
        localMissionState_->phase != MissionPhase::Active ||
        (localMissionState_->flags & static_cast<std::uint16_t>(
            MissionStateFlag::MissionActive)) == 0U) {
        return;
    }
    if (!localMissionProgressionOffer_.has_value() ||
        localMissionProgressionOffer_->missionEpoch !=
            localMissionState_->missionEpoch) {
        return;
    }
    const auto missionId = localMissionProgressionOffer_->missionId;
    for (const auto& sample : facade_.SampleMissionDialogue(missionId)) {
        if (sample.profileId == 0U || sample.rootId == 0U ||
            !sample.rootLoaded || !sample.rootPlaying ||
            !sample.requiredRolesBound ||
            !HasCampaignMissionDialogueProfile(missionId, sample.profileId) ||
            !IsCampaignMissionDialogueRoot(missionId, sample.rootId)) {
            continue;
        }
        const bool isCurrentCue = localMissionDialogueCue_.has_value() &&
            localMissionDialogueCue_->missionEpoch ==
                localMissionState_->missionEpoch &&
            localMissionDialogueCue_->checkpointGeneration ==
                localMissionState_->checkpointGeneration &&
            localMissionDialogueCue_->profileId == sample.profileId &&
            localMissionDialogueCue_->rootId == sample.rootId &&
            localMissionDialogueCue_->lineIndex == sample.lineIndex;
        if (isCurrentCue) {
            const bool guestReady = remoteMissionDialogueReady_.has_value() &&
                remoteMissionDialogueReady_->dialogueSequence ==
                    localMissionDialogueCue_->dialogueSequence &&
                remoteMissionDialogueReady_->state ==
                    MissionDialogueReadyState::Ready;
            if (guestReady || nowMs < lastMissionDialogueCueSentMs_ + 1'000U) {
                continue;
            }
            try {
                Frame retry;
                retry.header.type = MessageType::MissionDialogueCue;
                retry.header.sequence = sequencer_.Next();
                retry.header.tick = nowMs;
                retry.payload = EncodeMissionDialogueCue(*localMissionDialogueCue_);
                SendBestEffort(std::move(retry));
                lastMissionDialogueCueSentMs_ = nowMs;
                facade_.Log("[MISSION_DIALOGUE] host cue retransmitted while guest presentation is unavailable");
            } catch (const std::exception&) {
                facade_.Log("[MISSION_DIALOGUE] host cue retransmission rejected");
            }
            return;
        }
        MissionDialogueCuePayload cue{
            localEntityId_, localMissionState_->missionEpoch,
            localMissionState_->checkpointGeneration,
            AdvanceNonZero(localMissionDialogueSequence_), sample.profileId,
            sample.rootId, sample.lineIndex, nowMs + 150U};
        try {
            Frame frame;
            frame.header.type = MessageType::MissionDialogueCue;
            frame.header.sequence = sequencer_.Next();
            frame.header.tick = nowMs;
            frame.payload = EncodeMissionDialogueCue(cue);
            SendBestEffort(std::move(frame));
            localMissionDialogueCue_ = cue;
            lastMissionDialogueCueSentMs_ = nowMs;
            facade_.Log("[MISSION_DIALOGUE] host cue published");
            if (diagnostics_) {
                notificationText_ =
                    "DIALOGUE CUE p" + std::to_string(cue.profileId) +
                    " r" + std::to_string(cue.rootId) +
                    " line " + std::to_string(cue.lineIndex);
                notificationUntilMs_ = nowMs + 2'500U;
            }
        } catch (const std::exception&) {
            facade_.Log("[MISSION_DIALOGUE] invalid host dialogue observation rejected");
        }
        // One cue per tick bounds bandwidth and preserves causal ordering.
        return;
    }
}

// Guest receives a host dialogue cue, checks its matching local resources, and later replies with a ready/failure state for this exact cue identity.
void BridgeRuntime::HandleRemoteMissionDialogueCue(
    const Frame& frame,
    const MissionDialogueCuePayload& cue) {
    if (localSlot_ != PlayerSlot::Guest || !remoteMissionState_.has_value() ||
        !remoteMissionProgressionOffer_.has_value() ||
        cue.hostEntityId != remoteMissionState_->hostEntityId ||
        cue.missionEpoch != remoteMissionState_->missionEpoch ||
        cue.checkpointGeneration != remoteMissionState_->checkpointGeneration ||
        remoteMissionState_->phase != MissionPhase::Active) {
        facade_.Log("[MISSION_DIALOGUE] rejected cue outside active host mission");
        return;
    }
    const auto missionId = remoteMissionProgressionOffer_->missionId;
    if (remoteMissionProgressionOffer_->missionEpoch != cue.missionEpoch ||
        !HasCampaignMissionDialogueProfile(missionId, cue.profileId) ||
        !IsCampaignMissionDialogueRoot(missionId, cue.rootId)) {
        facade_.Log("[MISSION_DIALOGUE] rejected cue for a different mission profile");
        return;
    }
    if (remoteMissionDialogueCue_.has_value() &&
        cue.dialogueSequence < remoteMissionDialogueCue_->dialogueSequence) {
        facade_.Log("[MISSION_DIALOGUE] rejected stale cue");
        return;
    }
    if (remoteMissionDialogueCue_.has_value() &&
        cue.dialogueSequence == remoteMissionDialogueCue_->dialogueSequence &&
        remoteMissionDialogueReady_.has_value() &&
        remoteMissionDialogueReady_->dialogueSequence == cue.dialogueSequence &&
        remoteMissionDialogueReady_->state == MissionDialogueReadyState::Ready) {
        return;
    }
    remoteMissionDialogueCue_ = cue;
    MissionDialogueReadyState state = MissionDialogueReadyState::ProfileUnavailable;
    if (!remoteMissionStartBarrierReleased_) {
        // Host and guest clocks have independent origins.
        // Preserve the host's intentional lead time as a bounded local delay measured from the received host frame, rather than treating its raw tick as an absolute guest timestamp.
        const auto leadMs = cue.hostStartTick > frame.header.tick
            ? std::min<std::uint64_t>(cue.hostStartTick - frame.header.tick,
                  500U)
            : 0U;
        pendingHostMissionDialogueCue_ = cue;
        pendingHostMissionDialogueDueMs_ =
            facade_.TickMilliseconds() + leadMs;
        facade_.Log("[MISSION_DIALOGUE] companion-only guest queued local audio presentation");
        return;
    } else {
        for (const auto& sample : facade_.SampleMissionDialogue(missionId)) {
            if (sample.profileId != cue.profileId || sample.rootId != cue.rootId) {
                continue;
            }
            state = !sample.rootLoaded
                        ? MissionDialogueReadyState::RootNotLoaded
                        : !sample.requiredRolesBound
                              ? MissionDialogueReadyState::ProfileUnavailable
                              : !sample.rootPlaying
                                    ? MissionDialogueReadyState::RootNotPlaying
                                    : sample.lineIndex != cue.lineIndex
                                          ? MissionDialogueReadyState::StaleCue
                                          : MissionDialogueReadyState::Ready;
            break;
        }
    }
    SendMissionDialogueReady(cue, state);
    (void)frame;
}

void BridgeRuntime::SendMissionDialogueReady(
    const MissionDialogueCuePayload& cue,
    const MissionDialogueReadyState state) {
    MissionDialogueReadyPayload ready{
        cue.hostEntityId, cue.missionEpoch, cue.checkpointGeneration,
        cue.dialogueSequence, cue.profileId, cue.rootId, cue.lineIndex, state};
    try {
        Frame response;
        response.header.type = MessageType::MissionDialogueReady;
        response.header.sequence = sequencer_.Next();
        response.header.tick = facade_.TickMilliseconds();
        response.payload = EncodeMissionDialogueReady(ready);
        SendBestEffort(std::move(response));
        facade_.Log(state == MissionDialogueReadyState::Ready
                        ? "[MISSION_DIALOGUE] guest local conversation ready; vanilla playback retained"
                        : "[MISSION_DIALOGUE] guest unavailable; vanilla dialogue fallback retained");
        if (diagnostics_) {
            notificationText_ = state == MissionDialogueReadyState::Ready
                                    ? "DIALOGUE READY: local vanilla line matched"
                                    : "DIALOGUE FALLBACK: local root/line not ready";
            notificationUntilMs_ = facade_.TickMilliseconds() + 2'500U;
        }
    } catch (const std::exception&) {
        facade_.Log("[MISSION_DIALOGUE] rejected invalid guest readiness");
    }
}

void BridgeRuntime::HandleRemoteMissionDialogueReady(
    const Frame& frame,
    const MissionDialogueReadyPayload& ready) {
    if (localSlot_ != PlayerSlot::Host || !localMissionDialogueCue_.has_value() ||
        ready.hostEntityId != localEntityId_ ||
        ready.missionEpoch != localMissionDialogueCue_->missionEpoch ||
        ready.checkpointGeneration != localMissionDialogueCue_->checkpointGeneration ||
        ready.dialogueSequence != localMissionDialogueCue_->dialogueSequence ||
        ready.profileId != localMissionDialogueCue_->profileId ||
        ready.rootId != localMissionDialogueCue_->rootId ||
        ready.lineIndex != localMissionDialogueCue_->lineIndex) {
        facade_.Log("[MISSION_DIALOGUE] rejected readiness for a non-current cue");
        return;
    }
    remoteMissionDialogueReady_ = ready;
    facade_.Log(ready.state == MissionDialogueReadyState::Ready
                    ? "[MISSION_DIALOGUE] guest confirmed matching vanilla dialogue"
                    : "[MISSION_DIALOGUE] guest not ready; segment remains vanilla fallback");
    (void)frame;
}

// Keeps host/guest mission loading and spectator rules in a safe state while RDR2 transitions between checkpoints, loading screens, and normal play.
void BridgeRuntime::TickMissionLoadingAuthority(
    const std::uint64_t nowMs) {
    if (localSlot_ != PlayerSlot::Host ||
        !localEntityId_.IsValid() ||
        !transport_.IsConnected() ||
        !localMissionState_.has_value()) {
        localSampleUnavailableSinceMs_ = 0U;
        return;
    }
    if (localSampleUnavailableSinceMs_ == 0U ||
        nowMs < localSampleUnavailableSinceMs_) {
        localSampleUnavailableSinceMs_ = nowMs;
        return;
    }
    if (nowMs - localSampleUnavailableSinceMs_ <
        kMissionLoadingDetectionMilliseconds) {
        return;
    }

    auto next = *localMissionState_;
    const bool phaseChanged =
        next.phase != MissionPhase::Loading;
    if (phaseChanged) {
        localMissionRevision_ =
            AdvanceNonZero(localMissionRevision_);
        next.revision = localMissionRevision_;
        next.phase = MissionPhase::Loading;
        next.flags &= ~static_cast<std::uint8_t>(
            MissionStateFlag::CheckpointRecovery);
    }
    const bool heartbeatDue =
        nowMs >= nextMissionStateHeartbeatMs_;
    localMissionState_ = next;
    if (!phaseChanged && !heartbeatDue) {
        return;
    }

    Frame frame;
    frame.header.type = MessageType::MissionState;
    frame.header.sequence = sequencer_.Next();
    frame.header.tick = nowMs;
    frame.payload = EncodeMissionState(next);
    SendBestEffort(std::move(frame));
    nextMissionStateHeartbeatMs_ =
        nowMs + kMissionStateHeartbeatMilliseconds;
    facade_.Log(
        phaseChanged
            ? "[MISSION_TX][MISSION_FSM] host player unavailable for 350ms; entered loading spectator phase"
            : "[MISSION_TX] loading-phase heartbeat");
}

// Guest applies a newer host mission snapshot.
// Older epoch/revision packets are ignored so reconnect traffic cannot roll mission presentation backward.
void BridgeRuntime::HandleRemoteMissionState(
    const Frame& frame,
    const MissionStatePayload& state) {
    if (localSlot_ != PlayerSlot::Guest) {
        facade_.Log(
            "[WARNING][MISSION_RX] rejected mission state from a non-host identity");
        return;
    }
    auto& expectedHostId =
        playerEntityIds_[SlotIndex(PlayerSlot::Host)];
    if (expectedHostId.IsValid() &&
        state.hostEntityId != expectedHostId) {
        facade_.Log(
            "[WARNING][MISSION_RX] rejected mission state from a mismatched host identity");
        return;
    }
    if (!expectedHostId.IsValid()) {
        // The authenticated sidecar deliberately replays MissionState before the cached world graph.
        // Allow that reliable control frame to seed the host identity; the first PlayerState must naturally carry the same ID or the next mission frame is rejected.
        expectedHostId = state.hostEntityId;
        facade_.Log(
            "[MISSION_RX] bootstrapped authenticated host identity from mission-state replay");
    }
    const auto disposition =
        remoteMissionSequences_.Observe(frame.header.sequence);
    if (disposition == SequenceDisposition::Duplicate ||
        disposition == SequenceDisposition::Stale) {
        facade_.Log(
            "[WARNING][MISSION_RX] duplicate/stale mission frame ignored");
        return;
    }
    if (remoteMissionState_.has_value() &&
        (state.missionEpoch < remoteMissionState_->missionEpoch ||
         (state.missionEpoch == remoteMissionState_->missionEpoch &&
          state.revision < remoteMissionState_->revision))) {
        facade_.Log(
            "[WARNING][MISSION_RX] stale mission epoch/revision ignored");
        return;
    }

    const bool checkpointChanged =
        remoteMissionState_.has_value() &&
        (state.missionEpoch != remoteMissionState_->missionEpoch ||
         state.checkpointGeneration !=
             remoteMissionState_->checkpointGeneration);
    const auto isPresentationUnsafe = [](const MissionPhase phase) {
        return phase == MissionPhase::Cutscene ||
               phase == MissionPhase::Loading ||
               phase == MissionPhase::Recovery ||
               phase == MissionPhase::SoloOverride;
    };
    const bool enteredPresentationIsolation =
        isPresentationUnsafe(state.phase) &&
        (!remoteMissionState_.has_value() ||
         !isPresentationUnsafe(remoteMissionState_->phase));
    remoteMissionState_ = state;
    if (checkpointChanged || state.phase != MissionPhase::Active) {
        // The facade owns only the companion-only conversation and its hidden proxy actors.
        // Never leave that audio scene alive across a retry, cutscene/loading transition, mission end, or disconnect replay.
        facade_.ClearHostMissionDialoguePresentation();
        pendingHostMissionDialogueCue_.reset();
        pendingHostMissionDialogueDueMs_ = 0U;
    }
    if (checkpointChanged) {
        remoteMissionDialogueCue_.reset();
        pendingHostMissionDialogueCue_.reset();
        remoteMissionDialogueReady_.reset();
        remoteMissionDialogueCueSequences_.Reset();
        remoteMissionDialogueReadySequences_.Reset();
        remoteMissionCameraState_.reset();
        remoteMissionCameraReceivedAtMs_.reset();
        remoteMissionCameraSequences_.Reset();
        // Clear only host-derived proxies.
        // Keep the reversible local-population mask alive until spectator exits, otherwise guest-local mission NPCs leak into the host's cutscene and fight with the next graph rebuild.
        const auto resetSignals = guestWorldGraph_.Reset(true);
        ApplyGuestWorldGraphSignals(resetSignals);
        guestWorldMirrorActive_ = false;
        remoteSnapshots_.Reset();
        latestRemoteAnimationState_.reset();
        latestRemoteAnimationReceivedAtMs_.reset();
        facade_.Log(
            "[MISSION_CHECKPOINT][MISSION_RX] new checkpoint generation; guest proxies and interpolation history reset while the local population mask stays quarantined");
    } else if (enteredPresentationIsolation) {
        // A cinematic phase is not a world-generation boundary.
        // The V31.7 horse cinematic entered with 26 valid host nodes, erased them here, and immediately selected PROXY_CAST_FALLBACK with no cast left to render.
        // Retain the stable graph; spectator masking already hides the guest-local Story population, while the exact/fallback presentation decides which host proxies remain visible.
        facade_.Log(
            "[MISSION_SPECTATOR][MISSION_WORLD] host scene entered; retained stable host cast while local mission actors remain masked; nodes=" +
            std::to_string(guestWorldGraph_.Stats().nodeCount));
    }
    facade_.Log(
        "[MISSION_RX][MISSION_FSM] epoch=" +
        std::to_string(state.missionEpoch) +
        ", revision=" + std::to_string(state.revision) +
        ", phase=" +
        std::to_string(static_cast<std::uint8_t>(state.phase)) +
        ", checkpoint-gen=" +
        std::to_string(state.checkpointGeneration) +
        ", sender-tick=" + std::to_string(frame.header.tick) +
        ", local-tick=" + std::to_string(previousTickMs_));
}

// Host broadcasts a compact cinematic snapshot so the guest can enter, resume, or leave the same presentation without sharing RDR2-only scene handles.
void BridgeRuntime::PublishMissionCinematicState(
    const MissionCinematicPhase phase,
    const std::uint16_t flags,
    const Vec3& anchor,
    const float heading,
    const std::uint64_t nowMs) {
    if (localSlot_ != PlayerSlot::Host ||
        !localEntityId_.IsValid() ||
        !localMissionState_.has_value() ||
        localMissionCinematicGeneration_ == 0U) {
        return;
    }

    const bool newGeneration =
        !localMissionCinematicState_.has_value() ||
        localMissionCinematicState_->missionEpoch != localMissionEpoch_ ||
        localMissionCinematicState_->cinematicGeneration !=
            localMissionCinematicGeneration_;
    if (newGeneration) {
        localMissionCinematicRevision_ = 1U;
    }
    MissionCinematicStatePayload next{
        localEntityId_,
        localMissionEpoch_,
        localMissionCinematicGeneration_,
        localMissionCinematicRevision_,
        localCheckpointGeneration_,
        phase,
        flags,
        anchor,
        NormalizeHeading(heading)};
    const auto sameControl = [](
                                 const MissionCinematicStatePayload& left,
                                 const MissionCinematicStatePayload& right) {
        return left.missionEpoch == right.missionEpoch &&
               left.cinematicGeneration == right.cinematicGeneration &&
               left.checkpointGeneration == right.checkpointGeneration &&
               left.phase == right.phase &&
               left.flags == right.flags &&
               left.resumeAnchor.x == right.resumeAnchor.x &&
               left.resumeAnchor.y == right.resumeAnchor.y &&
               left.resumeAnchor.z == right.resumeAnchor.z &&
               left.resumeHeading == right.resumeHeading;
    };
    const bool changed =
        !localMissionCinematicState_.has_value() ||
        !sameControl(*localMissionCinematicState_, next);
    if (!newGeneration && changed) {
        localMissionCinematicRevision_ =
            AdvanceNonZero(localMissionCinematicRevision_);
        next.revision = localMissionCinematicRevision_;
    } else if (!changed &&
               nowMs < nextMissionCinematicHeartbeatMs_) {
        return;
    } else if (!changed) {
        next = *localMissionCinematicState_;
    }

    const bool terminal =
        next.phase == MissionCinematicPhase::Completed ||
        next.phase == MissionCinematicPhase::Aborted;
    if (terminal && changed &&
        localAnimSceneDefinition_.has_value()) {
        (void)SendAnimSceneControl(
            AnimSceneControlKind::HostAbort,
            AnimSceneControlReason::StaleGeneration,
            0U,
            0U,
            0.0F,
            0.0F,
            nowMs);
    }

    localMissionCinematicState_ = next;
    nextMissionCinematicHeartbeatMs_ =
        nowMs + kMissionCinematicHeartbeatMilliseconds;
    if (transport_.IsConnected()) {
        Frame frame;
        frame.header.type = MessageType::MissionCinematicState;
        frame.header.sequence = sequencer_.Next();
        frame.header.tick = nowMs;
        frame.payload = EncodeMissionCinematicState(next);
        SendBestEffort(std::move(frame));
    }
    if (changed) {
        facade_.Log(
            "[MISSION_CINEMATIC][TX] generation=" +
            std::to_string(next.cinematicGeneration) +
            ", revision=" + std::to_string(next.revision) +
            ", phase=" +
            std::to_string(static_cast<std::uint8_t>(next.phase)) +
            ", flags=" + std::to_string(next.flags));
    }
    if (terminal) {
        ResetAnimSceneHybridState(true);
    }
}

void BridgeRuntime::SendMissionCinematicAction(
    const MissionCinematicActionKind kind,
    const std::uint16_t flags,
    const std::uint64_t nowMs) {
    if (localSlot_ != PlayerSlot::Guest ||
        !remoteMissionCinematicState_.has_value() ||
        !transport_.IsConnected()) {
        return;
    }
    localMissionCinematicActionId_ =
        AdvanceNonZero(localMissionCinematicActionId_);
    const MissionCinematicActionPayload action{
        remoteMissionCinematicState_->hostEntityId,
        remoteMissionCinematicState_->missionEpoch,
        remoteMissionCinematicState_->cinematicGeneration,
        localMissionCinematicActionId_,
        kind,
        static_cast<std::uint8_t>(PlayerSlot::Guest),
        flags};
    Frame frame;
    frame.header.type = MessageType::MissionCinematicAction;
    frame.header.sequence = sequencer_.Next();
    frame.header.tick = nowMs;
    frame.payload = EncodeMissionCinematicAction(action);
    SendBestEffort(std::move(frame));
}

void BridgeRuntime::RequestCutsceneSkip(const std::uint64_t nowMs) {
    if (localSlot_ == PlayerSlot::Host &&
        localMissionCinematicState_.has_value() &&
        (localMissionCinematicState_->phase ==
             MissionCinematicPhase::Playing ||
         localMissionCinematicState_->phase ==
             MissionCinematicPhase::Loading)) {
        // Consent is scoped to the current cinematic generation.
        // Requiring both players to hit the key inside the same five-second window made a valid vote look like a broken skip on ordinary Hamachi latency and when one player was reading subtitles.
        localCutsceneSkipVoteUntilMs_ = 1U;
        facade_.Log(
            "[MISSION_SKIP][VOTE] host vote registered for this cutscene; waiting for the guest vote");
        TryCommitCutsceneSkip(nowMs);
        return;
    }
    if (localSlot_ == PlayerSlot::Guest &&
        remoteMissionCinematicState_.has_value() &&
        (remoteMissionCinematicState_->phase ==
             MissionCinematicPhase::Playing ||
         remoteMissionCinematicState_->phase ==
             MissionCinematicPhase::Loading)) {
        SendMissionCinematicAction(
            MissionCinematicActionKind::SkipRequest,
            0U,
            nowMs);
        facade_.Log(
            "[MISSION_SKIP][VOTE] guest vote registered for this cutscene; waiting for the host vote");
    }
}

void BridgeRuntime::TryCommitCutsceneSkip(
    const std::uint64_t nowMs) {
    if (localSlot_ != PlayerSlot::Host ||
        !localMissionCinematicState_.has_value() ||
        (localMissionCinematicState_->phase !=
             MissionCinematicPhase::Playing &&
         localMissionCinematicState_->phase !=
             MissionCinematicPhase::Loading)) {
        return;
    }
    const bool localVote =
        localCutsceneSkipVoteUntilMs_ != 0U;
    const bool guestConnected = remoteReplicaId_.IsValid();
    const bool guestVote =
        !guestConnected ||
        remoteCutsceneSkipVoteUntilMs_ != 0U;
    if (!localVote || !guestVote) {
        return;
    }
    localCutsceneSkipUntilMs_ = std::max(
        localCutsceneSkipUntilMs_,
        nowMs + kMissionSkipInjectionMilliseconds);
    localCutsceneSkipVoteUntilMs_ = 0U;
    remoteCutsceneSkipVoteUntilMs_ = 0U;
    facade_.Log(
        guestConnected
            ? "[MISSION_SKIP][CONSENSUS] both players voted; feeding INPUT_SKIP_CUTSCENE for at most 2500ms"
            : "[MISSION_SKIP][CONSENSUS] no guest is present; host skip accepted");
}

// Drives the local cinematic presentation state and handles safe skip/resume timing while preserving the host as the final decision maker.
void BridgeRuntime::TickMissionCinematic(
    const std::optional<LocalPlayerSample>& sample,
    const PlayerSlot localSlot,
    const std::uint64_t nowMs) {
    if (facade_.IsCutsceneSkipPressed()) {
        RequestCutsceneSkip(nowMs);
    }

    if (localSlot == PlayerSlot::Host) {
        TryCommitCutsceneSkip(nowMs);
        const bool injectingSkip =
            localCutsceneSkipUntilMs_ != 0U &&
            nowMs <= localCutsceneSkipUntilMs_ &&
            localMissionCinematicState_.has_value() &&
            (localMissionCinematicState_->phase ==
                 MissionCinematicPhase::Playing ||
             localMissionCinematicState_->phase ==
                 MissionCinematicPhase::Loading);
        facade_.MaintainCutsceneSkipInput(injectingSkip);
        if (!injectingSkip) {
            localCutsceneSkipUntilMs_ = 0U;
        }
        if (!localEntityId_.IsValid() ||
            !localMissionState_.has_value()) {
            facade_.MaintainMissionResumeBarrier(false);
            return;
        }

        // The frontend uses a cutscene camera while loading a normal Story save.
        // Only the mission authority classifier may promote that raw signal into the replicated cinematic FSM.
        const bool missionPresentationActive =
            localMissionState_->phase == MissionPhase::Cutscene ||
            localMissionState_->phase == MissionPhase::Loading;
        const bool cutsceneActive =
            missionPresentationActive && sample.has_value() &&
            sample->cutsceneActive;

        // PrepareResume is a one-way commit.
        // RDR2 briefly reports its cinematic camera as active again while restoring the HUD and player task graph.
        // Treating that rebound as a new Playing phase produced the V26.1 0.8s loop which repeatedly hid/spawned NPCs, toggled the minimap and dropped the guest back to follow-camera.
        if (localMissionCinematicState_.has_value() &&
            localMissionCinematicState_->phase ==
                MissionCinematicPhase::PrepareResume) {
            const bool noGuest =
                !remoteReplicaId_.IsValid() &&
                !awaitingRestoredGuestStream_;
            const bool waitingLong =
                localCinematicPrepareStartedMs_ != 0U &&
                nowMs - localCinematicPrepareStartedMs_ >=
                    kMissionResumeLongWaitWarningMilliseconds;
            if (waitingLong && !localCinematicResumeWaitWarned_) {
                localCinematicResumeWaitWarned_ = true;
                facade_.Log(
                    "[WARNING][MISSION_RESUME_BARRIER] guest has not confirmed ResumeReady for 30s; host remains safely held instead of forcing a contaminated resume");
            }
            if (localCinematicResumeReady_ || noGuest) {
                awaitingRestoredGuestStream_ = false;
                const auto anchor =
                    localMissionCinematicState_->resumeAnchor;
                const auto heading =
                    localMissionCinematicState_->resumeHeading;
                PublishMissionCinematicState(
                    MissionCinematicPhase::Completed,
                    static_cast<std::uint16_t>(
                        MissionCinematicStateFlag::AnchorValid),
                    anchor,
                    heading,
                    nowMs);
                facade_.MaintainCutsceneSkipInput(false);
                facade_.MaintainMissionResumeBarrier(false);
                localCutsceneSkipUntilMs_ = 0U;
                localCutsceneSkipVoteUntilMs_ = 0U;
                remoteCutsceneSkipVoteUntilMs_ = 0U;
                localCinematicTerminalLatchActive_ = true;
                localCinematicTerminalClearSinceMs_ = 0U;
                facade_.Log(
                    "[MISSION_CINEMATIC][FSM] resume committed; transient camera rebounds are now suppressed");
                return;
            }
            facade_.MaintainMissionResumeBarrier(true);
            PublishMissionCinematicState(
                MissionCinematicPhase::PrepareResume,
                localMissionCinematicState_->flags,
                localMissionCinematicState_->resumeAnchor,
                localMissionCinematicState_->resumeHeading,
                nowMs);
            return;
        }

        if (localCinematicTerminalLatchActive_) {
            facade_.MaintainMissionResumeBarrier(false);
            if (cutsceneActive) {
                localCinematicTerminalClearSinceMs_ = 0U;
                return;
            }
            if (localCinematicTerminalClearSinceMs_ == 0U ||
                nowMs < localCinematicTerminalClearSinceMs_) {
                localCinematicTerminalClearSinceMs_ = nowMs;
                return;
            }
            if (nowMs - localCinematicTerminalClearSinceMs_ <
                kMissionCinematicTerminalClearMilliseconds) {
                return;
            }
            localCinematicTerminalLatchActive_ = false;
            localCinematicTerminalClearSinceMs_ = 0U;
            localMissionCinematicState_.reset();
            facade_.Log(
                "[MISSION_CINEMATIC][FSM] terminal latch cleared after 1500ms of stable gameplay");
            return;
        }

        const bool cinematicActive =
            localMissionCinematicState_.has_value() &&
            IsCinematicPresentationPhase(
                localMissionCinematicState_->phase);
        if (cutsceneActive) {
            facade_.MaintainMissionResumeBarrier(false);
            if (!cinematicActive) {
                localMissionCinematicGeneration_ = AdvanceNonZero(
                    localMissionCinematicGeneration_);
                ResetAnimSceneHybridState(true);
                localMissionCinematicRevision_ = 1U;
                localMissionCameraRevision_ = 1U;
                remoteMissionCinematicActionId_ = 0U;
                remoteMissionCinematicActionSequences_.Reset();
                localCinematicPresentationReady_ = false;
                localCinematicCameraReady_ = false;
                localCinematicCameraWaitStartedMs_ = nowMs;
                localCinematicCameraWaitWarned_ = false;
                localCinematicResumeReady_ = false;
                localCinematicControlRecoveredSinceMs_ = 0U;
                localCinematicPrepareStartedMs_ = 0U;
                localCinematicResumeWaitWarned_ = false;
                localCinematicTerminalClearSinceMs_ = 0U;
                localCinematicTerminalLatchActive_ = false;
                localCutsceneSkipVoteUntilMs_ = 0U;
                remoteCutsceneSkipVoteUntilMs_ = 0U;
                localMissionCinematicState_.reset();
            } else {
                localCinematicControlRecoveredSinceMs_ = 0U;
                localCinematicPrepareStartedMs_ = 0U;
                localCinematicResumeWaitWarned_ = false;
                localCinematicResumeReady_ = false;
            }
            auto flags = static_cast<std::uint16_t>(
                MissionCinematicStateFlag::CameraExpected);
            if (injectingSkip) {
                flags |= static_cast<std::uint16_t>(
                    MissionCinematicStateFlag::SkipPending);
            }
            const bool cameraWaitTimedOut =
                localCinematicCameraWaitStartedMs_ != 0U &&
                nowMs >= localCinematicCameraWaitStartedMs_ &&
                nowMs - localCinematicCameraWaitStartedMs_ >=
                    kMissionInitialCameraWaitMilliseconds;
            if (remoteReplicaId_.IsValid() &&
                !localCinematicCameraReady_ && cameraWaitTimedOut &&
                !localCinematicCameraWaitWarned_) {
                localCinematicCameraWaitWarned_ = true;
                facade_.Log(
                    "[WARNING][MISSION_CAMERA][HANDSHAKE] no valid host camera sample after 1500ms; allowing the reversible follow-camera fallback");
            }
            const bool waitingForGuestPresentation =
                remoteReplicaId_.IsValid() &&
                (!localCinematicPresentationReady_ ||
                 (!localCinematicCameraReady_ && !cameraWaitTimedOut));
            PublishMissionCinematicState(
                waitingForGuestPresentation
                    ? MissionCinematicPhase::Loading
                    : MissionCinematicPhase::Playing,
                flags,
                {},
                0.0F,
                nowMs);
            return;
        }

        if (!sample.has_value()) {
            if (cinematicActive &&
                localMissionCinematicState_->phase !=
                    MissionCinematicPhase::PrepareResume) {
                auto flags = injectingSkip
                                 ? static_cast<std::uint16_t>(
                                       MissionCinematicStateFlag::SkipPending)
                                 : std::uint16_t{0U};
                PublishMissionCinematicState(
                    localMissionState_->phase == MissionPhase::Loading
                        ? MissionCinematicPhase::Loading
                        : localMissionCinematicState_->phase,
                    flags,
                    {},
                    0.0F,
                    nowMs);
            }
            return;
        }

        if (!cinematicActive) {
            facade_.MaintainMissionResumeBarrier(false);
            return;
        }
        if (localMissionCinematicState_->phase !=
            MissionCinematicPhase::PrepareResume) {
            if (localCinematicControlRecoveredSinceMs_ == 0U ||
                nowMs < localCinematicControlRecoveredSinceMs_) {
                localCinematicControlRecoveredSinceMs_ = nowMs;
            }
            if (nowMs - localCinematicControlRecoveredSinceMs_ <
                kMissionControlRecoveryMilliseconds) {
                PublishMissionCinematicState(
                    localMissionCinematicState_->phase,
                    localMissionCinematicState_->flags,
                    localMissionCinematicState_->resumeAnchor,
                    localMissionCinematicState_->resumeHeading,
                    nowMs);
                return;
            }
            localCinematicPrepareStartedMs_ = nowMs;
            localCinematicResumeReady_ = false;
            localCinematicResumeWaitWarned_ = false;
            facade_.MaintainMissionResumeBarrier(true);
            PublishMissionCinematicState(
                MissionCinematicPhase::PrepareResume,
                static_cast<std::uint16_t>(
                    MissionCinematicStateFlag::AnchorValid),
                sample->position,
                sample->heading,
                nowMs);
            return;
        }

        // The next tick owns the one-way PrepareResume branch above.
        // Keeping this path free of completion logic makes the transition explicit and prevents raw cutscene detection from pre-empting it.
        return;
    }

    const bool presentationWasActive =
        remoteMissionCinematicState_.has_value() &&
        IsCinematicPresentationPhase(
            remoteMissionCinematicState_->phase);
    if (presentationWasActive) {
        if (guestPostCinematicSkipUntilMs_ == 0U) {
            facade_.Log(
                "[MISSION_ISOLATION][POST_PRESENTATION] targeted authored-scene quarantine remains armed after the host presentation; no blind global skip is injected");
        }
        guestPostCinematicSkipUntilMs_ =
            nowMs + kGuestPostCinematicSkipGraceMilliseconds;
    } else if (guestPostCinematicSkipUntilMs_ != 0U &&
               nowMs > guestPostCinematicSkipUntilMs_) {
        guestPostCinematicSkipUntilMs_ = 0U;
    }
    // Do not continuously inject the global cutscene-skip control for the whole host mission.
    // The V31.3 traces proved that it did not prevent the delayed guest Story VM and could itself advance a queued private scene toward Mission Failed.
    // The full lease still blocks Story interactions; skip is now used only after positive quarantine detection (below) or in the short terminal grace.
    // Captured game-owned scenes are independently fast-forwarded by the facade, while bridge-owned exact scenes are exempt.
    const bool preemptiveStorySkipActive = false;
    const bool exactGuestSceneInFlight =
        remoteAnimSceneDefinition_.has_value() &&
        (remoteAnimSceneDefinitionPrepared_ ||
         remoteAnimSceneDefinitionCommitted_ ||
         !remoteAnimSceneDefinitionResponseSent_);

    bool quarantineSkipActive = false;
    if (!guestMissionQuarantineActive_) {
        guestQuarantineSkipUntilMs_ = 0U;
        guestMissionQuarantineSkipLatched_ = false;
    } else if (!guestMissionQuarantineSkipLatched_) {
        guestMissionQuarantineSkipLatched_ = true;
        guestQuarantineSkipUntilMs_ = nowMs;
        quarantineSkipActive = true;
        facade_.Log(
            "[MISSION_SKIP][QUARANTINE] guest-local Story transition is suppressed until the authored presentation is stably clear");
    } else if (guestMissionQuarantineActive_) {
        quarantineSkipActive = true;
    } else {
        guestQuarantineSkipUntilMs_ = 0U;
    }

    if (!remoteMissionCinematicState_.has_value()) {
        // A guest-local Story scene can surface long after the host has completed its presentation.
        // The host-mission lease and quarantine therefore remain authoritative even while no cinematic state is currently cached.
        facade_.MaintainCutsceneSkipInput(
            !exactGuestSceneInFlight &&
            (preemptiveStorySkipActive || quarantineSkipActive));
        return;
    }
    const bool presentationActive = IsCinematicPresentationPhase(
        remoteMissionCinematicState_->phase);
    const bool hostSkipCommitted =
        presentationActive &&
        (remoteMissionCinematicState_->flags &
         static_cast<std::uint16_t>(
             MissionCinematicStateFlag::SkipPending)) != 0U;
    // The guest's own Story VM may create its local scene at any later point in the mission.
    // Keep the vanilla skip context asserted for the complete host mission, except while the exact bridge-owned scene is in flight.
    // The synchronized vote remains a separate protocol action and still decides whether the host scene skips.
    facade_.MaintainCutsceneSkipInput(
        hostSkipCommitted ||
        (!exactGuestSceneInFlight &&
         (preemptiveStorySkipActive || quarantineSkipActive)));
    const bool hostLost =
        presentationActive &&
        (!remoteMissionCinematicReceivedAtMs_.has_value() ||
         ElapsedMilliseconds(
             *remoteMissionCinematicReceivedAtMs_,
             nowMs) > kMissionHostLossTimeoutMilliseconds);
    if (hostLost) {
        facade_.MaintainCutsceneSkipInput(
            !exactGuestSceneInFlight &&
            (preemptiveStorySkipActive || quarantineSkipActive));
        facade_.Log(
            "[WARNING][MISSION_CINEMATIC] host state missing for 3s; idempotent fallback teardown");
        facade_.MaintainReplicatedMissionCamera(false, std::nullopt);
        facade_.MaintainMissionSpectator(
            guestMissionQuarantineActive_ || soloOverride_);
        remoteMissionCameraState_.reset();
        remoteMissionCameraReceivedAtMs_.reset();
        auto aborted = *remoteMissionCinematicState_;
        aborted.phase = MissionCinematicPhase::Aborted;
        aborted.flags &= static_cast<std::uint16_t>(
            MissionCinematicStateFlag::AnchorValid);
        remoteMissionCinematicState_ = aborted;
        ResetAnimSceneHybridState(true);
        cutsceneSpectator_ = false;
        return;
    }
    if (!presentationActive) {
        facade_.MaintainReplicatedMissionCamera(false, std::nullopt);
        facade_.MaintainMissionSpectator(
            guestMissionQuarantineActive_ || soloOverride_);
        return;
    }

    facade_.MaintainMissionSpectator(true);
    if (!remoteCinematicPresentationReadySent_) {
        SendMissionCinematicAction(
            MissionCinematicActionKind::PresentationReady,
            0U,
            nowMs);
        remoteCinematicPresentationReadySent_ = true;
    }
    if (remoteMissionCinematicState_->phase ==
            MissionCinematicPhase::PrepareResume &&
        !remoteCinematicResumeReadySent_) {
        const auto preparation = facade_.PrepareMissionCinematicResume(
            remoteMissionCinematicState_->resumeAnchor,
            remoteMissionCinematicState_->resumeHeading,
            nowMs);
        if (preparation.ready) {
            remoteCinematicResumeFallbackUsed_ =
                preparation.fallbackUsed;
            SendMissionCinematicAction(
                MissionCinematicActionKind::ResumeReady,
                preparation.fallbackUsed
                    ? static_cast<std::uint16_t>(
                          MissionCinematicActionFlag::FallbackUsed)
                    : 0U,
                nowMs);
            remoteCinematicResumeReadySent_ = true;
        }
    }
}

void BridgeRuntime::HandleRemoteMissionCinematicState(
    const Frame& frame,
    const MissionCinematicStatePayload& state) {
    if (localSlot_ != PlayerSlot::Guest) {
        facade_.Log(
            "[WARNING][MISSION_CINEMATIC][RX] rejected host state on non-guest endpoint");
        return;
    }
    auto& expectedHostId = playerEntityIds_[SlotIndex(PlayerSlot::Host)];
    if (expectedHostId.IsValid() && state.hostEntityId != expectedHostId) {
        facade_.Log(
            "[WARNING][MISSION_CINEMATIC][RX] mismatched host identity");
        return;
    }
    if (!expectedHostId.IsValid()) {
        expectedHostId = state.hostEntityId;
    }
    const auto disposition =
        remoteMissionCinematicSequences_.Observe(frame.header.sequence);
    if (disposition == SequenceDisposition::Duplicate ||
        disposition == SequenceDisposition::Stale) {
        return;
    }

    bool newGeneration = !remoteMissionCinematicState_.has_value();
    if (remoteMissionCinematicState_.has_value()) {
        const auto& current = *remoteMissionCinematicState_;
        const bool stale =
            state.missionEpoch < current.missionEpoch ||
            (state.missionEpoch == current.missionEpoch &&
             (state.cinematicGeneration < current.cinematicGeneration ||
              (state.cinematicGeneration == current.cinematicGeneration &&
               state.revision < current.revision)));
        if (stale) {
            return;
        }
        if (state.missionEpoch == current.missionEpoch &&
            state.cinematicGeneration == current.cinematicGeneration &&
            state.revision == current.revision && state != current) {
            facade_.Log(
                "[WARNING][MISSION_CINEMATIC][RX] equal revision changed payload");
            return;
        }
        newGeneration =
            state.missionEpoch != current.missionEpoch ||
            state.cinematicGeneration != current.cinematicGeneration;
    }
    if (remoteMissionState_.has_value() &&
        state.missionEpoch != remoteMissionState_->missionEpoch) {
        facade_.Log(
            "[WARNING][MISSION_CINEMATIC][RX] mission epoch mismatch");
        return;
    }
    if (newGeneration) {
        ResetAnimSceneHybridState(true);
        remoteMissionCameraSequences_.Reset();
        remoteMissionCameraState_.reset();
        remoteMissionCameraReceivedAtMs_.reset();
        remoteAnimSceneSequences_.Reset();
        remoteAnimSceneState_.reset();
        remoteAnimSceneReceivedAtMs_.reset();
        if (remoteNativeAnimSceneActive_) {
            (void)facade_.MaintainReplicatedAnimScene(
                false,
                std::nullopt);
            remoteNativeAnimSceneActive_ = false;
        }
        remoteCinematicPresentationReadySent_ = false;
        remoteCinematicResumeReadySent_ = false;
        remoteCinematicResumeFallbackUsed_ = false;
        // This cursor is scoped to the authenticated co-op session.
        // A guest pipe reconnect and its ResyncRequest replay the current cinematic as a locally new generation, while the uninterrupted host retains the last accepted action id.
        // Keeping the cursor monotonic makes the next Skip/PresentationReady/ResumeReady unambiguously newer.
    }
    remoteMissionCinematicState_ = state;
    remoteMissionCinematicReceivedAtMs_ = previousTickMs_;
    if (state.phase == MissionCinematicPhase::Completed ||
        state.phase == MissionCinematicPhase::Aborted) {
        remoteMissionCameraState_.reset();
        remoteMissionCameraReceivedAtMs_.reset();
        remoteAnimSceneState_.reset();
        remoteAnimSceneReceivedAtMs_.reset();
        (void)facade_.MaintainReplicatedAnimScene(
            false,
            std::nullopt);
        remoteNativeAnimSceneActive_ = false;
        ResetAnimSceneHybridState(true);
    }
    facade_.Log(
        "[MISSION_CINEMATIC][RX] generation=" +
        std::to_string(state.cinematicGeneration) +
        ", revision=" + std::to_string(state.revision) +
        ", phase=" +
        std::to_string(static_cast<std::uint8_t>(state.phase)));
}

void BridgeRuntime::HandleRemoteMissionCinematicAction(
    const Frame& frame,
    const MissionCinematicActionPayload& action) {
    if (localSlot_ != PlayerSlot::Host ||
        !localMissionCinematicState_.has_value() ||
        action.senderSlot != static_cast<std::uint8_t>(PlayerSlot::Guest) ||
        action.hostEntityId != localEntityId_ ||
        action.missionEpoch != localMissionCinematicState_->missionEpoch ||
        action.cinematicGeneration !=
            localMissionCinematicState_->cinematicGeneration) {
        return;
    }
    const auto disposition =
        remoteMissionCinematicActionSequences_.Observe(frame.header.sequence);
    if (disposition == SequenceDisposition::Duplicate ||
        disposition == SequenceDisposition::Stale) {
        return;
    }
    if (remoteMissionCinematicActionId_ != 0U &&
        static_cast<std::int32_t>(
            action.actionId - remoteMissionCinematicActionId_) <= 0) {
        return;
    }
    remoteMissionCinematicActionId_ = action.actionId;
    switch (action.kind) {
        case MissionCinematicActionKind::PresentationReady:
            localCinematicPresentationReady_ = true;
            break;
        case MissionCinematicActionKind::ResumeReady:
            if (localMissionCinematicState_->phase ==
                MissionCinematicPhase::PrepareResume) {
                localCinematicResumeReady_ = true;
            }
            break;
        case MissionCinematicActionKind::SkipRequest:
            if (localMissionCinematicState_->phase ==
                    MissionCinematicPhase::Playing ||
                localMissionCinematicState_->phase ==
                    MissionCinematicPhase::Loading) {
                remoteCutsceneSkipVoteUntilMs_ = 1U;
                facade_.Log(
                    "[MISSION_SKIP][VOTE] guest vote registered for this cutscene; waiting for the host vote");
                TryCommitCutsceneSkip(previousTickMs_);
            }
            break;
    }
}

bool BridgeRuntime::SendAnimSceneControl(
    const AnimSceneControlKind kind,
    const AnimSceneControlReason reason,
    const std::uint32_t flags,
    const std::uint64_t playAtHostTick,
    const float startPhase,
    const float rate,
    const std::uint64_t nowMs) {
    if (!localSlot_.has_value() || !transport_.IsConnected()) {
        return false;
    }
    const auto* definition =
        *localSlot_ == PlayerSlot::Host
            ? (localAnimSceneDefinition_.has_value()
                   ? &*localAnimSceneDefinition_
                   : nullptr)
            : (remoteAnimSceneDefinition_.has_value()
                   ? &*remoteAnimSceneDefinition_
                   : nullptr);
    if (definition == nullptr) {
        return false;
    }

    localAnimSceneControlActionId_ =
        AdvanceNonZero(localAnimSceneControlActionId_);
    const AnimSceneControlPayload control{
        definition->hostEntityId,
        definition->missionEpoch,
        definition->cinematicGeneration,
        definition->definitionRevision,
        localAnimSceneControlActionId_,
        definition->fingerprintLow,
        definition->fingerprintHigh,
        playAtHostTick,
        startPhase,
        rate,
        kind,
        static_cast<std::uint8_t>(*localSlot_),
        reason,
        flags};
    Frame frame;
    frame.header.type = MessageType::AnimSceneControl;
    frame.header.sequence = sequencer_.Next();
    frame.header.tick = nowMs;
    frame.payload = EncodeAnimSceneControl(control);
    return SendBestEffort(std::move(frame));
}

void BridgeRuntime::TryCommitHostAnimSceneDefinition(
    const std::uint64_t nowMs) {
    if (localSlot_ != PlayerSlot::Host ||
        !localAnimSceneDefinition_.has_value() ||
        !localAnimSceneGuestReady_ ||
        localAnimSceneCommitSent_ ||
        localAnimSceneDefinitionTimedOut_) {
        return;
    }
    if (!localMissionCinematicState_.has_value() ||
        !IsCinematicPresentationPhase(
            localMissionCinematicState_->phase) ||
        localMissionCinematicState_->missionEpoch !=
            localAnimSceneDefinition_->missionEpoch ||
        localMissionCinematicState_->cinematicGeneration !=
            localAnimSceneDefinition_->cinematicGeneration) {
        return;
    }
    if (localAnimSceneGuestReadyAtMs_ != 0U &&
        nowMs >= localAnimSceneGuestReadyAtMs_ &&
        nowMs - localAnimSceneGuestReadyAtMs_ >=
            kAnimSceneHybridHostDecisionTimeoutMilliseconds) {
        localAnimSceneDefinitionTimedOut_ = true;
        (void)SetHostAnimSceneStartBarrier(
            false,
            "host-decision-timeout");
        (void)SendAnimSceneControl(
            AnimSceneControlKind::HostAbort,
            AnimSceneControlReason::LoadTimeout,
            static_cast<std::uint32_t>(
                AnimSceneControlFlag::FallbackUsed),
            0U,
            0.0F,
            0.0F,
            nowMs);
        facade_.Log(
            "[ANIMSCENE_HYBRID][ABORT][TX] guest was ready, but the host scene did not reach a runnable state before the decision deadline; SAFE_FALLBACK retained");
        return;
    }
    constexpr auto kRunning = static_cast<std::uint32_t>(
        AnimSceneReplicaStateFlag::Running);
    if (!lastLocalAnimSceneState_.has_value() ||
        lastLocalAnimSceneState_->missionEpoch !=
            localAnimSceneDefinition_->missionEpoch ||
        lastLocalAnimSceneState_->cinematicGeneration !=
            localAnimSceneDefinition_->cinematicGeneration ||
        lastLocalAnimSceneState_->dictionaryHash !=
            localAnimSceneDefinition_->dictionaryHash ||
        (lastLocalAnimSceneState_->flags & kRunning) == 0U ||
        !std::isfinite(lastLocalAnimSceneState_->rate) ||
        lastLocalAnimSceneState_->rate <= 0.0F) {
        return;
    }

    const auto phase = std::clamp(
        lastLocalAnimSceneState_->phase,
        0.0F,
        1.05F);
    const auto rate = std::clamp(
        lastLocalAnimSceneState_->rate,
        0.01F,
        4.0F);
    const auto lateJoinFlag = phase > 0.02F
                                  ? static_cast<std::uint32_t>(
                                        AnimSceneControlFlag::LateJoin)
                                  : 0U;
    if (!SendAnimSceneControl(
            AnimSceneControlKind::HostPlayCommit,
            AnimSceneControlReason::None,
            lateJoinFlag,
            std::max<std::uint64_t>(1U, nowMs),
            phase,
            rate,
            nowMs)) {
        return;
    }
    localAnimSceneCommitSent_ = true;
    (void)SetHostAnimSceneStartBarrier(false, "guest-ready-commit");
    facade_.Log(
        "[ANIMSCENE_HYBRID][COMMIT][TX] guest ready; non-invasive host marker released and the guest exact scene continues from sampled phase=" +
        std::to_string(phase));
}

// Host captures a portable description of an animation scene and waits for the guest to prepare it before committing the shared cutscene presentation.
void BridgeRuntime::TickAnimSceneHybridDefinition(
    const PlayerSlot localSlot,
    const std::optional<LocalPlayerSample>& localSample,
    const std::uint64_t nowMs) {
    try {
        if (localSlot == PlayerSlot::Guest) {
            if (remoteAnimSceneDefinition_.has_value() &&
                (!remoteMissionCinematicState_.has_value() ||
                 !IsCinematicPresentationPhase(
                     remoteMissionCinematicState_->phase) ||
                 remoteMissionCinematicState_->missionEpoch !=
                     remoteAnimSceneDefinition_->missionEpoch ||
                 remoteMissionCinematicState_->cinematicGeneration !=
                     remoteAnimSceneDefinition_->cinematicGeneration)) {
                ResetAnimSceneHybridState(true);
                return;
            }
            if (remoteAnimSceneDefinition_.has_value() &&
                remoteAnimSceneDefinitionPrepared_ &&
                !remoteAnimSceneDefinitionCommitted_ &&
                remoteAnimSceneReadySentAtMs_ != 0U &&
                nowMs >= remoteAnimSceneReadySentAtMs_ &&
                nowMs - remoteAnimSceneReadySentAtMs_ >=
                    kAnimSceneHybridGuestDecisionWindowMilliseconds) {
                facade_.AbortReplicatedAnimSceneDefinition();
                remoteAnimSceneDefinitionPrepared_ = false;
                remoteAnimSceneReadySentAtMs_ = 0U;
                (void)SendAnimSceneControl(
                    AnimSceneControlKind::GuestRejected,
                    AnimSceneControlReason::LoadTimeout,
                    0U,
                    0U,
                    0.0F,
                    0.0F,
                    nowMs);
                facade_.Log(
                    "[ANIMSCENE_HYBRID][DECISION_TIMEOUT] no matching host Commit/Abort arrived after GuestReady; prepared handle was released and SAFE_FALLBACK retained");
                return;
            }
            PollRemoteAnimSceneDefinitionPreparation(nowMs);
            return;
        }
        if (
            !localEntityId_.IsValid() ||
            !localMissionCinematicState_.has_value() ||
            !IsCinematicPresentationPhase(
                localMissionCinematicState_->phase)) {
            // Drain nothing outside an authoritative presentation.
            // A future capture hook retains incomplete records until CREATE/SET_ENTITY has produced one complete definition.
            return;
        }

        const auto captures =
            facade_.DrainCapturedAnimSceneDefinitions();
        for (const auto& capture : captures) {
            if (!capture.complete || capture.captureSequence == 0U ||
                capture.captureSequence <= lastCapturedAnimSceneSequence_) {
                continue;
            }
            lastCapturedAnimSceneSequence_ = capture.captureSequence;
            const auto sampledSceneHandle =
                facade_.SampledHostAnimSceneLocalHandle();
            const bool durationMatches =
                lastLocalAnimSceneState_.has_value() &&
                std::isfinite(capture.durationSeconds) &&
                std::abs(
                    capture.durationSeconds -
                    lastLocalAnimSceneState_->durationSeconds) <=
                    std::max(
                        0.05F,
                        lastLocalAnimSceneState_->durationSeconds * 0.01F);
            if (!lastLocalAnimSceneState_.has_value() ||
                capture.localSceneHandle == 0 ||
                !sampledSceneHandle.has_value() ||
                capture.localSceneHandle != *sampledSceneHandle ||
                capture.dictionaryHash == 0U ||
                capture.dictionaryHash !=
                    lastLocalAnimSceneState_->dictionaryHash ||
                !durationMatches ||
                capture.roles.size() >
                    kMaximumAnimSceneDefinitionRoles) {
                facade_.Log(
                    "[ANIMSCENE_HYBRID][CAPTURE_REJECTED] local scene handle, dictionary, duration or role limit did not match the active camera-owning AnimScene; SAFE_FALLBACK retained");
                continue;
            }

            std::vector<AnimSceneRoleBindingPayload> roles;
            roles.reserve(capture.roles.size());
            std::vector<NetEntityId> mappedEntityIds;
            mappedEntityIds.reserve(capture.roles.size());
            std::size_t mappedRoles{};
            std::size_t optionalUnboundRoles{};
            std::size_t unresolvedPlayerRoles{};
            std::size_t unresolvedRequiredRoles{};
            std::size_t duplicateMappings{};
            std::string omittedRoleNames;
            for (const auto& capturedRole : capture.roles) {
                NetEntityId entityId{};
                if (localSample.has_value() &&
                    capturedRole.localHandle != 0 &&
                    capturedRole.localHandle ==
                        localSample->localHandle) {
                    entityId = localEntityId_;
                } else if (
                    localSample.has_value() &&
                    localSample->mount.has_value() &&
                    capturedRole.localHandle != 0 &&
                    capturedRole.localHandle ==
                        localSample->mount->localHandle &&
                    lastLocalMountState_.has_value()) {
                    entityId = lastLocalMountState_->mountEntityId;
                } else if (const auto replica =
                               facade_.FindKnownReplicaNetworkId(
                                   capturedRole.localHandle);
                           replica.has_value()) {
                    entityId = *replica;
                } else if (worldMirrorHost_.has_value()) {
                    entityId = worldMirrorHost_
                                   ->FindNetwork(
                                       capturedRole.localHandle)
                                   .value_or(NetEntityId{});
                }

                constexpr auto kRequired =
                    static_cast<std::uint16_t>(
                        AnimSceneRoleFlag::Required);
                constexpr auto kPlayer =
                    static_cast<std::uint16_t>(
                        AnimSceneRoleFlag::Player);
                auto roleFlags = capturedRole.flags;
                bool duplicateAlias{};
                if (entityId.IsValid() &&
                    std::find(
                        mappedEntityIds.begin(),
                        mappedEntityIds.end(),
                        entityId) != mappedEntityIds.end()) {
                    // The protocol deliberately permits one AnimScene role per replicated entity.
                    // A repeated SET_ENTITY alias is kept in the definition as an optional unbound role instead of invalidating the complete scene.
                    entityId = NetEntityId{};
                    duplicateAlias = true;
                    ++duplicateMappings;
                }
                if (entityId.IsValid()) {
                    roleFlags |= kRequired;
                    mappedEntityIds.push_back(entityId);
                    ++mappedRoles;
                } else {
                    roleFlags &= static_cast<std::uint16_t>(~kRequired);
                    ++optionalUnboundRoles;
                    if (!omittedRoleNames.empty()) {
                        omittedRoleNames += ',';
                    }
                    if (omittedRoleNames.size() < 256U) {
                        omittedRoleNames += capturedRole.roleName;
                    }
                }
                if (!entityId.IsValid() &&
                    (roleFlags & kPlayer) != 0U) {
                    ++unresolvedPlayerRoles;
                }
                const bool optionalSceneLocalObject =
                    capturedRole.kind == AnimSceneRoleKind::Object ||
                    capturedRole.kind == AnimSceneRoleKind::Pickup;
                if (!entityId.IsValid() && !duplicateAlias &&
                    !optionalSceneLocalObject &&
                    (capturedRole.flags & kRequired) != 0U) {
                    ++unresolvedRequiredRoles;
                }
                roles.push_back(AnimSceneRoleBindingPayload{
                    capturedRole.roleName,
                    entityId,
                    entityId.IsValid()
                         ? capturedRole.modelHash
                         : 0U,
                    capturedRole.kind,
                    roleFlags,
                    entityId.IsValid()
                        ? capturedRole.bindingFlags
                        : 0U});
            }
            facade_.Log(
                "[ANIMSCENE_HYBRID][CAPTURE_MAP] mapped=" +
                std::to_string(mappedRoles) +
                ", optional-unbound=" +
                std::to_string(optionalUnboundRoles) +
                ", duplicate-aliases=" +
                std::to_string(duplicateMappings) +
                ", unresolved-players=" +
                std::to_string(unresolvedPlayerRoles) +
                ", unresolved-required=" +
                std::to_string(unresolvedRequiredRoles) +
                (omittedRoleNames.empty()
                     ? std::string{}
                     : ", omitted=" + omittedRoleNames));
            if (unresolvedPlayerRoles != 0U ||
                unresolvedRequiredRoles != 0U) {
                facade_.Log(
                    "[ANIMSCENE_HYBRID][CAPTURE_REJECTED] unresolved-player-roles=" +
                    std::to_string(unresolvedPlayerRoles) +
                    ", unresolved-required-roles=" +
                    std::to_string(unresolvedRequiredRoles) +
                    "; an incomplete captured scene is not started, SAFE_FALLBACK retained");
                continue;
            }
            std::sort(
                roles.begin(),
                roles.end(),
                [](const auto& lhs, const auto& rhs) {
                    return lhs.roleName < rhs.roleName;
                });

            if (localAnimSceneDefinition_.has_value() &&
                localAnimSceneDefinitionSentAtMs_ != 0U &&
                !localAnimSceneDefinitionTimedOut_) {
                (void)SendAnimSceneControl(
                    AnimSceneControlKind::HostAbort,
                    AnimSceneControlReason::Superseded,
                    static_cast<std::uint32_t>(
                        AnimSceneControlFlag::FallbackUsed),
                    0U,
                    0.0F,
                    0.0F,
                    nowMs);
            }

            AnimSceneDefinitionPayload definition{
                localEntityId_,
                localMissionCinematicState_->missionEpoch,
                localMissionCinematicState_->cinematicGeneration,
                localAnimSceneDefinitionRevision_,
                capture.dictionaryHash,
                0U,
                0U,
                capture.durationSeconds,
                capture.sceneFlags,
                capture.createOptionFlags,
                capture.resourceName,
                capture.playbackList,
                std::move(roles)};
            const auto fingerprint =
                ComputeAnimSceneDefinitionFingerprint(definition);
            definition.fingerprintLow = fingerprint.low;
            definition.fingerprintHigh = fingerprint.high;

            localAnimSceneDefinition_ = std::move(definition);
            localAnimSceneDefinitionRevision_ =
                AdvanceNonZero(localAnimSceneDefinitionRevision_);
            localAnimSceneDefinitionSentAtMs_ = 0U;
            localAnimSceneGuestReadyAtMs_ = 0U;
            localAnimSceneGuestReady_ = false;
            localAnimSceneCommitSent_ = false;
            localAnimSceneDefinitionTimedOut_ = false;
            remoteAnimSceneControlActionId_ = 0U;
        }

        if (localAnimSceneDefinition_.has_value() &&
            !localAnimSceneGuestReady_ &&
            !localAnimSceneDefinitionTimedOut_ &&
            localAnimSceneDefinitionSentAtMs_ == 0U &&
            !forceHostWorldMirrorReplay_ &&
            remoteReplicaId_.IsValid() &&
            transport_.IsConnected()) {
            if (!SetHostAnimSceneStartBarrier(
                    true,
                    "definition-ready-for-guest")) {
                localAnimSceneDefinitionTimedOut_ = true;
                return;
            }
            Frame frame;
            frame.header.type = MessageType::AnimSceneDefinition;
            frame.header.sequence = sequencer_.Next();
            frame.header.tick = nowMs;
            frame.payload = EncodeAnimSceneDefinition(
                *localAnimSceneDefinition_);
            if (SendBestEffort(std::move(frame))) {
                localAnimSceneDefinitionSentAtMs_ = nowMs;
                facade_.Log(
                    "[ANIMSCENE_HYBRID][PREPARE][TX] revision=" +
                    std::to_string(
                        localAnimSceneDefinition_->definitionRevision) +
                    ", roles=" +
                    std::to_string(
                        localAnimSceneDefinition_->roles.size()) +
                    "; guest local prepare is capped at 8000ms and the host accepts its reply for 10000ms including transport grace; the game-owned host AnimScene continues untouched");
            }
        }

        if (localAnimSceneDefinition_.has_value() &&
            !localAnimSceneGuestReady_ &&
            !localAnimSceneDefinitionTimedOut_ &&
            localAnimSceneDefinitionSentAtMs_ != 0U &&
            nowMs >= localAnimSceneDefinitionSentAtMs_ &&
            nowMs - localAnimSceneDefinitionSentAtMs_ >=
                kAnimSceneHybridHostReplyWindowMilliseconds) {
            localAnimSceneDefinitionTimedOut_ = true;
            (void)SetHostAnimSceneStartBarrier(
                false,
                "guest-prepare-timeout");
            (void)SendAnimSceneControl(
                AnimSceneControlKind::HostAbort,
                AnimSceneControlReason::LoadTimeout,
                static_cast<std::uint32_t>(
                    AnimSceneControlFlag::FallbackUsed),
                0U,
                0.0F,
                0.0F,
                nowMs);
            facade_.Log(
                "[ANIMSCENE_HYBRID][ABORT][TX] guest prepare timeout; host continued normally and SAFE_FALLBACK stays active");
        }
        TryCommitHostAnimSceneDefinition(nowMs);
    } catch (...) {
        localAnimSceneDefinitionTimedOut_ = true;
        (void)SetHostAnimSceneStartBarrier(false, "capture-error");
        facade_.Log(
            "[ANIMSCENE_HYBRID][ERROR] capture/definition preparation failed safely; no native scene was created");
    }
}

// Guest receives a host animation-scene recipe.
// It validates resources and role bindings before making any local RDR2 scene replica.
void BridgeRuntime::HandleRemoteAnimSceneDefinition(
    const Frame& frame,
    const AnimSceneDefinitionPayload& definition) {
    if (localSlot_ != PlayerSlot::Guest) {
        facade_.Log(
            "[WARNING][ANIMSCENE_HYBRID][PREPARE][RX] definition rejected on non-guest endpoint");
        return;
    }
    const auto expectedHostId =
        playerEntityIds_[SlotIndex(PlayerSlot::Host)];
    if (!expectedHostId.IsValid() ||
        definition.hostEntityId != expectedHostId ||
        !remoteMissionCinematicState_.has_value() ||
        !IsCinematicPresentationPhase(
            remoteMissionCinematicState_->phase) ||
        definition.missionEpoch !=
            remoteMissionCinematicState_->missionEpoch ||
        definition.cinematicGeneration !=
            remoteMissionCinematicState_->cinematicGeneration) {
        facade_.Log(
            "[WARNING][ANIMSCENE_HYBRID][PREPARE][RX] host identity or cinematic generation mismatch");
        return;
    }
    const auto disposition =
        remoteAnimSceneDefinitionSequences_.Observe(
            frame.header.sequence);
    if (disposition == SequenceDisposition::Duplicate ||
        disposition == SequenceDisposition::Stale) {
        return;
    }
    if (remoteAnimSceneDefinition_.has_value() &&
        definition.definitionRevision <=
            remoteAnimSceneDefinition_->definitionRevision) {
        if (definition.definitionRevision ==
                remoteAnimSceneDefinition_->definitionRevision &&
            definition != *remoteAnimSceneDefinition_) {
            facade_.Log(
                "[WARNING][ANIMSCENE_HYBRID][PREPARE][RX] equal revision changed canonical definition");
            return;
        }
        if (definition == *remoteAnimSceneDefinition_) {
            if (remoteAnimSceneDefinitionPrepared_ ||
                remoteAnimSceneDefinitionCommitted_) {
                const auto flags =
                    static_cast<std::uint32_t>(
                        AnimSceneControlFlag::ResourceLoaded) |
                    static_cast<std::uint32_t>(
                        AnimSceneControlFlag::RequiredRolesBound) |
                    static_cast<std::uint32_t>(
                        AnimSceneControlFlag::CacheHit);
                if (SendAnimSceneControl(
                        AnimSceneControlKind::GuestReady,
                        AnimSceneControlReason::None,
                        flags,
                        0U,
                        0.0F,
                        0.0F,
                        previousTickMs_)) {
                    remoteAnimSceneDefinitionResponseSent_ = true;
                    remoteAnimSceneReadySentAtMs_ =
                        remoteAnimSceneDefinitionCommitted_
                            ? 0U
                            : previousTickMs_;
                    facade_.Log(
                        "[ANIMSCENE_HYBRID][READY][REPLAY] refreshed the idempotent readiness decision for a replayed definition");
                }
            } else {
                remoteAnimSceneDefinitionResponseSent_ = false;
                remoteAnimSceneDefinitionReceivedAtMs_ = previousTickMs_;
                PollRemoteAnimSceneDefinitionPreparation(previousTickMs_);
            }
        }
        return;
    }

    facade_.AbortReplicatedAnimSceneDefinition();
    remoteAnimSceneDefinition_ = definition;
    remoteAnimSceneDefinitionPrepared_ = false;
    remoteAnimSceneDefinitionCommitted_ = false;
    remoteAnimSceneDefinitionResponseSent_ = false;
    remoteAnimSceneDefinitionReceivedAtMs_ = previousTickMs_;
    remoteAnimSceneReadySentAtMs_ = 0U;
    nextRemoteAnimScenePrepareDiagnosticsMs_ = 0U;
    remoteAnimScenePrepareStage_ =
        ReplicatedAnimScenePrepareStage::None;
    remoteAnimScenePrepareResolvedRoles_ = 0U;
    remoteAnimScenePrepareRequiredRoles_ = 0U;
    remoteAnimScenePreparePendingEntityId_ = {};
    remoteAnimScenePreparePendingRoleName_.clear();
    remoteAnimSceneControlActionId_ = 0U;
    PollRemoteAnimSceneDefinitionPreparation(previousTickMs_);
}

void BridgeRuntime::PollRemoteAnimSceneDefinitionPreparation(
    const std::uint64_t nowMs) {
    if (localSlot_ != PlayerSlot::Guest ||
        !remoteAnimSceneDefinition_.has_value() ||
        remoteAnimSceneDefinitionResponseSent_ ||
        remoteAnimSceneDefinitionCommitted_) {
        return;
    }
    if (!remoteMissionCinematicState_.has_value() ||
        !IsCinematicPresentationPhase(
            remoteMissionCinematicState_->phase) ||
        remoteMissionCinematicState_->missionEpoch !=
            remoteAnimSceneDefinition_->missionEpoch ||
        remoteMissionCinematicState_->cinematicGeneration !=
            remoteAnimSceneDefinition_->cinematicGeneration) {
        ResetAnimSceneHybridState(true);
        return;
    }
    const auto preparation =
        facade_.PrepareReplicatedAnimSceneDefinition(
            *remoteAnimSceneDefinition_,
            localEntityId_);
    const bool preparationChanged =
        preparation.stage != remoteAnimScenePrepareStage_ ||
        preparation.resolvedRoles !=
            remoteAnimScenePrepareResolvedRoles_ ||
        preparation.requiredRoles !=
            remoteAnimScenePrepareRequiredRoles_ ||
        preparation.pendingEntityId !=
            remoteAnimScenePreparePendingEntityId_ ||
        preparation.pendingRoleName !=
            remoteAnimScenePreparePendingRoleName_;
    if (preparationChanged ||
        nextRemoteAnimScenePrepareDiagnosticsMs_ == 0U ||
        nowMs >= nextRemoteAnimScenePrepareDiagnosticsMs_) {
        facade_.Log(
            "[ANIMSCENE_HYBRID][PREPARE_PROGRESS] stage=" +
            std::string{AnimScenePrepareStageName(preparation.stage)} +
            ", resolved=" +
            std::to_string(preparation.resolvedRoles) + "/" +
            std::to_string(preparation.requiredRoles) +
            ", resource-loaded=" +
            std::to_string(preparation.resourceLoaded ? 1 : 0) +
            (preparation.pendingRoleName.empty()
                 ? std::string{}
                 : ", pending-role=" +
                       preparation.pendingRoleName) +
            (!preparation.pendingEntityId.IsValid()
                 ? std::string{}
                 : ", pending-entity=" +
                       std::to_string(
                           preparation.pendingEntityId.Value())));
        nextRemoteAnimScenePrepareDiagnosticsMs_ = nowMs + 1'000U;
    }
    remoteAnimScenePrepareStage_ = preparation.stage;
    remoteAnimScenePrepareResolvedRoles_ =
        preparation.resolvedRoles;
    remoteAnimScenePrepareRequiredRoles_ =
        preparation.requiredRoles;
    remoteAnimScenePreparePendingEntityId_ =
        preparation.pendingEntityId;
    remoteAnimScenePreparePendingRoleName_ =
        preparation.pendingRoleName;
    remoteAnimSceneDefinitionPrepared_ =
        preparation.status == ReplicatedAnimScenePrepareStatus::Ready;
    if (remoteAnimSceneDefinitionPrepared_) {
        std::uint32_t flags =
            static_cast<std::uint32_t>(
                AnimSceneControlFlag::ResourceLoaded) |
            static_cast<std::uint32_t>(
                AnimSceneControlFlag::RequiredRolesBound);
        if (preparation.cacheHit) {
            flags |= static_cast<std::uint32_t>(
                AnimSceneControlFlag::CacheHit);
        }
        if (SendAnimSceneControl(
                AnimSceneControlKind::GuestReady,
                AnimSceneControlReason::None,
                flags,
                0U,
                0.0F,
                0.0F,
                nowMs)) {
            remoteAnimSceneDefinitionResponseSent_ = true;
            remoteAnimSceneReadySentAtMs_ = nowMs;
            facade_.Log(
                "[ANIMSCENE_HYBRID][READY][TX] revision=" +
                std::to_string(
                    remoteAnimSceneDefinition_->definitionRevision) +
                ", resolved=" +
                std::to_string(preparation.resolvedRoles) + "/" +
                std::to_string(preparation.requiredRoles));
            if (pendingRemoteAnimSceneCommit_.has_value()) {
                const auto pending = *pendingRemoteAnimSceneCommit_;
                CommitPreparedGuestAnimSceneDefinition(pending);
            }
        }
        return;
    }

    if (preparation.status ==
            ReplicatedAnimScenePrepareStatus::Pending &&
        remoteAnimSceneDefinitionReceivedAtMs_ != 0U &&
        nowMs >= remoteAnimSceneDefinitionReceivedAtMs_ &&
        nowMs - remoteAnimSceneDefinitionReceivedAtMs_ <
            kAnimSceneHybridGuestPrepareTimeoutMilliseconds) {
        return;
    }

    const auto reason = [&preparation]() {
        switch (preparation.status) {
            case ReplicatedAnimScenePrepareStatus::Pending:
                return AnimSceneControlReason::LoadTimeout;
            case ReplicatedAnimScenePrepareStatus::Unsupported:
            case ReplicatedAnimScenePrepareStatus::ResourceUnavailable:
                return AnimSceneControlReason::UnsupportedResource;
            case ReplicatedAnimScenePrepareStatus::MissingBinding:
                return AnimSceneControlReason::MissingBinding;
            case ReplicatedAnimScenePrepareStatus::EntityMismatch:
                return AnimSceneControlReason::EntityMismatch;
            case ReplicatedAnimScenePrepareStatus::NativeFailure:
                return AnimSceneControlReason::NativeFailure;
            case ReplicatedAnimScenePrepareStatus::Ready:
                return AnimSceneControlReason::None;
        }
        return AnimSceneControlReason::NativeFailure;
    }();
    if (SendAnimSceneControl(
            AnimSceneControlKind::GuestRejected,
            reason,
            0U,
            0U,
            0.0F,
            0.0F,
            nowMs)) {
        remoteAnimSceneDefinitionResponseSent_ = true;
        facade_.Log(
            "[ANIMSCENE_HYBRID][REJECTED][TX] reason=" +
            std::to_string(static_cast<std::uint8_t>(reason)) +
            ", final-stage=" +
            std::string{AnimScenePrepareStageName(preparation.stage)} +
            ", resolved=" +
            std::to_string(preparation.resolvedRoles) + "/" +
            std::to_string(preparation.requiredRoles) +
            "; camera/proxy SAFE_FALLBACK retained");
    }
}

void BridgeRuntime::CommitPreparedGuestAnimSceneDefinition(
    const AnimSceneControlPayload& control) {
    if (localSlot_ != PlayerSlot::Guest ||
        !remoteAnimSceneDefinition_.has_value() ||
        !remoteAnimSceneDefinitionPrepared_ ||
        remoteAnimSceneDefinitionCommitted_ ||
        !remoteMissionCinematicState_.has_value() ||
        !IsCinematicPresentationPhase(
            remoteMissionCinematicState_->phase) ||
        control.hostEntityId !=
            remoteAnimSceneDefinition_->hostEntityId ||
        control.missionEpoch !=
            remoteAnimSceneDefinition_->missionEpoch ||
        control.cinematicGeneration !=
            remoteAnimSceneDefinition_->cinematicGeneration ||
        control.definitionRevision !=
            remoteAnimSceneDefinition_->definitionRevision ||
        control.fingerprintLow !=
            remoteAnimSceneDefinition_->fingerprintLow ||
        control.fingerprintHigh !=
            remoteAnimSceneDefinition_->fingerprintHigh) {
        return;
    }
    remoteAnimSceneDefinitionCommitted_ =
        facade_.CommitReplicatedAnimSceneDefinition(control);
    pendingRemoteAnimSceneCommit_.reset();
    remoteAnimSceneReadySentAtMs_ = 0U;
    if (remoteAnimSceneDefinitionCommitted_) {
        facade_.Log(
            "[ANIMSCENE_HYBRID][COMMIT][RX] exact bridge-owned scene started immediately from the sampled host phase; phase stream remains authoritative");
    } else {
        facade_.AbortReplicatedAnimSceneDefinition();
        remoteAnimSceneDefinitionPrepared_ = false;
        facade_.Log(
            "[ANIMSCENE_HYBRID][COMMIT][FAILED] native start rejected; SAFE_FALLBACK retained");
    }
}

// Handles ready/rejected/commit/abort answers for one exact animation-scene generation.
// Late answers cannot change a newer cinematic.
void BridgeRuntime::HandleRemoteAnimSceneControl(
    const Frame& frame,
    const AnimSceneControlPayload& control) {
    if (!localSlot_.has_value()) {
        return;
    }
    const bool receivingGuestReply =
        *localSlot_ == PlayerSlot::Host &&
        (control.kind == AnimSceneControlKind::GuestReady ||
         control.kind == AnimSceneControlKind::GuestRejected) &&
        control.senderSlot ==
            static_cast<std::uint8_t>(PlayerSlot::Guest);
    const bool receivingHostDecision =
        *localSlot_ == PlayerSlot::Guest &&
        (control.kind == AnimSceneControlKind::HostPlayCommit ||
         control.kind == AnimSceneControlKind::HostAbort) &&
        control.senderSlot ==
            static_cast<std::uint8_t>(PlayerSlot::Host);
    if (!receivingGuestReply && !receivingHostDecision) {
        facade_.Log(
            "[WARNING][ANIMSCENE_HYBRID][CONTROL][RX] rejected direction/sender");
        return;
    }
    if (receivingGuestReply) {
        const auto expectedGuestId =
            playerEntityIds_[SlotIndex(PlayerSlot::Guest)];
        const bool authenticatedGuestStream =
            remoteReplicaId_.IsValid() && expectedGuestId.IsValid() &&
            remoteReplicaId_ == expectedGuestId &&
            lastRemoteStateMs_.has_value() &&
            ElapsedMilliseconds(
                *lastRemoteStateMs_,
                previousTickMs_) <=
                kRemotePlayerDespawnTimeoutMilliseconds;
        if (!authenticatedGuestStream) {
            facade_.Log(
                "[WARNING][ANIMSCENE_HYBRID][CONTROL][RX] rejected guest reply without an active authenticated guest stream");
            return;
        }
        if (forceHostWorldMirrorReplay_ ||
            localAnimSceneDefinitionSentAtMs_ == 0U) {
            facade_.Log(
                "[WARNING][ANIMSCENE_HYBRID][CONTROL][RX] rejected guest reply before stable world spawns and the current definition revision were sent");
            return;
        }
    }
    const auto disposition =
        remoteAnimSceneControlSequences_.Observe(frame.header.sequence);
    if (disposition == SequenceDisposition::Duplicate ||
        disposition == SequenceDisposition::Stale) {
        return;
    }
    const auto* definition = receivingGuestReply
                                 ? (localAnimSceneDefinition_.has_value()
                                        ? &*localAnimSceneDefinition_
                                        : nullptr)
                                 : (remoteAnimSceneDefinition_.has_value()
                                        ? &*remoteAnimSceneDefinition_
                                        : nullptr);
    if (definition == nullptr ||
        control.hostEntityId != definition->hostEntityId ||
        control.missionEpoch != definition->missionEpoch ||
        control.cinematicGeneration !=
            definition->cinematicGeneration ||
        control.definitionRevision !=
            definition->definitionRevision ||
        control.fingerprintLow != definition->fingerprintLow ||
        control.fingerprintHigh != definition->fingerprintHigh) {
        facade_.Log(
            "[WARNING][ANIMSCENE_HYBRID][CONTROL][RX] rejected stale definition key/fingerprint");
        return;
    }
    const auto* cinematic = receivingGuestReply
                                ? (localMissionCinematicState_.has_value()
                                       ? &*localMissionCinematicState_
                                       : nullptr)
                                : (remoteMissionCinematicState_.has_value()
                                       ? &*remoteMissionCinematicState_
                                       : nullptr);
    if (cinematic == nullptr ||
        !IsCinematicPresentationPhase(cinematic->phase) ||
        cinematic->missionEpoch != definition->missionEpoch ||
        cinematic->cinematicGeneration !=
            definition->cinematicGeneration) {
        facade_.Log(
            "[WARNING][ANIMSCENE_HYBRID][CONTROL][RX] rejected outside the active cinematic presentation");
        return;
    }
    if (remoteAnimSceneControlActionId_ != 0U &&
        static_cast<std::int32_t>(
            control.actionId - remoteAnimSceneControlActionId_) <= 0) {
        return;
    }
    remoteAnimSceneControlActionId_ = control.actionId;

    if (receivingGuestReply) {
        if (control.kind == AnimSceneControlKind::GuestRejected) {
            localAnimSceneDefinitionTimedOut_ = true;
            (void)SetHostAnimSceneStartBarrier(
                false,
                "guest-rejected");
            localAnimSceneGuestReady_ = false;
            localAnimSceneGuestReadyAtMs_ = 0U;
            (void)SendAnimSceneControl(
                AnimSceneControlKind::HostAbort,
                control.reason,
                static_cast<std::uint32_t>(
                    AnimSceneControlFlag::FallbackUsed),
                0U,
                0.0F,
                0.0F,
                previousTickMs_);
            facade_.Log(
                "[ANIMSCENE_HYBRID][REJECTED][RX] guest could not prepare exact scene; fallback committed");
            return;
        }
        if (localAnimSceneDefinitionTimedOut_) {
            (void)SendAnimSceneControl(
                AnimSceneControlKind::HostAbort,
                AnimSceneControlReason::LoadTimeout,
                static_cast<std::uint32_t>(
                    AnimSceneControlFlag::FallbackUsed),
                0U,
                0.0F,
                0.0F,
                previousTickMs_);
            return;
        }
        localAnimSceneGuestReady_ = true;
        localAnimSceneCommitSent_ = false;
        localAnimSceneGuestReadyAtMs_ = previousTickMs_;
        TryCommitHostAnimSceneDefinition(previousTickMs_);
        return;
    }

    if (control.kind == AnimSceneControlKind::HostAbort) {
        facade_.Log(
            "[ANIMSCENE_HYBRID][ABORT][RX] host selected SAFE_FALLBACK");
        ResetAnimSceneHybridState(true);
        return;
    }
    if (remoteAnimSceneDefinitionCommitted_) {
        return;
    }
    if (!remoteAnimSceneDefinitionPrepared_) {
        pendingRemoteAnimSceneCommit_ = control;
        facade_.Log(
            "[ANIMSCENE_HYBRID][COMMIT][BUFFERED] matching host decision arrived before local prepare completed");
        return;
    }
    CommitPreparedGuestAnimSceneDefinition(control);
}

// Sends/applies the portable camera view used during mission transitions when a full local cinematic scene cannot be safely reproduced on the guest.
void BridgeRuntime::TickMissionCamera(
    const PlayerSlot localSlot,
    const std::uint64_t nowMs) {
    if (localSlot != PlayerSlot::Host ||
        !localEntityId_.IsValid()) {
        localMissionCameraActive_ = false;
        return;
    }

    const bool cutsceneActive =
        localMissionCinematicState_.has_value() &&
        (localMissionCinematicState_->phase ==
             MissionCinematicPhase::Loading ||
         localMissionCinematicState_->phase ==
             MissionCinematicPhase::Playing) &&
        localCinematicControlRecoveredSinceMs_ == 0U &&
        localMissionState_.has_value() &&
        (localMissionState_->phase == MissionPhase::Cutscene ||
         localMissionState_->phase == MissionPhase::Loading);
    if (cutsceneActive &&
        transport_.IsConnected() &&
        nowMs >= nextAnimSceneSampleMs_) {
        nextAnimSceneSampleMs_ =
            nowMs + kAnimSceneSampleMilliseconds;
        auto scene = facade_.SampleHostAnimScene(
            localEntityId_,
            localMissionCinematicState_->missionEpoch,
            localMissionCinematicState_->cinematicGeneration,
            localAnimSceneRevision_);
        if (scene.has_value()) {
            if (localAnimSceneDefinition_.has_value() &&
                localAnimSceneDefinition_->missionEpoch ==
                    scene->missionEpoch &&
                localAnimSceneDefinition_->cinematicGeneration ==
                    scene->cinematicGeneration &&
                localAnimSceneDefinition_->dictionaryHash ==
                    scene->dictionaryHash) {
                scene->definitionRevision =
                    localAnimSceneDefinition_->definitionRevision;
            }
            lastLocalAnimSceneState_ = *scene;
            Frame frame;
            frame.header.type = MessageType::AnimSceneReplicaState;
            frame.header.sequence = sequencer_.Next();
            frame.header.tick = nowMs;
            frame.payload = EncodeAnimSceneReplicaState(*scene);
            SendBestEffort(std::move(frame));
            localAnimSceneRevision_ =
                AdvanceNonZero(localAnimSceneRevision_);
            if (!localAnimSceneActive_) {
                facade_.Log(
                    "[ANIMSCENE_REPLICA][TX] active host AnimScene discovered; deep native presentation stream started");
            }
            localAnimSceneActive_ = true;
        } else if (localAnimSceneActive_) {
            localAnimSceneActive_ = false;
            facade_.Log(
                "[ANIMSCENE_REPLICA][TX] active scene no longer discoverable; camera keyframes remain active");
        }
    }
    if (cutsceneActive &&
        transport_.IsConnected() &&
        nowMs >= nextMissionCameraSampleMs_) {
        nextMissionCameraSampleMs_ =
            nowMs + kMissionCameraSampleMilliseconds;
        const auto camera = facade_.SampleMissionCamera();
        if (!camera.has_value()) {
            return;
        }
        localMissionCameraRevision_ =
            AdvanceNonZero(localMissionCameraRevision_);
        const MissionCameraStatePayload state{
            localEntityId_,
            localMissionCinematicState_->missionEpoch,
            localMissionCinematicState_->cinematicGeneration,
            localMissionCameraRevision_,
            static_cast<std::uint32_t>(
                MissionCameraStateFlag::Active) |
                camera->flags,
            camera->position,
            camera->rotation,
            camera->fieldOfView};
        Frame frame;
        frame.header.type = MessageType::MissionCameraState;
        frame.header.sequence = sequencer_.Next();
        frame.header.tick = nowMs;
        frame.payload = EncodeMissionCameraState(state);
        SendBestEffort(std::move(frame));
        localCinematicCameraReady_ = true;
        if (!localMissionCameraActive_) {
            facade_.Log(
                "[MISSION_CAMERA][TX] first camera keyframe sent during Loading; host cutscene stream running at 30 Hz");
        }
        localMissionCameraActive_ = true;
        return;
    }

    if (!cutsceneActive && localMissionCameraActive_) {
        localMissionCameraActive_ = false;
        localMissionCameraRevision_ =
            AdvanceNonZero(localMissionCameraRevision_);
        if (transport_.IsConnected() &&
            localMissionCinematicState_.has_value()) {
            const MissionCameraStatePayload inactive{
                localEntityId_,
                localMissionCinematicState_->missionEpoch,
                localMissionCinematicState_->cinematicGeneration,
                localMissionCameraRevision_,
                0U,
                {},
                {},
                0.0F};
            Frame frame;
            frame.header.type = MessageType::MissionCameraState;
            frame.header.sequence = sequencer_.Next();
            frame.header.tick = nowMs;
            frame.payload = EncodeMissionCameraState(inactive);
            SendBestEffort(std::move(frame));
        }
        facade_.Log(
            "[MISSION_CAMERA][TX] host cutscene camera stream stopped");
    }
    if (!cutsceneActive) {
        localAnimSceneActive_ = false;
        lastLocalAnimSceneState_.reset();
    }
}

void BridgeRuntime::HandleRemoteMissionCamera(
    const Frame& frame,
    const MissionCameraStatePayload& state) {
    if (localSlot_ != PlayerSlot::Guest) {
        facade_.Log(
            "[WARNING][MISSION_CAMERA][RX] rejected camera state on a non-guest endpoint");
        return;
    }
    const auto expectedHostId =
        playerEntityIds_[SlotIndex(PlayerSlot::Host)];
    if (!expectedHostId.IsValid() ||
        state.hostEntityId != expectedHostId) {
        facade_.Log(
            "[WARNING][MISSION_CAMERA][RX] rejected camera state from a mismatched host identity");
        return;
    }
    if (remoteMissionState_.has_value() &&
        state.missionEpoch !=
            remoteMissionState_->missionEpoch) {
        facade_.Log(
            "[WARNING][MISSION_CAMERA][RX] rejected camera state from a stale mission epoch");
        return;
    }
    if (!remoteMissionCinematicState_.has_value() ||
        state.missionEpoch !=
            remoteMissionCinematicState_->missionEpoch ||
        state.cinematicGeneration !=
            remoteMissionCinematicState_->cinematicGeneration) {
        facade_.Log(
            "[WARNING][MISSION_CAMERA][RX] rejected camera state from a stale cinematic generation");
        return;
    }
    const auto disposition =
        remoteMissionCameraSequences_.Observe(
            frame.header.sequence);
    if (disposition == SequenceDisposition::Duplicate ||
        disposition == SequenceDisposition::Stale) {
        return;
    }
    if (remoteMissionCameraState_.has_value() &&
        state.missionEpoch ==
            remoteMissionCameraState_->missionEpoch &&
        state.cinematicGeneration ==
            remoteMissionCameraState_->cinematicGeneration &&
        state.revision <=
            remoteMissionCameraState_->revision) {
        return;
    }

    constexpr auto kActive =
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::Active);
    const bool wasActive =
        remoteMissionCameraState_.has_value() &&
        (remoteMissionCameraState_->flags & kActive) != 0U;
    const bool active = (state.flags & kActive) != 0U;
    remoteMissionCameraState_ = state;
    remoteMissionCameraReceivedAtMs_ = previousTickMs_;
    if (wasActive != active) {
        facade_.Log(
            active
                ? "[MISSION_CAMERA][RX] first fresh host camera snapshot received"
                : "[MISSION_CAMERA][RX] host camera stream sent an explicit stop");
    }
}

void BridgeRuntime::HandleRemoteAnimSceneReplicaState(
    const Frame& frame,
    const AnimSceneReplicaStatePayload& state) {
    if (localSlot_ != PlayerSlot::Guest) {
        facade_.Log(
            "[WARNING][ANIMSCENE_REPLICA][RX] rejected host scene on a non-guest endpoint");
        return;
    }
    const auto expectedHostId =
        playerEntityIds_[SlotIndex(PlayerSlot::Host)];
    if (!expectedHostId.IsValid() ||
        state.hostEntityId != expectedHostId) {
        facade_.Log(
            "[WARNING][ANIMSCENE_REPLICA][RX] mismatched host identity");
        return;
    }
    if (!remoteMissionCinematicState_.has_value() ||
        state.missionEpoch !=
            remoteMissionCinematicState_->missionEpoch ||
        state.cinematicGeneration !=
            remoteMissionCinematicState_->cinematicGeneration) {
        facade_.Log(
            "[WARNING][ANIMSCENE_REPLICA][RX] stale mission epoch or cinematic generation");
        return;
    }
    if (state.definitionRevision != 0U &&
        (!remoteAnimSceneDefinition_.has_value() ||
         remoteAnimSceneDefinition_->missionEpoch != state.missionEpoch ||
         remoteAnimSceneDefinition_->cinematicGeneration !=
             state.cinematicGeneration ||
         remoteAnimSceneDefinition_->definitionRevision !=
             state.definitionRevision)) {
        facade_.Log(
            "[WARNING][ANIMSCENE_HYBRID][STATE] definition revision is not prepared on this guest; ignored stale/early phase snapshot");
        return;
    }
    const auto disposition =
        remoteAnimSceneSequences_.Observe(frame.header.sequence);
    if (disposition == SequenceDisposition::Duplicate ||
        disposition == SequenceDisposition::Stale) {
        return;
    }
    if (remoteAnimSceneState_.has_value() &&
        state.missionEpoch == remoteAnimSceneState_->missionEpoch &&
        state.cinematicGeneration ==
            remoteAnimSceneState_->cinematicGeneration &&
        state.revision <= remoteAnimSceneState_->revision) {
        return;
    }

    const bool firstScene =
        !remoteAnimSceneState_.has_value() ||
        remoteAnimSceneState_->dictionaryHash != state.dictionaryHash;
    remoteAnimSceneState_ = state;
    remoteAnimSceneReceivedAtMs_ = previousTickMs_;
    if (firstScene) {
        facade_.Log(
            "[ANIMSCENE_REPLICA][RX] host scene signature received; searching for a safe matching local scene");
    }
}

// Final per-tick mission presentation decision: spectator, camera, input lock, and replica visibility are chosen from the current host-owned state.
void BridgeRuntime::MaintainMissionPresentation(
    const std::optional<LocalPlayerSample>& localSample,
    const bool remoteStreaming,
    const std::uint64_t nowMs) {
    if (localSlot_ != PlayerSlot::Guest) {
        facade_.MaintainReplicatedMissionCamera(
            false,
            std::nullopt);
        (void)facade_.MaintainReplicatedAnimScene(
            false,
            std::nullopt);
        remoteNativeAnimSceneActive_ = false;
        facade_.MaintainMissionCompanionPresentation({});
        return;
    }

    const bool spectatorActive =
        cutsceneSpectator_ || soloOverride_;
    std::optional<MissionCameraStatePayload> camera;
    constexpr auto kCameraActive =
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::Active);
    if (spectatorActive &&
        remoteMissionCinematicState_.has_value() &&
        (remoteMissionCinematicState_->phase ==
             MissionCinematicPhase::Loading ||
         remoteMissionCinematicState_->phase ==
             MissionCinematicPhase::Playing) &&
        remoteMissionCameraState_.has_value() &&
        remoteMissionCameraReceivedAtMs_.has_value() &&
        remoteMissionCameraState_->missionEpoch ==
            remoteMissionCinematicState_->missionEpoch &&
        remoteMissionCameraState_->cinematicGeneration ==
            remoteMissionCinematicState_->cinematicGeneration &&
        (remoteMissionCameraState_->flags & kCameraActive) != 0U &&
        ElapsedMilliseconds(
            *remoteMissionCameraReceivedAtMs_,
            nowMs) <= kMissionCameraFreshnessMilliseconds) {
        camera = remoteMissionCameraState_;
    }

    std::optional<AnimSceneReplicaStatePayload> animScene;
    constexpr auto kAnimSceneActive =
        static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::Active);
    if (spectatorActive &&
        remoteMissionCinematicState_.has_value() &&
        (remoteMissionCinematicState_->phase ==
             MissionCinematicPhase::Loading ||
         remoteMissionCinematicState_->phase ==
             MissionCinematicPhase::Playing) &&
        remoteAnimSceneState_.has_value() &&
        remoteAnimSceneReceivedAtMs_.has_value() &&
        remoteAnimSceneState_->missionEpoch ==
            remoteMissionCinematicState_->missionEpoch &&
        remoteAnimSceneState_->cinematicGeneration ==
            remoteMissionCinematicState_->cinematicGeneration &&
        (remoteAnimSceneState_->flags & kAnimSceneActive) != 0U &&
        ElapsedMilliseconds(
            *remoteAnimSceneReceivedAtMs_,
            nowMs) <= kAnimSceneFreshnessMilliseconds) {
        animScene = remoteAnimSceneState_;
    }
    const bool nativeAnimSceneActive =
        facade_.MaintainReplicatedAnimScene(
            spectatorActive,
            animScene);
    if (nativeAnimSceneActive != remoteNativeAnimSceneActive_) {
        facade_.Log(
            nativeAnimSceneActive
                ? "[ANIMSCENE_REPLICA] matching local scene owns presentation; camera-keyframe fallback suspended"
                : "[ANIMSCENE_REPLICA] native presentation unavailable; using camera-keyframe fallback");
        remoteNativeAnimSceneActive_ = nativeAnimSceneActive;
    }
    facade_.MaintainReplicatedMissionCamera(
        spectatorActive && !nativeAnimSceneActive,
        nativeAnimSceneActive
            ? std::nullopt
            : camera);

    constexpr auto kMissionActive =
        static_cast<std::uint8_t>(
            MissionStateFlag::MissionActive);
    constexpr auto kAnchorValid =
        static_cast<std::uint8_t>(
            MissionStateFlag::AnchorValid);
    const bool objectiveActive =
        remoteStreaming && !spectatorActive &&
        localSample.has_value() &&
        remoteMissionState_.has_value() &&
        remoteMissionState_->phase == MissionPhase::Active &&
        (remoteMissionState_->flags & kMissionActive) != 0U &&
        (remoteMissionState_->flags & kAnchorValid) != 0U;
    if (!objectiveActive) {
        facade_.MaintainMissionCompanionPresentation({});
        return;
    }

    Vec3 target = remoteMissionState_->hostAnchor;
    bool liveHostPosition{};
    if (latestRemoteState_.has_value() &&
        lastRemoteStateMs_.has_value() &&
        latestRemoteState_->entityId ==
            remoteMissionState_->hostEntityId &&
        ElapsedMilliseconds(*lastRemoteStateMs_, nowMs) <=
            kRemotePlayerFreshnessMilliseconds &&
        IsFinite(latestRemoteState_->position)) {
        target = latestRemoteState_->position;
        liveHostPosition = true;
    }
    const auto distance =
        Distance(localSample->position, target);
    if (!IsFinite(target) || !std::isfinite(distance)) {
        facade_.MaintainMissionCompanionPresentation({});
        return;
    }
    facade_.MaintainMissionCompanionPresentation(
        MissionCompanionPresentation{
            true,
            liveHostPosition,
            target,
            distance,
            remoteMissionObjective_.has_value() &&
                    remoteMissionObjective_->missionEpoch ==
                        remoteMissionState_->missionEpoch
                ? remoteMissionObjective_->text
                : std::string{}});
}

// Writes compact counters/state markers used to diagnose stalls, missing world updates, reconnects, and other multiplayer problems without changing state.
void BridgeRuntime::EmitRuntimeDiagnostics(
    const bool remoteStreaming,
    const std::uint64_t nowMs) {
    const MissionStatePayload* mission{};
    if (localSlot_ == PlayerSlot::Host &&
        localMissionState_.has_value()) {
        mission = &*localMissionState_;
    } else if (localSlot_ == PlayerSlot::Guest &&
               remoteMissionState_.has_value()) {
        mission = &*remoteMissionState_;
    }
    const auto missionEpoch =
        mission != nullptr ? mission->missionEpoch : 0U;
    const auto missionRevision =
        mission != nullptr ? mission->revision : 0U;
    const auto missionPhase =
        mission != nullptr ? mission->phase : MissionPhase::Idle;
    const bool missionGateOpen = IsMissionWorldMirrorSafe();
    const bool spectator = cutsceneSpectator_ || soloOverride_;
    const auto timeline =
        "role=" + std::string{RoleName(localSlot_)} +
        ", epoch=" + std::to_string(missionEpoch) +
        ", revision=" + std::to_string(missionRevision) +
        ", phase=" +
        std::string{MissionPhaseName(missionPhase)} +
        ", gate=" + (missionGateOpen ? "open" : "closed") +
        ", quarantine=" +
        std::to_string(guestMissionQuarantineActive_ ? 1 : 0) +
        ", spectator=" + std::to_string(spectator ? 1 : 0);
    if (timeline != lastMissionTimelineState_) {
        lastMissionTimelineState_ = timeline;
        facade_.Log("[MISSION_TIMELINE] " + timeline);
    }

    if (nowMs < nextRuntimeDiagnosticsMs_) {
        return;
    }
    nextRuntimeDiagnosticsMs_ =
        nowMs + kRuntimeDiagnosticsIntervalMilliseconds;

    const auto divergence =
        facade_.SampleRuntimeDivergenceDiagnostics();
    WorldMirrorGraphStats graph;
    if (localSlot_ == PlayerSlot::Host &&
        worldMirrorHost_.has_value()) {
        graph = worldMirrorHost_->Stats();
    } else {
        graph = guestWorldGraph_.Stats();
    }

    constexpr auto kCameraActive =
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::Active);
    const auto remoteAge = DiagnosticAge(lastRemoteStateMs_, nowMs);
    const auto actionAge = DiagnosticAge(
        lastRemotePlayerActionReceivedAtMs_,
        nowMs);
    const auto cameraAge = DiagnosticAge(
        remoteMissionCameraReceivedAtMs_,
        nowMs);
    const auto animSceneAge = DiagnosticAge(
        remoteAnimSceneReceivedAtMs_,
        nowMs);
    const bool cameraActive =
        localSlot_ == PlayerSlot::Host
            ? localMissionCameraActive_
            : remoteMissionCameraState_.has_value() &&
                  (remoteMissionCameraState_->flags &
                   kCameraActive) != 0U &&
                  cameraAge <=
                      kMissionCameraFreshnessMilliseconds;
    const auto averageTickMilliseconds =
        runtimeDiagnosticTickCount_ == 0U
            ? 0.0
            : static_cast<double>(
                  runtimeDiagnosticTickElapsedSumMs_) /
                  static_cast<double>(
                      runtimeDiagnosticTickCount_);

    facade_.Log(
        "[SESSION_HEALTH] role=" +
        std::string{RoleName(localSlot_)} +
        ", tick-count=" +
        std::to_string(runtimeDiagnosticTickCount_) +
        ", average-tick-ms=" +
        std::to_string(averageTickMilliseconds) +
        ", max-tick-ms=" +
        std::to_string(runtimeDiagnosticTickMaximumMs_) +
        ", hitch-count=" +
        std::to_string(runtimeDiagnosticHitchCount_) +
        ", remote=" + (remoteStreaming ? "fresh" : "stale") +
        ", remote-age-ms=" + std::to_string(remoteAge) +
        ", snapshots=" +
        std::to_string(remoteSnapshots_.Size()) +
        ", interpolation-delay-ms=" +
        std::to_string(remoteSnapshots_.InterpolationDelayMs()) +
        ", interpolation-jitter-ms=" +
        std::to_string(remoteSnapshots_.ArrivalJitterMs()) +
        ", action-seen=" +
        std::to_string(
            lastRemotePlayerActionReceivedAtMs_.has_value() ? 1 : 0) +
        ", action-age-ms=" + std::to_string(actionAge) +
        ", action-tx=" + std::to_string(playerActionsSent_) +
        ", action-rx=" + std::to_string(playerActionsReceived_) +
        ", camera=" + (cameraActive ? "fresh" : "inactive") +
        ", camera-age-ms=" + std::to_string(cameraAge) +
        ", animscene-host-stream=" +
        std::to_string(localAnimSceneActive_ ? 1 : 0) +
        ", animscene-native-guest=" +
        std::to_string(remoteNativeAnimSceneActive_ ? 1 : 0) +
        ", animscene-age-ms=" + std::to_string(animSceneAge) +
        ", appearance-local-revision=" +
        std::to_string(localAppearanceRevision_) +
        ", appearance-remote-revision=" +
        std::to_string(
            remoteAppearance_.has_value()
                ? remoteAppearance_->revision
                : 0U) +
        ", appearance-remote-components=" +
        std::to_string(
            remoteAppearance_.has_value()
                ? remoteAppearance_->componentHashes.size()
                : 0U) +
        ", mission-epoch=" + std::to_string(missionEpoch) +
        ", mission-revision=" + std::to_string(missionRevision) +
        ", mission-phase=" +
        std::string{MissionPhaseName(missionPhase)} +
        ", graph-nodes=" + std::to_string(graph.nodeCount) +
        ", graph-active=" + std::to_string(graph.activeCount) +
        ", graph-pending=" + std::to_string(graph.pendingCount) +
        ", graph-edges=" + std::to_string(graph.edgeCount));

    const auto& player = divergence.player;
    facade_.Log(
        "[PLAYER_DIVERGENCE] available=" +
        std::to_string(player.available ? 1 : 0) +
        ", position-error-m=" +
        std::to_string(player.positionErrorMeters) +
        ", rotation-error-deg=" +
        std::to_string(player.rotationErrorDegrees) +
        ", expected-gait=" +
        std::to_string(player.expectedGait) +
        ", observed-gait=" +
        std::to_string(player.observedGait) +
        ", action-active=" +
        std::to_string(player.actionActive ? 1 : 0) +
        ", action-id=" +
        std::to_string(player.activeActionId) +
        ", action-freshness-ms=" +
        std::to_string(player.actionFreshnessMilliseconds));

    const bool gaitDiverged =
        player.expectedGait != player.observedGait;
    const bool playerDiverged =
        player.available &&
        (player.positionErrorMeters >=
             kPlayerDivergencePositionThresholdMeters ||
         player.rotationErrorDegrees >=
             kPlayerDivergenceRotationThresholdDegrees ||
         (gaitDiverged &&
          player.positionErrorMeters >= 0.75F) ||
         (player.actionActive &&
          player.actionFreshnessMilliseconds >=
              kPlayerActionFreshnessThresholdMilliseconds));
    if (!playerDivergenceStateInitialized_ ||
        playerDiverged != playerDivergenceActive_) {
        const bool wasDiverged = playerDivergenceActive_;
        playerDivergenceStateInitialized_ = true;
        playerDivergenceActive_ = playerDiverged;
        if (playerDiverged) {
            facade_.Log(
                "[PLAYER_DIVERGENCE][EVENT] state=first-divergence, position-error-m=" +
                std::to_string(player.positionErrorMeters) +
                ", rotation-error-deg=" +
                std::to_string(player.rotationErrorDegrees) +
                ", gait-mismatch=" +
                std::to_string(gaitDiverged ? 1 : 0) +
                ", action-freshness-ms=" +
                std::to_string(
                    player.actionFreshnessMilliseconds));
        } else if (wasDiverged) {
            facade_.Log(
                "[PLAYER_DIVERGENCE][EVENT] state=recovered");
        }
    }

    const auto& entities = divergence.entities;
    facade_.Log(
        "[ENTITY_DIVERGENCE] desired=" +
        std::to_string(entities.desiredCount) +
        ", desired-script-owned=" +
        std::to_string(entities.desiredScriptOwnedCount) +
        ", live=" + std::to_string(entities.liveCount) +
        ", live-script-owned=" +
        std::to_string(entities.liveScriptOwnedCount) +
        ", pending=" +
        std::to_string(entities.pendingCount) +
        ", pending-script-owned=" +
        std::to_string(entities.pendingScriptOwnedCount) +
        ", missing=" + std::to_string(entities.missingCount) +
        ", missing-script-owned=" +
        std::to_string(entities.missingScriptOwnedCount) +
        ", divergent=" +
        std::to_string(entities.divergentCount) +
        ", divergent-script-owned=" +
        std::to_string(entities.divergentScriptOwnedCount) +
        ", top-worst-entity=" +
        (entities.worstEntityId.IsValid()
             ? std::to_string(entities.worstEntityId.Value())
             : std::string{"none"}) +
        ", top-worst-error-m=" +
        std::to_string(entities.worstPositionErrorMeters) +
        ", position-error-p95-m=" +
        std::to_string(entities.positionErrorP95Meters) +
        ", oldest-missing-entity=" +
        (entities.oldestMissingEntityId.IsValid()
             ? std::to_string(
                   entities.oldestMissingEntityId.Value())
             : std::string{"none"}) +
        ", oldest-missing-age-ms=" +
        std::to_string(
            entities.oldestMissingAgeMilliseconds));
    const bool entityDiverged =
        entities.missingCount != 0U ||
        entities.divergentCount != 0U;
    if (!entityDivergenceStateInitialized_ ||
        entityDiverged != entityDivergenceActive_) {
        const bool wasDiverged = entityDivergenceActive_;
        entityDivergenceStateInitialized_ = true;
        entityDivergenceActive_ = entityDiverged;
        if (entityDiverged) {
            facade_.Log(
                "[ENTITY_DIVERGENCE][EVENT] state=first-divergence, missing=" +
                std::to_string(entities.missingCount) +
                ", missing-script-owned=" +
                std::to_string(
                    entities.missingScriptOwnedCount) +
                ", divergent=" +
                std::to_string(entities.divergentCount) +
                ", divergent-script-owned=" +
                std::to_string(
                    entities.divergentScriptOwnedCount) +
                ", top-worst-entity=" +
                (entities.worstEntityId.IsValid()
                     ? std::to_string(
                           entities.worstEntityId.Value())
                     : std::string{"none"}) +
                ", top-worst-error-m=" +
                std::to_string(
                    entities.worstPositionErrorMeters) +
                ", position-error-p95-m=" +
                std::to_string(
                    entities.positionErrorP95Meters) +
                ", oldest-missing-age-ms=" +
                std::to_string(
                    entities.oldestMissingAgeMilliseconds));
        } else if (wasDiverged) {
            facade_.Log(
                "[ENTITY_DIVERGENCE][EVENT] state=recovered");
        }
    }
    runtimeDiagnosticTickCount_ = 0U;
    runtimeDiagnosticTickElapsedSumMs_ = 0U;
    runtimeDiagnosticHitchCount_ = 0U;
    runtimeDiagnosticTickMaximumMs_ = 0U;
}

void BridgeRuntime::BeginProblemDiagnosticBurst(
    const std::uint64_t correlationId,
    const bool remoteOrigin,
    const std::uint64_t nowMs) noexcept {
    problemDiagnosticCorrelationId_ = correlationId;
    problemDiagnosticRemoteOrigin_ = remoteOrigin;
    problemDiagnosticUntilMs_ =
        nowMs + kProblemDiagnosticBurstMilliseconds;
    nextProblemDiagnosticSnapshotMs_ = nowMs;
    problemDiagnosticSampleIndex_ = 0U;
}

void BridgeRuntime::EmitProblemDiagnosticSnapshot(
    const std::optional<LocalPlayerSample>& localSample,
    const bool remoteStreaming,
    const std::uint64_t nowMs) {
    if (problemDiagnosticCorrelationId_ == 0U) {
        return;
    }
    if (nowMs > problemDiagnosticUntilMs_) {
        facade_.Log(
            "[PROBLEM_SNAPSHOT] correlation=" +
            std::to_string(problemDiagnosticCorrelationId_) +
            ", state=completed, samples=" +
            std::to_string(problemDiagnosticSampleIndex_));
        problemDiagnosticCorrelationId_ = 0U;
        problemDiagnosticUntilMs_ = 0U;
        nextProblemDiagnosticSnapshotMs_ = 0U;
        problemDiagnosticSampleIndex_ = 0U;
        return;
    }
    if (nowMs < nextProblemDiagnosticSnapshotMs_) {
        return;
    }
    nextProblemDiagnosticSnapshotMs_ =
        nowMs + kProblemDiagnosticSnapshotMilliseconds;
    problemDiagnosticSampleIndex_ =
        AdvanceNonZero(problemDiagnosticSampleIndex_);

    const MissionStatePayload* mission{};
    if (localSlot_ == PlayerSlot::Host && localMissionState_.has_value()) {
        mission = &*localMissionState_;
    } else if (
        localSlot_ == PlayerSlot::Guest && remoteMissionState_.has_value()) {
        mission = &*remoteMissionState_;
    }
    const MissionCinematicStatePayload* cinematic{};
    if (localSlot_ == PlayerSlot::Host &&
        localMissionCinematicState_.has_value()) {
        cinematic = &*localMissionCinematicState_;
    } else if (
        localSlot_ == PlayerSlot::Guest &&
        remoteMissionCinematicState_.has_value()) {
        cinematic = &*remoteMissionCinematicState_;
    }
    const auto divergence = facade_.SampleRuntimeDivergenceDiagnostics();
    const auto remoteAge = DiagnosticAge(lastRemoteStateMs_, nowMs);
    const auto actionAge = DiagnosticAge(
        lastRemotePlayerActionReceivedAtMs_,
        nowMs);
    const auto cameraAge = DiagnosticAge(
        remoteMissionCameraReceivedAtMs_,
        nowMs);
    const auto animSceneAge = DiagnosticAge(
        remoteAnimSceneReceivedAtMs_,
        nowMs);
    const auto localPosition =
        localSample.has_value() ? localSample->position : Vec3{};
    const auto remotePosition = latestRemoteState_.has_value()
        ? latestRemoteState_->position
        : Vec3{};

    facade_.Log(
        "[PROBLEM_SNAPSHOT] correlation=" +
        std::to_string(problemDiagnosticCorrelationId_) +
        ", sample=" +
        std::to_string(problemDiagnosticSampleIndex_) +
        ", origin=" +
        (problemDiagnosticRemoteOrigin_ ? "remote" : "local") +
        ", marker-role=" +
        std::string{ProblemMarkerOriginRole(
            problemDiagnosticCorrelationId_)} +
        ", marker-id=" +
        std::to_string(ProblemMarkerLocalId(
            problemDiagnosticCorrelationId_)) +
        ", role=" + std::string{RoleName(localSlot_)} +
        ", tick=" + std::to_string(nowMs) +
        ", pipe=" +
        std::to_string(transport_.IsConnected() ? 1 : 0) +
        ", remote-fresh=" +
        std::to_string(remoteStreaming ? 1 : 0) +
        ", remote-age-ms=" + std::to_string(remoteAge) +
        ", action-age-ms=" + std::to_string(actionAge) +
        ", camera-age-ms=" + std::to_string(cameraAge) +
        ", animscene-age-ms=" + std::to_string(animSceneAge) +
        ", local-sample=" +
        std::to_string(localSample.has_value() ? 1 : 0) +
        ", local-x=" + std::to_string(localPosition.x) +
        ", local-y=" + std::to_string(localPosition.y) +
        ", local-z=" + std::to_string(localPosition.z) +
        ", local-heading=" +
        std::to_string(
            localSample.has_value() ? localSample->heading : 0.0F) +
        ", local-mission=" +
        std::to_string(
            localSample.has_value() && localSample->missionActive ? 1 : 0) +
        ", local-cutscene=" +
        std::to_string(
            localSample.has_value() && localSample->cutsceneActive ? 1 : 0) +
        ", local-downed=" +
        std::to_string(
            localSample.has_value() && localSample->downed ? 1 : 0) +
        ", local-mounted=" +
        std::to_string(
            localSample.has_value() && localSample->mounted ? 1 : 0) +
        ", local-aim=" +
        std::to_string(
            localSample.has_value() && localSample->aiming ? 1 : 0) +
        ", local-fire=" +
        std::to_string(
            localSample.has_value() && localSample->firing ? 1 : 0) +
        ", local-melee=" +
        std::to_string(
            localSample.has_value() && localSample->meleeCombat ? 1 : 0) +
        ", local-block=" +
        std::to_string(
            localSample.has_value() && localSample->meleeBlocking ? 1 : 0) +
        ", local-grapple=" +
        std::to_string(
            localSample.has_value() && localSample->meleeGrappling ? 1 : 0) +
        ", local-lasso=" +
        std::to_string(
            localSample.has_value() && localSample->peerLassoActive ? 1 : 0) +
        ", local-cover=" +
        std::to_string(
            localSample.has_value() &&
                    (localSample->inCover || localSample->goingIntoCover)
                ? 1
                : 0) +
        ", local-stealth=" +
        std::to_string(
            localSample.has_value() && localSample->stealthMovement ? 1 : 0) +
        ", local-ragdoll=" +
        std::to_string(
            localSample.has_value() && localSample->ragdoll ? 1 : 0) +
        ", local-water=" +
        std::to_string(
            localSample.has_value() && localSample->inWater ? 1 : 0) +
        ", local-swim=" +
        std::to_string(
            localSample.has_value() && localSample->swimming ? 1 : 0) +
        ", local-weapon=" +
        std::to_string(
            localSample.has_value() ? localSample->weaponHash : 0U) +
        ", remote-state=" +
        std::to_string(latestRemoteState_.has_value() ? 1 : 0) +
        ", remote-x=" + std::to_string(remotePosition.x) +
        ", remote-y=" + std::to_string(remotePosition.y) +
        ", remote-z=" + std::to_string(remotePosition.z) +
        ", remote-heading=" +
        std::to_string(
            latestRemoteState_.has_value()
                ? latestRemoteState_->heading
                : 0.0F) +
        ", remote-flags=" +
        std::to_string(
            latestRemoteState_.has_value()
                ? latestRemoteState_->flags
                : 0U) +
        ", mission-epoch=" +
        std::to_string(mission != nullptr ? mission->missionEpoch : 0U) +
        ", mission-revision=" +
        std::to_string(mission != nullptr ? mission->revision : 0U) +
        ", mission-phase=" +
        std::string{MissionPhaseName(
            mission != nullptr ? mission->phase : MissionPhase::Idle)} +
        ", cinematic-generation=" +
        std::to_string(
            cinematic != nullptr ? cinematic->cinematicGeneration : 0U) +
        ", cinematic-revision=" +
        std::to_string(cinematic != nullptr ? cinematic->revision : 0U) +
        ", cinematic-phase=" +
        std::to_string(
            cinematic != nullptr
                ? static_cast<std::uint16_t>(cinematic->phase)
                : 0U) +
        ", cinematic-flags=" +
        std::to_string(cinematic != nullptr ? cinematic->flags : 0U) +
        ", animscene-host=" +
        std::to_string(localAnimSceneActive_ ? 1 : 0) +
        ", animscene-attached=" +
        std::to_string(remoteNativeAnimSceneActive_ ? 1 : 0) +
        ", presentation-ready=" +
        std::to_string(localCinematicPresentationReady_ ? 1 : 0) +
        ", resume-ready=" +
        std::to_string(localCinematicResumeReady_ ? 1 : 0) +
        ", quarantine=" +
        std::to_string(guestMissionQuarantineActive_ ? 1 : 0) +
        ", mission-gate-lease=" +
        std::to_string(guestMissionIsolationLeaseActive_ ? 1 : 0) +
        ", player-error-m=" +
        std::to_string(divergence.player.positionErrorMeters) +
        ", player-error-deg=" +
        std::to_string(divergence.player.rotationErrorDegrees));
}

bool BridgeRuntime::IsMissionWorldMirrorSafe() const noexcept {
    if (remoteMissionCinematicState_.has_value() &&
        IsCinematicPresentationPhase(
            remoteMissionCinematicState_->phase)) {
        return false;
    }
    if (remoteMissionState_.has_value()) {
        return remoteMissionState_->phase != MissionPhase::Cutscene &&
               remoteMissionState_->phase != MissionPhase::Loading &&
               remoteMissionState_->phase != MissionPhase::Recovery &&
               remoteMissionState_->phase !=
                   MissionPhase::SoloOverride;
    }
    return IsWorldMirrorSafeRemoteState(latestRemoteState_);
}

// Samples local combat/action intent, sends allowed requests, and keeps action revisions in sync so delayed packets cannot restart an old action.
void BridgeRuntime::TickLocalPlayerActions(
    const LocalPlayerSample& sample,
    const PlayerSlot localSlot,
    const std::uint64_t nowMs) {
    if (!transport_.IsConnected() || !localEntityId_.IsValid()) {
        // A named-pipe outage does not end the LAN session.
        // Keep the active runtime so the first sample after reconnect can emit End/Cancel with the original action id (especially the authoritative lasso release).
        // Fresh sessions and role mismatch paths clear this array explicitly.
        return;
    }

    struct ActionInput final {
        PlayerActionKind kind{PlayerActionKind::None};
        bool active{};
        bool persistent{};
        bool physicalTargetEffect{};
        bool restartOnEdge{};
        std::uint32_t variantHash{};
    };
    const std::array<ActionInput, 7> inputs{
        ActionInput{
            PlayerActionKind::Aim,
            sample.aiming && sample.aimTargetValid,
            true,
            false,
            false,
            0U},
        ActionInput{
            PlayerActionKind::MeleeAttack,
            sample.meleeCombat,
            false,
            false,
            sample.meleeAttackPressed,
            0U},
        ActionInput{
            PlayerActionKind::MeleeBlock,
            sample.meleeBlocking && !sample.inCover &&
                !sample.goingIntoCover,
            true,
            false,
            false,
            0U},
        ActionInput{
            PlayerActionKind::Grapple,
            sample.meleeGrappling && sample.peerCombatTarget,
            true,
            false,
            false,
            0U},
        ActionInput{
            PlayerActionKind::Lasso,
            sample.peerLassoIntent,
            true,
            sample.peerLassoActive,
            false,
            0U},
        ActionInput{
            PlayerActionKind::Knockdown,
            sample.peerKnockdown,
            false,
            true,
            false,
            sample.peerMountPull
                ? kPlayerActionVariantPeerMountPull
                : 0U},
        ActionInput{
            PlayerActionKind::Crafting,
            sample.scenarioActive && !sample.mounted && !sample.downed &&
                !sample.missionActive,
            true,
            false,
            false,
            0U}};

    constexpr auto kIntent = static_cast<std::uint32_t>(
        PlayerActionFlag::Intent);
    constexpr auto kAuthoritative = static_cast<std::uint32_t>(
        PlayerActionFlag::Authoritative);
    constexpr auto kTargetEntityValid = static_cast<std::uint32_t>(
        PlayerActionFlag::TargetEntityValid);
    constexpr auto kTargetPointValid = static_cast<std::uint32_t>(
        PlayerActionFlag::TargetPointValid);
    constexpr auto kActorAnchorValid = static_cast<std::uint32_t>(
        PlayerActionFlag::ActorAnchorValid);
    constexpr auto kPersistent = static_cast<std::uint32_t>(
        PlayerActionFlag::Persistent);
    constexpr auto kPhysicalTargetEffect = static_cast<std::uint32_t>(
        PlayerActionFlag::PhysicalTargetEffect);
    constexpr auto kNormalizedPhaseValid = static_cast<std::uint32_t>(
        PlayerActionFlag::NormalizedPhaseValid);
    constexpr auto kVariantValid = static_cast<std::uint32_t>(
        PlayerActionFlag::VariantValid);

    for (const auto& input : inputs) {
        const auto index = static_cast<std::size_t>(input.kind);
        if (index >= localPlayerActions_.size()) {
            continue;
        }
        auto& runtime = localPlayerActions_[index];
        // Each melee click is its own transaction.
        // A fresh edge preempts a still-finishing visual pulse instead of extending one autonomous combat task for the whole time the button is held.
        const bool begin =
            input.active &&
            (!runtime.active || input.restartOnEdge);
        const bool end = !input.active && runtime.active;
        const bool sustain =
            input.active && runtime.active && input.persistent &&
            (runtime.lastSentAtMs == 0U ||
             nowMs < runtime.lastSentAtMs ||
             nowMs - runtime.lastSentAtMs >=
                 kPlayerActionSustainMilliseconds);
        if (!begin && !end && !sustain) {
            continue;
        }

        if (begin) {
            runtime = LocalPlayerActionRuntime{};
            runtime.actionId = AdvanceNonZero(localPlayerActionId_);
            localPlayerActionId_ = runtime.actionId;
            runtime.revision = 1U;
            runtime.startedAtMs = nowMs;
            runtime.active = true;
            runtime.weaponHash = sample.weaponHash;
            runtime.variantHash = input.variantHash;
            runtime.flags =
                (localSlot == PlayerSlot::Host
                     ? kAuthoritative
                     : kIntent) |
                kActorAnchorValid |
                kNormalizedPhaseValid;
            if (input.persistent) {
                runtime.flags |= kPersistent;
            }
            if (runtime.variantHash != 0U) {
                runtime.flags |= kVariantValid;
            }
            if (sample.peerCombatTarget &&
                remoteReplicaId_.IsValid()) {
                runtime.targetEntityId = remoteReplicaId_;
                runtime.flags |= kTargetEntityValid;
                if (input.physicalTargetEffect) {
                    runtime.flags |= kPhysicalTargetEffect;
                }
            }
            if (sample.aimTargetValid) {
                runtime.targetPoint = sample.aimTarget;
                runtime.flags |= kTargetPointValid;
            }
        } else {
            runtime.revision = AdvanceNonZero(runtime.revision);
            runtime.flags &=
                ~(kTargetEntityValid |
                  kTargetPointValid |
                  kPhysicalTargetEffect);
            runtime.targetEntityId = NetEntityId{};
            runtime.targetPoint = Vec3{};
            runtime.weaponHash = sample.weaponHash;
            if (input.active && sample.aimTargetValid) {
                runtime.targetPoint = sample.aimTarget;
                runtime.flags |= kTargetPointValid;
            }
            if (input.active && sample.peerCombatTarget &&
                remoteReplicaId_.IsValid()) {
                runtime.targetEntityId = remoteReplicaId_;
                runtime.flags |= kTargetEntityValid;
                if (input.physicalTargetEffect) {
                    runtime.flags |= kPhysicalTargetEffect;
                }
            }
        }

        const auto duration =
            input.persistent
                ? kPersistentPlayerActionDurationMilliseconds
                : kTransientPlayerActionDurationMilliseconds;
        const auto elapsed =
            nowMs >= runtime.startedAtMs
                ? static_cast<std::uint32_t>(
                      std::min<std::uint64_t>(
                          nowMs - runtime.startedAtMs,
                          duration))
                : 0U;
        localPlayerActionSequence_ =
            AdvanceNonZero(localPlayerActionSequence_);
        PlayerActionPayload action;
        action.actorEntityId = localEntityId_;
        action.targetEntityId = runtime.targetEntityId;
        action.sequence = localPlayerActionSequence_;
        action.actionId = runtime.actionId;
        action.revision = runtime.revision;
        action.actorSlot = localSlot;
        action.authoritySlot = localSlot;
        action.kind = input.kind;
        action.phase = begin
                           ? PlayerActionPhase::Begin
                           : end
                                 ? PlayerActionPhase::End
                                 : PlayerActionPhase::Sustain;
        action.flags = runtime.flags;
        action.durationMilliseconds = duration;
        action.phaseElapsedMilliseconds = elapsed;
        action.weaponHash = runtime.weaponHash;
        action.variantHash = runtime.variantHash;
        action.actorAnchor = sample.position;
        action.targetPoint = runtime.targetPoint;
        action.facingHeading = NormalizeHeading(sample.heading);
        // This is the normalized phase of the reliable semantic transaction, not a claim that the underlying RAGE clip cursor was read.
        // It gives paired-action receivers a stable shared clock today and can be correlated with an exact clip sample when the versioned reader is available later.
        action.normalizedPhase =
            duration == 0U
                ? 0.0F
                : std::clamp(
                      static_cast<float>(elapsed) /
                          static_cast<float>(duration),
                      0.0F,
                      1.0F);

        try {
            Frame frame;
            frame.header.type = MessageType::PlayerAction;
            frame.header.sequence = sequencer_.Next();
            frame.header.tick = nowMs;
            frame.payload = EncodePlayerAction(action);
            SendBestEffort(std::move(frame));
            runtime.lastSentAtMs = nowMs;
            ++playerActionsSent_;
            if (begin || end) {
                facade_.Log(
                    "[ACTION_TX] tx kind=" +
                    std::to_string(
                        static_cast<std::uint8_t>(input.kind)) +
                    ", phase=" +
                    std::string{begin ? "begin" : "end"} +
                    ", action-id=" +
                    std::to_string(runtime.actionId) +
                    ", revision=" +
                    std::to_string(runtime.revision) +
                    ", target-peer=" +
                    std::to_string(
                        runtime.targetEntityId.IsValid() ? 1 : 0) +
                    ", physical-effect=" +
                    std::to_string(
                        (runtime.flags &
                         kPhysicalTargetEffect) != 0U
                            ? 1
                            : 0) +
                    ", duration-ms=" +
                    std::to_string(duration));
                facade_.Log(
                    std::string{ActionLifecycleTag(input.kind)} +
                    " direction=tx, state=" +
                    (begin ? "begin" : "end") +
                    ", correlation=action-" +
                    std::to_string(runtime.actionId) +
                    ", action-id=" +
                    std::to_string(runtime.actionId) +
                    ", kind=" +
                    std::to_string(
                        static_cast<std::uint8_t>(input.kind)) +
                    ", revision=" +
                    std::to_string(runtime.revision) +
                    ", reason=" +
                    (begin ? "local-input-edge" : "local-input-release"));
            }
        } catch (const std::exception&) {
            ++playerActionsRejected_;
            facade_.Log(
                "[ERROR][ACTION_TX] local action payload validation failed");
        }
        if (end) {
            runtime = LocalPlayerActionRuntime{};
        }
    }
}

// Samples hold-to-interact actions such as revive/mount/dismount.
// The host validates each request before either game applies the result.
void BridgeRuntime::TickLocalInteractions(
    const LocalPlayerSample& sample,
    const PlayerSlot localSlot,
    const std::uint64_t nowMs) {
    const auto remoteSlot = OtherSlot(localSlot);
    const auto remoteId = playerEntityIds_[SlotIndex(remoteSlot)];
    const auto remoteLifecycle = players_.State(remoteSlot).lifecycle;
    const bool localAlive =
        players_.State(localSlot).lifecycle == PlayerLifecycle::Alive;
    const bool remoteDowned =
        remoteLifecycle == PlayerLifecycle::Downed ||
        remoteLifecycle == PlayerLifecycle::Reviving;
    const bool remoteRestrained =
        remoteRestraintState_.has_value() &&
        remoteRestraintState_->subjectEntityId == remoteId &&
            remoteRestraintState_->state != PlayerRestraintState::Free;
    constexpr auto kMountPresent = static_cast<std::uint8_t>(
        PlayerMountStateFlag::Present);
    constexpr auto kVehicle = static_cast<std::uint8_t>(
        PlayerMountStateFlag::Vehicle);
    constexpr auto kVehicleDriver = static_cast<std::uint8_t>(
        PlayerMountStateFlag::VehicleDriver);
    const bool remoteDrivingSharedWagon =
        localSlot == PlayerSlot::Guest && remoteMountState_.has_value() &&
        (remoteMountState_->flags & (kMountPresent | kVehicle | kVehicleDriver)) ==
            (kMountPresent | kVehicle | kVehicleDriver) &&
        remoteMountState_->mountEntityId.IsValid();
    const bool alreadyInSharedWagon =
        sample.mount.has_value() && sample.mount->vehicle;
    const auto distance = facade_.HostGuestDistanceMeters().value_or(
        std::numeric_limits<float>::infinity());
    const auto desiredKind =
        localAlive && remoteDowned && distance <= 2.0F
            ? InteractionKind::Revive
            : remoteRestrained && distance <= 2.0F
                  ? InteractionKind::ReleaseRestraint
                  : remoteDrivingSharedWagon && !alreadyInSharedWagon &&
                            distance <= 3.5F
                      ? InteractionKind::MountPassenger
                  : InteractionKind::None;
    const bool wantsInteraction =
        sample.interactionHeld &&
        desiredKind != InteractionKind::None &&
        remoteId.IsValid();

    const bool targetChanged =
        localInteraction_.active &&
        (localInteraction_.kind != desiredKind ||
         localInteraction_.targetEntityId != remoteId);
    const bool begin =
        wantsInteraction &&
        (!localInteraction_.active || targetChanged);
    const bool cancel =
        localInteraction_.active &&
        (!wantsInteraction || targetChanged);
    const bool sustain =
        wantsInteraction &&
        localInteraction_.active &&
        !targetChanged &&
        (localInteraction_.lastSentAtMs == 0U ||
         nowMs < localInteraction_.lastSentAtMs ||
         nowMs - localInteraction_.lastSentAtMs >=
             kInteractionSustainMilliseconds);
    if (!begin && !cancel && !sustain) {
        return;
    }

    InteractionIntentPayload intent;
    if (begin) {
        localInteraction_ = LocalInteractionRuntime{};
        localInteraction_.interactionId =
            AdvanceNonZero(localInteractionId_);
        localInteractionId_ = localInteraction_.interactionId;
        localInteraction_.revision = 1U;
        localInteraction_.startedAtMs = nowMs;
        localInteraction_.targetEntityId = remoteId;
        localInteraction_.secondaryEntityId =
            desiredKind == InteractionKind::MountPassenger
                ? remoteMountState_->mountEntityId
                : NetEntityId{};
        localInteraction_.kind = desiredKind;
        localInteraction_.active = true;
    } else {
        localInteraction_.revision =
            AdvanceNonZero(localInteraction_.revision);
    }
    intent.actorEntityId = localEntityId_;
    intent.targetEntityId = localInteraction_.targetEntityId;
    intent.secondaryEntityId = localInteraction_.secondaryEntityId;
    intent.interactionId = localInteraction_.interactionId;
    intent.revision = localInteraction_.revision;
    intent.actorSlot = localSlot;
    intent.kind = localInteraction_.kind;
    intent.phase = begin
                       ? InteractionIntentPhase::Begin
                       : cancel
                             ? InteractionIntentPhase::Cancel
                             : InteractionIntentPhase::Sustain;
    intent.flags = static_cast<std::uint8_t>(
        InteractionIntentFlag::TargetPlayer);
    if (!cancel && intent.kind == InteractionKind::Revive) {
        intent.flags |= static_cast<std::uint8_t>(
            InteractionIntentFlag::HoldRequired);
        intent.requestedDurationMilliseconds = 4'000U;
    }

    try {
        Frame frame;
        frame.header.type = MessageType::InteractionIntent;
        frame.header.sequence = sequencer_.Next();
        frame.header.tick = nowMs;
        frame.payload = EncodeInteractionIntent(intent);
        SendBestEffort(std::move(frame));
        localInteraction_.lastSentAtMs = nowMs;
        if (begin || cancel) {
            facade_.Log(
                "[INTERACTION_TX] kind=" +
                std::to_string(static_cast<std::uint8_t>(intent.kind)) +
                ", phase=" +
                std::to_string(static_cast<std::uint8_t>(intent.phase)) +
                ", interaction-id=" +
                std::to_string(intent.interactionId) +
                ", target=" +
                std::to_string(intent.targetEntityId.Value()));
        }
    } catch (const std::exception&) {
        facade_.Log(
            "[ERROR][INTERACTION_TX] local interaction payload validation failed");
        localInteraction_ = LocalInteractionRuntime{};
    }
    if (cancel) {
        localInteraction_ = LocalInteractionRuntime{};
    }
}

void BridgeRuntime::HandleInteractionResult(
    const InteractionResultPayload& result) {
    if (!localSlot_.has_value() ||
        (result.actorEntityId != localEntityId_ &&
         result.targetEntityId != localEntityId_ &&
         result.actorEntityId != remoteReplicaId_ &&
         result.targetEntityId != remoteReplicaId_)) {
        return;
    }
    const bool terminal = result.status ==
                              InteractionResultStatus::Completed ||
                          result.status ==
                              InteractionResultStatus::Rejected ||
                          result.status ==
                              InteractionResultStatus::Cancelled;
    if (result.actorEntityId == localEntityId_ &&
        localInteraction_.active &&
        localInteraction_.interactionId == result.interactionId &&
        terminal) {
        localInteraction_ = LocalInteractionRuntime{};
    }
    if (result.kind == InteractionKind::Revive &&
        result.status == InteractionResultStatus::Completed) {
        if (const auto target = FindSlot(result.targetEntityId);
            target.has_value()) {
            players_.SetAlive(*target, kRevivedHealthFraction);
            if (*target == *localSlot_) {
                previousLocalLifecycle_ = PlayerLifecycle::Alive;
            } else {
                remoteLifecycle_ = PlayerLifecycle::Alive;
            }
        }
        pendingRevive_.reset();
    }
    if (!facade_.ApplyInteractionResult(result, localEntityId_)) {
        facade_.Log(
            "[WARNING][INTERACTION_APPLY] authoritative result could not be applied to a local handle");
    }
    if (terminal) {
        facade_.Log(
            "[INTERACTION_RX] kind=" +
            std::to_string(static_cast<std::uint8_t>(result.kind)) +
            ", status=" +
            std::to_string(static_cast<std::uint8_t>(result.status)) +
            ", reason=" +
            std::to_string(static_cast<std::uint8_t>(result.rejectReason)) +
            ", interaction-id=" +
            std::to_string(result.interactionId));
    }
}

void BridgeRuntime::HandleRestraintState(
    const RestraintStatePayload& state) {
    if (ApplyMatchedRestraintState(state)) {
        pendingRestraintStates_.erase(state.subjectEntityId);
        return;
    }

    CachePendingRestraintState(state);
}

bool BridgeRuntime::ApplyMatchedRestraintState(
    const RestraintStatePayload& state) {
    if (state.subjectEntityId == localEntityId_) {
        localRestraintState_ = state;
    } else if (state.subjectEntityId == remoteReplicaId_) {
        remoteRestraintState_ = state;
    } else {
        return false;
    }
    const bool applied = facade_.ApplyRestraintState(state, localEntityId_);
    if (!applied) {
        facade_.Log(
            "[WARNING][RESTRAINT_APPLY] authoritative restraint state could not be applied");
    }
    facade_.Log(
        "[RESTRAINT_FSM] subject=" +
        std::to_string(state.subjectEntityId.Value()) +
        ", owner=" +
        std::to_string(state.ownerEntityId.Value()) +
        ", state=" +
        std::to_string(static_cast<std::uint8_t>(state.state)) +
        ", revision=" +
        std::to_string(state.revision));
    return applied;
}

void BridgeRuntime::CachePendingRestraintState(
    const RestraintStatePayload& state) {
    pendingRestraintStates_.insert_or_assign(
        state.subjectEntityId,
        state);
    facade_.Log(
        "[RESTRAINT_FSM] deferred unmatched subject=" +
        std::to_string(state.subjectEntityId.Value()) +
        ", state=" +
        std::to_string(static_cast<std::uint8_t>(state.state)) +
        ", revision=" +
        std::to_string(state.revision));
}

void BridgeRuntime::ApplyPendingRestraintState(
    const NetEntityId subjectEntityId) {
    const auto pending = pendingRestraintStates_.find(subjectEntityId);
    if (pending == pendingRestraintStates_.end()) {
        return;
    }

    const auto state = pending->second;
    if (!ApplyMatchedRestraintState(state)) {
        return;
    }
    pendingRestraintStates_.erase(pending);
    facade_.Log(
        "[RESTRAINT_FSM] applied deferred subject=" +
        std::to_string(subjectEntityId.Value()) +
        ", revision=" +
        std::to_string(state.revision));
}

// Applies a host-approved remote action or forwards a guest request to the host policy.
// Action IDs and revisions protect against delayed repeats.
void BridgeRuntime::HandleRemotePlayerAction(
    const Frame& frame,
    const PlayerActionPayload& action) {
    constexpr auto kIntent = static_cast<std::uint32_t>(
        PlayerActionFlag::Intent);
    constexpr auto kAuthoritative = static_cast<std::uint32_t>(
        PlayerActionFlag::Authoritative);
    if (localSlot_.has_value() &&
        (action.actorSlot == *localSlot_ ||
         action.actorEntityId == localEntityId_)) {
        // The host sidecar echoes its authoritative rewrite back to the actor as an acknowledgement.
        // The local game already owns that animation, so consuming it without applying a second task avoids both feedback loops and false rejection counters.
        if (action.authoritySlot == PlayerSlot::Host &&
            (action.flags & kAuthoritative) != 0U &&
            (action.flags & kIntent) == 0U) {
            return;
        }
        ++playerActionsRejected_;
        return;
    }
    if (!localSlot_.has_value() ||
        (remoteReplicaId_.IsValid() &&
         action.actorEntityId != remoteReplicaId_)) {
        ++playerActionsRejected_;
        return;
    }
    constexpr auto kTargetEntityValid = static_cast<std::uint32_t>(
        PlayerActionFlag::TargetEntityValid);
    constexpr auto kPhysicalTargetEffect = static_cast<std::uint32_t>(
        PlayerActionFlag::PhysicalTargetEffect);
    if ((action.flags & kTargetEntityValid) != 0U &&
        action.targetEntityId != localEntityId_) {
        ++playerActionsRejected_;
        facade_.Log(
            "[WARNING][ACTION_TX] rejected action targeting an unknown local entity");
        return;
    }
    const auto disposition =
        remotePlayerActionSequences_.Observe(action.sequence);
    if (disposition == SequenceDisposition::Duplicate) {
        ++playerActionsDuplicate_;
        return;
    }
    if (disposition == SequenceDisposition::Stale) {
        ++playerActionsStale_;
        return;
    }
    lastRemotePlayerActionReceivedAtMs_ = previousTickMs_;
    if (remoteParticipantSceneIsolated_) {
        // Consume the reliable revision so stale combat cannot replay after a cutscene, but never give an action task ownership of the host camera scene.
        ++playerActionsReceived_;
        return;
    }
    if (!facade_.ApplyRemotePlayerAction(action)) {
        ++playerActionsRejected_;
        facade_.Log(
            "[WARNING][ACTION_APPLY] remote player action was not applied");
        return;
    }
    ++playerActionsReceived_;
    if (action.phase != PlayerActionPhase::Sustain &&
        action.phase != PlayerActionPhase::Active) {
        facade_.Log(
            "[ACTION_TX] rx kind=" +
            std::to_string(
                static_cast<std::uint8_t>(action.kind)) +
            ", phase=" +
            std::to_string(
                static_cast<std::uint8_t>(action.phase)) +
            ", action-id=" +
            std::to_string(action.actionId) +
            ", revision=" +
            std::to_string(action.revision) +
            ", target-local=" +
            std::to_string(
                (action.flags & kTargetEntityValid) != 0U ? 1 : 0) +
            ", physical-effect=" +
            std::to_string(
                (action.flags & kPhysicalTargetEffect) != 0U ? 1 : 0) +
            ", sender-tick=" +
            std::to_string(frame.header.tick) +
            ", local-tick=" +
            std::to_string(previousTickMs_));
        facade_.Log(
            std::string{ActionLifecycleTag(action.kind)} +
            " direction=rx, state=transition, correlation=action-" +
            std::to_string(action.actionId) +
            ", action-id=" +
            std::to_string(action.actionId) +
            ", kind=" +
            std::to_string(
                static_cast<std::uint8_t>(action.kind)) +
            ", phase=" +
            std::to_string(
                static_cast<std::uint8_t>(action.phase)) +
            ", revision=" +
            std::to_string(action.revision) +
            ", reason=network-transition");
    }
}

void BridgeRuntime::HandleRemoteMountState(
    const Frame& frame,
    const PlayerMountStatePayload& state) {
    if (!localSlot_.has_value() ||
        state.slot == *localSlot_ ||
        state.playerEntityId == localEntityId_ ||
        (remoteReplicaId_.IsValid() &&
         state.playerEntityId != remoteReplicaId_)) {
        return;
    }
    const auto disposition =
        remoteMountSequences_.Observe(
            frame.header.sequence);
    if (disposition ==
            SequenceDisposition::Duplicate ||
        disposition ==
            SequenceDisposition::Stale) {
        return;
    }
    constexpr auto kPresent =
        static_cast<std::uint8_t>(
            PlayerMountStateFlag::Present);
    if ((state.flags & kPresent) == 0U) {
        if (remoteMountAbsenceConfirmed_) {
            return;
        }
        if (!pendingRemoteMountAbsentState_.has_value()) {
            facade_.Log(
                "[REMOTE_MOUNT_RX] absence candidate started; waiting for debounce");
            facade_.Log(
                "[MOUNT_LIFECYCLE] direction=rx, state=absence-pending, correlation=mount-" +
                std::to_string(state.generation) +
                ", generation=" +
                std::to_string(state.generation) +
                ", reason=remote-present-flag-cleared");
        }
        pendingRemoteMountAbsentState_ = state;
        if (remoteMountAbsentSinceMs_ == 0U ||
            previousTickMs_ < remoteMountAbsentSinceMs_) {
            remoteMountAbsentSinceMs_ = previousTickMs_;
        }
        return;
    }
    pendingRemoteMountAbsentState_.reset();
    remoteMountAbsentSinceMs_ = 0U;
    remoteMountAbsenceConfirmed_ = false;
    if (!remoteMountState_.has_value() ||
        remoteMountState_->mountEntityId !=
            state.mountEntityId ||
        remoteMountState_->flags != state.flags ||
        remoteMountState_->generation != state.generation) {
        facade_.Log(
            "[REMOTE_MOUNT_RX] id=" +
            std::to_string(state.mountEntityId.Value()) +
            ", generation=" +
            std::to_string(state.generation) +
            ", flags=" +
            std::to_string(state.flags));
        constexpr auto kMountedFlag =
            static_cast<std::uint8_t>(
                PlayerMountStateFlag::Mounted);
        constexpr auto kDeadFlag =
            static_cast<std::uint8_t>(
                PlayerMountStateFlag::Dead);
        facade_.Log(
            "[MOUNT_LIFECYCLE] direction=rx, state=" +
            std::string{
                (state.flags & kDeadFlag) != 0U
                    ? "dead"
                    : (state.flags & kMountedFlag) != 0U
                          ? "mounted"
                          : "present"} +
            ", correlation=mount-" +
            std::to_string(state.generation) +
            ", generation=" +
            std::to_string(state.generation) +
            ", reason=remote-state-change");
    }
    const bool mountRelationChanged =
        !remoteMountState_.has_value() ||
        remoteMountState_->mountEntityId != state.mountEntityId ||
        remoteMountState_->flags != state.flags ||
        remoteMountState_->generation != state.generation;
    remoteMountState_ = state;
    if (mountRelationChanged) {
        nextRemoteMountMaintainMs_ = previousTickMs_;
    }
}

void BridgeRuntime::HandleDamageIntent(
    const DamageIntentPayload& intent) {
    const auto reject = [&](const std::string_view reason) {
        facade_.Log(
            "[MISSION_DAMAGE] rejected reason=" +
            std::string{reason} +
            ", attacker=" +
            std::to_string(intent.attackerId.Value()) +
            ", target=" +
            std::to_string(intent.targetId.Value()) +
            ", shot-seq=" +
            std::to_string(intent.shotSequence));
    };

    // Only the host can turn a guest shot request into real RDR2 damage.
    // The guest only sees a copied NPC, so it cannot decide damage by itself.
    if (localSlot_ != PlayerSlot::Host ||
        !worldMirrorHost_.has_value() ||
        !hostWorldMirrorActive_) {
        reject("host-world-authority-unavailable");
        return;
    }
    if (intent.attackerId !=
        playerEntityIds_[SlotIndex(PlayerSlot::Guest)]) {
        reject("attacker-is-not-authenticated-guest");
        return;
    }
    if (intent.targetId ==
            playerEntityIds_[SlotIndex(PlayerSlot::Host)] ||
        intent.targetId ==
            playerEntityIds_[SlotIndex(PlayerSlot::Guest)]) {
        reject("player-target-not-world-entity");
        return;
    }
    if (!latestRemoteState_.has_value() ||
        !lastRemoteStateMs_.has_value() ||
        ElapsedMilliseconds(
            *lastRemoteStateMs_,
            previousTickMs_) >
            kRemotePlayerFreshnessMilliseconds) {
        reject("guest-transform-stale");
        return;
    }
    if (!remoteEquipment_.has_value() ||
        remoteEquipment_->entityId != intent.attackerId ||
        remoteEquipment_->weaponHash == 0U ||
        remoteEquipment_->weaponHash != intent.weaponHash) {
        reject("guest-weapon-not-authoritative");
        return;
    }
    if (!std::isfinite(intent.damage) ||
        intent.damage <= 0.0F ||
        intent.damage > kWorldDamageMaximumPerIntent) {
        reject("invalid-damage-value");
        return;
    }

    // DamageIntent is reliable TCP control while PlayerState is best-effort UDP.
    // Requiring the latest snapshot's transient Firing bit creates a cross-channel reorder drop.
    // Authentication, guest identity, fresh transform, equipped weapon, target ownership, range, rate and replay checks below remain authoritative.
    const auto disposition =
        worldDamageIntentSequences_.Observe(
            intent.shotSequence);
    if (disposition == SequenceDisposition::Duplicate ||
        disposition == SequenceDisposition::Stale) {
        reject("duplicate-or-stale-shot-sequence");
        return;
    }
    const auto now = previousTickMs_;
    if (lastWorldDamageIntentMs_ != 0U &&
        now >= lastWorldDamageIntentMs_ &&
        now - lastWorldDamageIntentMs_ <
            kWorldDamageMinimumIntervalMilliseconds) {
        reject("shot-rate-limit");
        return;
    }

    // The target must still belong to this host's currently mirrored graph.
    // A shot at an NPC outside its 48-entity/radius selection is rejected.
    const auto targetState =
        worldMirrorHost_->FindState(intent.targetId);
    const auto localHandle =
        worldMirrorHost_->FindLocal(intent.targetId);
    constexpr auto kScriptOwned = static_cast<std::uint8_t>(
        WorldEntityStateFlag::ScriptOwned);
    constexpr auto kInCombat = static_cast<std::uint8_t>(
        WorldEntityStateFlag::InCombat);
    if (!targetState.has_value() || !localHandle.has_value()) {
        reject("target-not-in-host-entity-graph");
        return;
    }
    if ((targetState->flags &
         static_cast<std::uint8_t>(
             WorldEntityStateFlag::Dead)) != 0U) {
        reject("target-already-dead");
        return;
    }
    if (Distance(
            latestRemoteState_->position,
            targetState->position) >
        kWorldDamageMaximumMeters) {
        reject("target-out-of-authoritative-range");
        return;
    }

    const bool scriptOwned =
        (targetState->flags & kScriptOwned) != 0U;
    if (scriptOwned) {
        constexpr auto kMissionActive = static_cast<std::uint8_t>(
            MissionStateFlag::MissionActive);
        const bool activeHostMission =
            localMissionState_.has_value() &&
            (localMissionState_->flags & kMissionActive) != 0U &&
            localMissionState_->phase == MissionPhase::Active;
        if (!activeHostMission) {
            reject("script-owned-target-without-active-host-mission");
            return;
        }
        if ((targetState->flags & kInCombat) == 0U ||
            targetState->combatTargetSlot ==
                WorldCombatTargetSlot::None) {
            reject("script-owned-target-not-hostile");
            return;
        }
    }

    const auto appliedDamage =
        std::clamp(
            intent.damage,
            1.0F,
            kWorldDamageMaximumPerIntent);
    // Script-owned mission targets take the attributed projectile route; ambient peds use direct authoritative damage.
    // Either way the host reports outcome.
    const bool applied =
        scriptOwned
            ? facade_.ApplyMissionWorldEntityDamage(
                  *localHandle,
                  intent.weaponHash,
                  appliedDamage)
            : facade_.ApplyWorldEntityDamage(
                  *localHandle,
                  appliedDamage);
    if (!applied) {
        reject(
            scriptOwned
                ? "attributed-mission-projectile-failed"
                : "ambient-direct-damage-failed");
        return;
    }
    lastWorldDamageIntentMs_ = now;
    facade_.Log(
        "[MISSION_DAMAGE] accepted path=" +
        std::string{
            scriptOwned
                ? "script-owned-attributed-projectile"
                : "ambient-direct-damage"} +
        ", target=" +
        std::to_string(intent.targetId.Value()) +
        ", weapon=" +
        std::to_string(intent.weaponHash) +
        ", damage=" +
        std::to_string(appliedDamage) +
        ", shot-seq=" +
        std::to_string(intent.shotSequence));
}

void BridgeRuntime::HandleLocalPauseToggle() {
    if (!localSlot_.has_value() ||
        !transport_.IsConnected()) {
        return;
    }

    const bool desiredPaused = !synchronizedPaused_;
    if (*localSlot_ == PlayerSlot::Guest) {
        Frame request;
        request.header.type = MessageType::PauseVote;
        request.header.sequence = sequencer_.Next();
        request.header.tick = previousTickMs_;
        request.payload = EncodePauseVote(
            PauseVotePayload{
                PauseVoteKind::RequestState,
                PlayerSlot::Guest,
                desiredPaused
                    ? static_cast<std::uint8_t>(
                          PauseVoteFlag::Paused)
                    : 0U,
                pauseVoteGeneration_});
        SendBestEffort(std::move(request));
        facade_.Log(
            synchronizedPaused_
                ? "guest requested synchronized resume"
                : "guest requested synchronized pause");
        return;
    }

    synchronizedPaused_ = desiredPaused;
    hostPauseVoted_ = false;
    guestPauseVoted_ = false;
    pauseVoteGeneration_ =
        pauseVoteGeneration_ ==
                std::numeric_limits<std::uint32_t>::max()
            ? 0U
            : pauseVoteGeneration_ + 1U;
    PublishPauseVoteState();
}

void BridgeRuntime::HandlePauseVote(
    const Frame& frame,
    const PauseVotePayload& payload) {
    if (!localSlot_.has_value()) {
        return;
    }
    const auto disposition =
        pauseVoteSequences_.Observe(frame.header.sequence);
    if (disposition == SequenceDisposition::Duplicate ||
        disposition == SequenceDisposition::Stale) {
        return;
    }

    if (*localSlot_ == PlayerSlot::Host) {
        if (payload.kind != PauseVoteKind::RequestState ||
            payload.voterSlot != PlayerSlot::Guest) {
            return;
        }
        if (payload.generation != pauseVoteGeneration_) {
            PublishPauseVoteState();
            return;
        }
        const bool desiredPaused =
            (payload.flags &
             static_cast<std::uint8_t>(
                 PauseVoteFlag::Paused)) != 0U;
        pauseStateChangedByRemoteThisTick_ =
            synchronizedPaused_ != desiredPaused;
        synchronizedPaused_ = desiredPaused;
        hostPauseVoted_ = false;
        guestPauseVoted_ = false;
        pauseVoteGeneration_ =
            pauseVoteGeneration_ ==
                    std::numeric_limits<std::uint32_t>::max()
                ? 0U
                : pauseVoteGeneration_ + 1U;
        PublishPauseVoteState();
        return;
    }

    if (payload.kind !=
            PauseVoteKind::AuthoritativeState ||
        payload.voterSlot != PlayerSlot::Host) {
        return;
    }
    pauseVoteGeneration_ = payload.generation;
    hostPauseVoted_ =
        (payload.flags &
         static_cast<std::uint8_t>(
             PauseVoteFlag::HostVoted)) != 0U;
    guestPauseVoted_ =
        (payload.flags &
         static_cast<std::uint8_t>(
             PauseVoteFlag::GuestVoted)) != 0U;
    const bool desiredPaused =
        (payload.flags &
         static_cast<std::uint8_t>(
             PauseVoteFlag::Paused)) != 0U;
    pauseStateChangedByRemoteThisTick_ =
        synchronizedPaused_ != desiredPaused;
    synchronizedPaused_ = desiredPaused;
}

void BridgeRuntime::PublishPauseVoteState() {
    if (localSlot_ != PlayerSlot::Host ||
        !transport_.IsConnected()) {
        return;
    }
    std::uint8_t flags{};
    if (synchronizedPaused_) {
        flags |= static_cast<std::uint8_t>(
            PauseVoteFlag::Paused);
    }
    Frame state;
    state.header.type = MessageType::PauseVote;
    state.header.sequence = sequencer_.Next();
    state.header.tick = previousTickMs_;
    state.payload = EncodePauseVote(
        PauseVotePayload{
            PauseVoteKind::AuthoritativeState,
            PlayerSlot::Host,
            flags,
            pauseVoteGeneration_});
    SendBestEffort(std::move(state));
    facade_.Log(
        synchronizedPaused_
            ? "authoritative synchronized pause state published"
            : "authoritative synchronized resume state published");
}

void BridgeRuntime::ResetPauseVoteState(
    const bool notifyPeer) noexcept {
    try {
        const bool changed =
            synchronizedPaused_ ||
            hostPauseVoted_ ||
            guestPauseVoted_;
        synchronizedPaused_ = false;
        hostPauseVoted_ = false;
        guestPauseVoted_ = false;
        pauseVoteSequences_.Reset();
        if (changed &&
            notifyPeer &&
            localSlot_ == PlayerSlot::Host) {
            pauseVoteGeneration_ =
                pauseVoteGeneration_ ==
                        std::numeric_limits<std::uint32_t>::max()
                    ? 0U
                    : pauseVoteGeneration_ + 1U;
            PublishPauseVoteState();
        }
    } catch (...) {
        synchronizedPaused_ = false;
        hostPauseVoted_ = false;
        guestPauseVoted_ = false;
    }
}

// Main pipe receive switch.
// It decodes each Sidecar message and sends it to the correct multiplayer subsystem; it does not trust a type without validation.
void BridgeRuntime::HandleInboundFrame(const Frame& frame) {
    if (frame.header.type == MessageType::HelloAck) {
        AcceptHelloAck(frame.payload);
        return;
    }
    if (frame.header.type == MessageType::SessionMenuStatus) {
        const auto status = DecodeSessionMenuStatus(frame.payload);
        if (!status.has_value()) {
            sessionMenu_.SetStatus(
                SessionOverlayPhase::Error,
                "The sidecar sent an invalid session status.");
            return;
        }
        switch (status->kind) {
            case SessionMenuStatusKind::Waiting:
                ReleaseGuestMissionIsolationLease(
                    "explicit session stop/cancel acknowledged");
                if (localSlot_.has_value()) {
                    facade_.MaintainMissionSpectator(false);
                    facade_.MaintainMissionResumeBarrier(false);
                    facade_.MaintainRemoteMissionParticipant(false);
                    facade_.MaintainRealtimeSession(false, false);
                    facade_.MaintainLocalDownedState(
                        false,
                        kRevivedHealthFraction);
                    localInteraction_ = LocalInteractionRuntime{};
                    localRestraintState_.reset();
                    remoteRestraintState_.reset();
                    pendingRestraintStates_.clear();
                    interactionResultSequences_.Reset();
                    restraintStateSequences_.Reset();
                    players_.SetAlive(
                        *localSlot_,
                        kRevivedHealthFraction);
                    previousLocalLifecycle_ = PlayerLifecycle::Alive;
                    if (*localSlot_ == PlayerSlot::Host) {
                        ResetHostWorldMirror(false);
                    } else {
                        ResetGuestWorldMirror();
                    }
                    DespawnRemoteReplica();
                    hostInviteCode_.clear();
                    ++sessionEpoch_;
                    if (sessionEpoch_ == 0U) {
                        sessionEpoch_ = 1U;
                    }
                    localMissionState_.reset();
                    remoteMissionState_.reset();
                    remoteMissionSequences_.Reset();
                    soloOverride_ = false;
                    cutsceneSpectator_ = false;
                    SendHello();
                }
                sessionMenu_.MarkSessionStopped(status->message);
                break;
            case SessionMenuStatusKind::StartingHost:
                sessionMenu_.SetStatus(
                    SessionOverlayPhase::StartingHost,
                    status->message);
                break;
            case SessionMenuStatusKind::StartingGuest:
                sessionMenu_.SetStatus(
                    SessionOverlayPhase::StartingGuest,
                    status->message);
                break;
            case SessionMenuStatusKind::ReadyHost: {
                hostInviteCode_ = status->inviteCode;
                const bool copied =
                    !hostInviteCode_.empty() &&
                    facade_.WriteClipboardText(hostInviteCode_);
                sessionMenu_.MarkSessionReady(
                    true,
                    copied
                        ? status->message + " Copied."
                        : "HOST ready. The code could not be copied.");
                break;
            }
            case SessionMenuStatusKind::ReadyGuest:
                sessionMenu_.MarkSessionReady(
                    false,
                    status->message);
                break;
            case SessionMenuStatusKind::Error:
                if (!localSlot_.has_value()) {
                    ReleaseGuestMissionIsolationLease(
                        "JOIN failed before role negotiation");
                }
                sessionMenu_.SetStatus(
                    SessionOverlayPhase::Error,
                    status->message);
                break;
        }
        return;
    }
    if (!localSlot_.has_value()) {
        if (diagnostics_) {
            facade_.Log(
                "ignored pipe frame received before HelloAck");
        }
        return;
    }

    switch (frame.header.type) {
        case MessageType::MotionReplicationConfig: {
            const auto config =
                DecodeMotionReplicationConfig(frame.payload);
            if (!config.has_value()) {
                ++animationSamplesRejected_;
                facade_.Log(
                    "[ERROR][ANIMGRAPH_REPLICA] rejected invalid local motion configuration");
                break;
            }
            if (config->revision <= motionReplicationRevision_ &&
                config->mode == motionReplicationMode_ &&
                config->flags == motionReplicationFlags_) {
                break;
            }
            const bool motionModeChanged =
                config->mode != motionReplicationMode_;
            motionReplicationRevision_ = config->revision;
            motionReplicationMode_ = config->mode;
            motionReplicationFlags_ = config->flags;
            latestRemoteAnimationState_.reset();
            latestRemoteAnimationReceivedAtMs_.reset();
            latestRemoteAnimationSenderTickMs_ = 0U;
            remoteAnimationSequences_.Reset();
            remoteAnimationPayloadSequences_.Reset();
            if (motionModeChanged) {
                localAnimationSampleSequence_ = 0U;
            }
            facade_.ConfigureMotionReplication(*config);
            facade_.Log(
                motionReplicationMode_ ==
                        MotionReplicationWireMode::AnimGraphReplica
                    ? "motion replication configured: experimental AnimGraph/Direct Replica"
                    : "motion replication configured: Task/Navmesh Puppet");
            break;
        }
        case MessageType::PlayerAnimationState: {
            const auto animation =
                DecodePlayerAnimationState(frame.payload);
            if (motionReplicationMode_ !=
                    MotionReplicationWireMode::AnimGraphReplica ||
                !animation.has_value() ||
                animation->slot == *localSlot_ ||
                (remoteReplicaId_.IsValid() &&
                 animation->entityId != remoteReplicaId_)) {
                ++animationSamplesRejected_;
                if (diagnostics_) {
                    facade_.Log(
                        "rejected incompatible AnimGraph player sample");
                }
                break;
            }
            const auto disposition =
                remoteAnimationSequences_.Observe(
                    frame.header.sequence);
            if (disposition == SequenceDisposition::Duplicate ||
                disposition == SequenceDisposition::Stale) {
                break;
            }
            const auto payloadDisposition =
                remoteAnimationPayloadSequences_.Observe(
                    animation->sampleSequence);
            if (payloadDisposition == SequenceDisposition::Duplicate ||
                payloadDisposition == SequenceDisposition::Stale) {
                ++animationSamplesRejected_;
                if (diagnostics_) {
                    facade_.Log(
                        "rejected duplicate or stale AnimGraph payload sequence");
                }
                break;
            }
            latestRemoteAnimationState_ = *animation;
            latestRemoteAnimationReceivedAtMs_ = previousTickMs_;
            latestRemoteAnimationSenderTickMs_ = frame.header.tick;
            ++animationSamplesReceived_;
            break;
        }
        case MessageType::PlayerState: {
            const auto state = DecodePlayerState(frame.payload);
            if (state.has_value() && state->slot != *localSlot_) {
                ApplyRemotePlayerState(
                    *state,
                    frame.header.sequence,
                    frame.header.tick);
            }
            break;
        }
        case MessageType::PlayerTraversal: {
            const auto traversal =
                DecodePlayerTraversal(frame.payload);
            if (!traversal.has_value() ||
                traversal->slot == *localSlot_) {
                if (diagnostics_) {
                    facade_.Log(
                        "rejected invalid or local-slot traversal transaction");
                }
                break;
            }
            if (remoteReplicaId_.IsValid() &&
                traversal->entityId != remoteReplicaId_) {
                facade_.Log(
                    "rejected traversal for an unknown remote entity");
                break;
            }
            if (!remoteParticipantSceneIsolated_ &&
                !facade_.ApplyRemoteTraversal(*traversal) && diagnostics_) {
                facade_.Log(
                    "remote traversal transaction could not be queued");
            }
            break;
        }
        case MessageType::PlayerAction: {
            const auto action = DecodePlayerAction(frame.payload);
            if (action.has_value()) {
                HandleRemotePlayerAction(frame, *action);
            } else {
                ++playerActionsRejected_;
                facade_.Log(
                    "[WARNING][ACTION_TX] rejected malformed remote player action");
            }
            break;
        }
        case MessageType::PlayerIdentity: {
            const auto identity =
                DecodePlayerIdentity(frame.payload);
            if (!identity.has_value() ||
                identity->slot == *localSlot_) {
                if (diagnostics_) {
                    facade_.Log(
                        "rejected invalid or local-slot player identity");
                }
                break;
            }
            if (remoteReplicaId_.IsValid() &&
                identity->entityId != remoteReplicaId_) {
                facade_.Log(
                    "rejected player identity for an unknown remote entity");
                break;
            }
            remoteIdentity_ = *identity;
            if (remoteReplicaId_.IsValid() &&
                !remoteParticipantSceneIsolated_) {
                if (facade_.ApplyRemoteIdentity(*identity)) {
                    facade_.Log(
                        "remote player identity applied: " +
                        identity->nickname);
                } else {
                    facade_.Log(
                        "remote player identity could not be attached to the replica");
                }
            }
            break;
        }
        case MessageType::PlayerAppearanceState: {
            const auto appearance =
                DecodePlayerAppearanceState(frame.payload);
            if (!appearance.has_value() ||
                appearance->slot == *localSlot_ ||
                (remoteReplicaId_.IsValid() &&
                 appearance->entityId != remoteReplicaId_)) {
                facade_.Log(
                    "[WARNING][METAPED_APPEARANCE][RX] invalid or wrong-slot appearance rejected");
                break;
            }
            if (remoteAppearance_.has_value() &&
                appearance->entityId == remoteAppearance_->entityId &&
                appearance->revision <= remoteAppearance_->revision) {
                break;
            }
            remoteAppearance_ = *appearance;
            if (remoteReplicaId_.IsValid() &&
                !remoteParticipantSceneIsolated_) {
                if (facade_.ApplyRemoteAppearance(*appearance)) {
                    facade_.Log(
                        "[METAPED_APPEARANCE][RX] exact ordered shop-component set applied to remote player");
                } else if (diagnostics_) {
                    facade_.Log(
                        "[METAPED_APPEARANCE][RX] appearance cached; proxy/model is not ready yet");
                }
            }
            break;
        }
        case MessageType::MissionState: {
            const auto mission = DecodeMissionState(frame.payload);
            if (!mission.has_value()) {
                facade_.Log(
                    "[ERROR][MISSION_RX] malformed mission state rejected");
                break;
            }
            HandleRemoteMissionState(frame, *mission);
            break;
        }
        case MessageType::MissionCinematicState: {
            const auto cinematic =
                DecodeMissionCinematicState(frame.payload);
            if (!cinematic.has_value()) {
                facade_.Log(
                    "[ERROR][MISSION_CINEMATIC][RX] malformed state rejected");
                break;
            }
            HandleRemoteMissionCinematicState(frame, *cinematic);
            break;
        }
        case MessageType::MissionCinematicAction: {
            const auto action =
                DecodeMissionCinematicAction(frame.payload);
            if (!action.has_value()) {
                facade_.Log(
                    "[ERROR][MISSION_CINEMATIC][RX] malformed action rejected");
                break;
            }
            HandleRemoteMissionCinematicAction(frame, *action);
            break;
        }
        case MessageType::MissionCameraState: {
            const auto camera =
                DecodeMissionCameraState(frame.payload);
            if (!camera.has_value()) {
                facade_.Log(
                    "[ERROR][MISSION_CAMERA][RX] malformed mission camera state rejected");
                break;
            }
            HandleRemoteMissionCamera(frame, *camera);
            break;
        }
        case MessageType::AnimSceneReplicaState: {
            const auto scene =
                DecodeAnimSceneReplicaState(frame.payload);
            if (!scene.has_value()) {
                facade_.Log(
                    "[ERROR][ANIMSCENE_REPLICA][RX] malformed native scene state rejected");
                break;
            }
            HandleRemoteAnimSceneReplicaState(frame, *scene);
            break;
        }
        case MessageType::AnimSceneDefinition: {
            const auto definition =
                DecodeAnimSceneDefinition(frame.payload);
            if (!definition.has_value()) {
                facade_.Log(
                    "[ERROR][ANIMSCENE_HYBRID][PREPARE][RX] malformed or non-canonical definition rejected");
                break;
            }
            HandleRemoteAnimSceneDefinition(frame, *definition);
            break;
        }
        case MessageType::AnimSceneControl: {
            const auto control =
                DecodeAnimSceneControl(frame.payload);
            if (!control.has_value()) {
                facade_.Log(
                    "[ERROR][ANIMSCENE_HYBRID][CONTROL][RX] malformed control rejected");
                break;
            }
            HandleRemoteAnimSceneControl(frame, *control);
            break;
        }
        case MessageType::WorldState: {
            const auto world = DecodeWorldState(frame.payload);
            if (*localSlot_ != PlayerSlot::Guest ||
                !world.has_value()) {
                if (diagnostics_) {
                    facade_.Log(
                        "rejected non-host or invalid world state");
                }
                break;
            }
            const auto localSample = facade_.SampleLocalPlayer();
            if (soloOverride_ ||
                !localSample.has_value() ||
                localSample->cutsceneActive) {
                if (diagnostics_) {
                    facade_.Log(
                        "ignored host world state during an unsafe cutscene or override");
                }
                break;
            }
            if (!guestWorldAuthorityConfirmed_) {
                guestWorldAuthorityConfirmed_ = true;
                facade_.Log(
                    "[INFO][ENTITY_GRAPH_GUEST] host world authority confirmed; an empty host population is authoritative");
            }
            if (!facade_.ApplyWorldState(*world) &&
                diagnostics_) {
                facade_.Log(
                    "host world state could not be applied");
            }
            break;
        }
        case MessageType::EquipmentState: {
            const auto equipment =
                DecodeEquipmentState(frame.payload);
            if (!equipment.has_value() ||
                equipment->entityId == localEntityId_ ||
                (remoteReplicaId_.IsValid() &&
                 equipment->entityId != remoteReplicaId_)) {
                if (diagnostics_) {
                    facade_.Log(
                        "rejected invalid equipment state");
                }
                break;
            }
            remoteEquipment_ = *equipment;
            const bool isGuestReceivingHostEquipment =
                localSlot_ == PlayerSlot::Guest &&
                remoteReplicaId_.IsValid() &&
                equipment->entityId == remoteReplicaId_ &&
                (equipment->flags &
                 static_cast<std::uint32_t>(
                     EquipmentStateFlag::Equipped)) != 0U &&
                equipment->weaponHash != 0U;
            if (isGuestReceivingHostEquipment &&
                !facade_.UnlockLocalWeaponEntitlement(equipment->weaponHash) &&
                diagnostics_) {
                facade_.Log("host weapon shop entitlement could not be applied to guest");
            }
            if (remoteReplicaId_.IsValid() &&
                !remoteParticipantSceneIsolated_ &&
                !facade_.ApplyRemoteEquipment(*equipment) &&
                diagnostics_) {
                facade_.Log(
                    "remote equipment could not be applied");
            }
            break;
        }
        case MessageType::CampaignCapability: {
            const auto capability = DecodeCampaignCapability(frame.payload);
            if (localSlot_ != PlayerSlot::Guest || !capability.has_value()) {
                facade_.Log("[CAPABILITY] rejected non-host or malformed capability");
                break;
            }
            if (!facade_.ApplyCampaignCapability(*capability)) {
                facade_.Log("[CAPABILITY] guest capability could not be applied");
            } else {
                Frame acknowledgement;
                acknowledgement.header.type = MessageType::CampaignCapabilityAck;
                acknowledgement.header.sequence = sequencer_.Next();
                acknowledgement.header.tick = facade_.TickMilliseconds();
                acknowledgement.payload = EncodeCampaignCapabilityAck(
                    CampaignCapabilityAckPayload{capability->kind,
                        capability->recordHash, capability->hostEventId});
                SendBestEffort(std::move(acknowledgement));
            }
            break;
        }
        case MessageType::MissionProgression: {
            const auto progression = DecodeMissionProgression(frame.payload);
            if (!progression.has_value()) {
                facade_.Log("[MISSION_PROGRESSION] rejected malformed payload");
                break;
            }
            HandleRemoteMissionProgression(*progression);
            break;
        }
        case MessageType::MissionObjective: {
            const auto objective = DecodeMissionObjective(frame.payload);
            if (!objective.has_value()) {
                facade_.Log("[MISSION_OBJECTIVE] rejected malformed objective payload");
                break;
            }
            HandleRemoteMissionObjective(frame, *objective);
            break;
        }
        case MessageType::MissionDialogueCue: {
            const auto cue = DecodeMissionDialogueCue(frame.payload);
            if (!cue.has_value()) {
                facade_.Log("[MISSION_DIALOGUE] rejected malformed cue payload");
                break;
            }
            const auto disposition =
                remoteMissionDialogueCueSequences_.Observe(frame.header.sequence);
            if (disposition == SequenceDisposition::Stale ||
                disposition == SequenceDisposition::Duplicate) {
                facade_.Log("[MISSION_DIALOGUE] rejected stale cue frame");
                break;
            }
            HandleRemoteMissionDialogueCue(frame, *cue);
            break;
        }
        case MessageType::MissionDialogueReady: {
            const auto ready = DecodeMissionDialogueReady(frame.payload);
            if (!ready.has_value()) {
                facade_.Log("[MISSION_DIALOGUE] rejected malformed readiness payload");
                break;
            }
            const auto disposition =
                remoteMissionDialogueReadySequences_.Observe(frame.header.sequence);
            if (disposition == SequenceDisposition::Stale ||
                disposition == SequenceDisposition::Duplicate) {
                facade_.Log("[MISSION_DIALOGUE] rejected stale readiness frame");
                break;
            }
            HandleRemoteMissionDialogueReady(frame, *ready);
            break;
        }
        case MessageType::AmbientEncounterProposal: {
            const auto proposal = DecodeAmbientEncounterProposal(frame.payload);
            if (!proposal.has_value()) {
                facade_.Log("[AMBIENT_ENCOUNTER] rejected malformed proposal");
                break;
            }
            const auto disposition = remoteAmbientEncounterProposalSequences_.Observe(frame.header.sequence);
            if (disposition == SequenceDisposition::Duplicate || disposition == SequenceDisposition::Stale) break;
            HandleRemoteAmbientEncounterProposal(frame, *proposal);
            break;
        }
        case MessageType::AmbientEncounterState: {
            const auto state = DecodeAmbientEncounterState(frame.payload);
            if (!state.has_value()) {
                facade_.Log("[AMBIENT_ENCOUNTER] rejected malformed host state");
                break;
            }
            const auto disposition = remoteAmbientEncounterStateSequences_.Observe(frame.header.sequence);
            if (disposition == SequenceDisposition::Duplicate || disposition == SequenceDisposition::Stale) break;
            HandleRemoteAmbientEncounterState(frame, *state);
            break;
        }
        case MessageType::PlayerMountState: {
            const auto mount =
                DecodePlayerMountState(frame.payload);
            if (mount.has_value()) {
                HandleRemoteMountState(
                    frame,
                    *mount);
            } else if (diagnostics_) {
                facade_.Log(
                    "rejected invalid remote player mount state");
            }
            break;
        }
        case MessageType::PauseVote: {
            const auto pause =
                DecodePauseVote(frame.payload);
            if (pause.has_value()) {
                HandlePauseVote(frame, *pause);
            }
            break;
        }
        case MessageType::EntitySpawn: {
            const auto state =
                DecodeWorldEntityState(frame.payload);
            if (state.has_value()) {
                HandleEntitySpawn(frame, *state);
            }
            break;
        }
        case MessageType::EntityUpdate: {
            const auto state =
                DecodeWorldEntityState(frame.payload);
            if (state.has_value()) {
                HandleEntityUpdate(frame, *state);
            }
            break;
        }
        case MessageType::EntityDespawn: {
            const auto despawn =
                DecodeEntityDespawn(frame.payload);
            if (despawn.has_value()) {
                HandleEntityDespawn(
                    frame,
                    despawn->entityId);
            }
            break;
        }
        case MessageType::DamageIntent: {
            const auto intent =
                DecodeDamageIntent(frame.payload);
            if (intent.has_value()) {
                HandleDamageIntent(*intent);
            }
            break;
        }
        case MessageType::Command: {
            const auto command = DecodeCommand(frame.payload);
            if (command.has_value()) {
                bool applyWithFacade = true;
                switch (command->opcode) {
                    case CommandOpcode::SpectatorOn:
                        (void)players_.SetSpectator(
                            PlayerSlot::Guest,
                            true);
                        break;
                    case CommandOpcode::SpectatorOff:
                        (void)players_.SetSpectator(
                            PlayerSlot::Guest,
                            false);
                        break;
                    case CommandOpcode::SoloOverrideOn:
                        if (!soloOverride_) {
                            soloOverride_ = true;
                            (void)players_.SetSpectator(
                                PlayerSlot::Guest,
                                true);
                        }
                        break;
                    case CommandOpcode::SoloOverrideOff:
                        if (soloOverride_) {
                            soloOverride_ = false;
                            (void)players_.SetSpectator(
                                PlayerSlot::Guest,
                                false);
                        }
                        break;
                    case CommandOpcode::RetryCheckpoint:
                        // A host retry is meaningful on the guest only after the exact matching MissionData instance crossed the start barrier.
                        // Never send a generic checkpoint reload into a companion-only/free-roam guest: that would act on a private Story VM and defeat the host-authority invariant.
                        if (*localSlot_ != PlayerSlot::Guest ||
                            !remoteMissionStartBarrier_.has_value() ||
                            !remoteMissionStartBarrierReleased_ ||
                            remoteMissionStartBarrierRejected_) {
                            facade_.Log("[MISSION_RETRY] ignored host retry outside a released matching mission instance");
                            applyWithFacade = false;
                            break;
                        }
                        if (const auto probe = facade_.ProbeCampaignMission(
                                remoteMissionStartBarrier_->missionId);
                            !probe.has_value() ||
                            probe->missionId !=
                                remoteMissionStartBarrier_->missionId ||
                            !probe->active) {
                            facade_.Log("[MISSION_RETRY] ignored host retry because the exact guest MissionData instance is inactive");
                            applyWithFacade = false;
                            break;
                        }
                        facade_.RequestCheckpointRetry();
                        break;
                    case CommandOpcode::ToggleDiagnostics:
                        diagnostics_ = !diagnostics_;
                        break;
                    case CommandOpcode::Unload:
                        shouldUnload_ = true;
                        break;
                    case CommandOpcode::TeleportGuest:
                        HandleInboundTeleportGuest(*command);
                        applyWithFacade = false;
                        break;
                    case CommandOpcode::ResyncEquipment:
                        lastLocalEquipment_.reset();
                        nextEquipmentRefreshMs_ = 0U;
                        applyWithFacade = false;
                        break;
                    case CommandOpcode::Resync:
                        if (*localSlot_ == PlayerSlot::Host) {
                            ResetHostWorldMirror(true);
                            nextWorldMirrorSampleMs_ = 0U;
                        } else {
                            ResetGuestWorldMirror(true);
                        }
                        applyWithFacade = false;
                        break;
                    case CommandOpcode::DiagnosticMarker: {
                        applyWithFacade = false;
                        const auto correlationId = command->target.Value();
                        if (correlationId == 0U) {
                            facade_.Log(
                                "[WARNING][USER_MARKER] rejected remote diagnostic marker without correlation id");
                            break;
                        }
                        const auto now = facade_.TickMilliseconds();
                        const bool remoteFresh =
                            remoteReplicaId_.IsValid() &&
                            lastRemoteStateMs_.has_value() &&
                            DiagnosticAge(lastRemoteStateMs_, now) <=
                                kRemotePlayerFreshnessMilliseconds;
                        facade_.Log(
                            "[USER_MARKER][REMOTE] correlation=" +
                            std::to_string(correlationId) +
                            ", marker-id=" +
                            std::to_string(
                                ProblemMarkerLocalId(correlationId)) +
                            ", origin-role=" +
                            std::string{
                                ProblemMarkerOriginRole(correlationId)} +
                            ", role=" +
                            std::string{RoleName(localSlot_)} +
                            ", sender-tick=" +
                            std::to_string(frame.header.tick) +
                            ", receiver-tick=" +
                            std::to_string(now) +
                            ", remote=" +
                            (remoteFresh ? "fresh" : "stale"));
                        notificationText_ =
                            "MARKER ERROR " +
                            std::string{ProblemMarkerOriginRole(
                                correlationId)} +
                            " #" +
                            std::to_string(
                                ProblemMarkerLocalId(correlationId)) +
                            " ODEBRANY";
                        notificationUntilMs_ = now + 2'500U;
                        BeginProblemDiagnosticBurst(
                            correlationId,
                            true,
                            now);
                        nextRuntimeDiagnosticsMs_ = now;
                        EmitRuntimeDiagnostics(remoteFresh, now);
                        break;
                    }
                    default:
                        break;
                }
                if (applyWithFacade) {
                    (void)facade_.ApplyNetworkCommand(*command);
                }
                if (command->opcode ==
                        CommandOpcode::DespawnReplica &&
                    command->target == remoteReplicaId_) {
                    if (localSlot_.has_value()) {
                        playerEntityIds_[SlotIndex(
                            OtherSlot(*localSlot_))] = NetEntityId{};
                    }
                    remoteReplicaId_ = NetEntityId{};
                    latestRemoteState_.reset();
                    latestRemoteAnimationState_.reset();
                    latestRemoteAnimationReceivedAtMs_.reset();
                    latestRemoteAnimationSenderTickMs_ = 0U;
                    remoteIdentity_.reset();
                    remoteEquipment_.reset();
                    remoteLifecycle_.reset();
                    remoteRestraintState_.reset();
                    localInteraction_ = LocalInteractionRuntime{};
                    remotePlayerSequences_.Reset();
                    remoteAnimationSequences_.Reset();
                    remoteAnimationPayloadSequences_.Reset();
                    worldDamageIntentSequences_.Reset();
                    interactionResultSequences_.Reset();
                    restraintStateSequences_.Reset();
                    lastWorldDamageIntentMs_ = 0U;
                    remoteSnapshots_.Reset();
                    lastRemoteStateMs_.reset();
                    consecutiveMotionApplyFailures_ = 0U;
                }
            }
            break;
        }
        case MessageType::DownedState: {
            const auto event = DecodeDownedState(frame.payload);
            const auto slot = event.has_value()
                                  ? FindSlot(event->entityId)
                                  : std::nullopt;
            const bool authorizedSubject =
                event.has_value() &&
                slot.has_value() &&
                (*localSlot_ != PlayerSlot::Host ||
                 *slot == PlayerSlot::Guest);
            if (authorizedSubject) {
                if (event->lifecycle == PlayerLifecycle::Downed ||
                    event->lifecycle == PlayerLifecycle::Reviving) {
                    players_.SetDowned(*slot);
                } else if (
                    event->lifecycle == PlayerLifecycle::Alive) {
                    players_.SetAlive(
                        *slot,
                        event->healthFraction);
                }
            }
            break;
        }
        case MessageType::InteractionResult: {
            const auto result = DecodeInteractionResult(frame.payload);
            if (!result.has_value()) {
                break;
            }
            const auto disposition =
                interactionResultSequences_.Observe(frame.header.sequence);
            if (disposition == SequenceDisposition::Duplicate ||
                disposition == SequenceDisposition::Stale) {
                break;
            }
            HandleInteractionResult(*result);
            break;
        }
        case MessageType::RestraintState: {
            const auto state = DecodeRestraintState(frame.payload);
            if (!state.has_value()) {
                break;
            }
            const auto disposition =
                restraintStateSequences_.Observe(frame.header.sequence);
            if (disposition == SequenceDisposition::Duplicate ||
                disposition == SequenceDisposition::Stale) {
                break;
            }
            HandleRestraintState(*state);
            break;
        }
        case MessageType::InteractionIntent:
            // Intents are consumed by the host sidecar and never arrive from an authenticated peer as direct bridge authority.
            break;
        case MessageType::ReviveRequest: {
            const auto request = DecodeReviveRequest(frame.payload);
            const auto reviver =
                request.has_value()
                    ? FindSlot(request->reviverId)
                    : std::nullopt;
            const auto target =
                request.has_value()
                    ? FindSlot(request->targetId)
                    : std::nullopt;
            const bool authorizedRequest =
                reviver.has_value() &&
                target.has_value() &&
                *reviver != *target &&
                (*localSlot_ != PlayerSlot::Host ||
                 (*reviver == PlayerSlot::Guest &&
                  *target == PlayerSlot::Host));
            if (authorizedRequest) {
                pendingRevive_ = ReviveAttempt{
                    *reviver,
                    *target,
                    true,
                    facade_.HostGuestDistanceMeters().value_or(
                        std::numeric_limits<float>::infinity())};
                reviveRequestExpiresMs_ =
                    facade_.TickMilliseconds() + 150U;
            }
            break;
        }
        case MessageType::ReviveComplete: {
            if (*localSlot_ == PlayerSlot::Host) {
                break;
            }
            const auto event = DecodeReviveComplete(frame.payload);
            const auto target = event.has_value()
                                    ? FindSlot(event->targetId)
                                    : std::nullopt;
            if (event.has_value() && target.has_value()) {
                players_.SetAlive(
                    *target,
                    event->healthFraction > 0.0F
                        ? event->healthFraction
                        : kRevivedHealthFraction);
                pendingRevive_.reset();
            }
            break;
        }
        case MessageType::SpectatorState: {
            if (*localSlot_ == PlayerSlot::Host) {
                break;
            }
            const auto event = DecodeDownedState(frame.payload);
            const auto slot = event.has_value()
                                  ? FindSlot(event->entityId)
                                  : std::nullopt;
            if (event.has_value() && slot.has_value()) {
                (void)players_.SetSpectator(
                    *slot,
                    event->lifecycle == PlayerLifecycle::Spectator);
            }
            break;
        }
        case MessageType::Goodbye:
            if (*localSlot_ == PlayerSlot::Host) {
                ResetHostWorldMirror(false);
            } else {
                ResetGuestWorldMirror();
            }
            DespawnRemoteReplica();
            localInteraction_ = LocalInteractionRuntime{};
            localRestraintState_.reset();
            remoteRestraintState_.reset();
            pendingRestraintStates_.clear();
            interactionResultSequences_.Reset();
            restraintStateSequences_.Reset();
            facade_.MaintainLocalDownedState(
                false,
                kRevivedHealthFraction);
            facade_.MaintainRealtimeSession(false, false);
            transport_.Disconnect();
            nextReconnectMs_ = facade_.TickMilliseconds() + 1'000U;
            break;
        case MessageType::ResyncRequest: {
            // Resync removes and rebuilds copies once after a reconnect.
            // Calling it every frame would keep deleting NPCs before they can come back.
            // A ResyncRequest is a same-session transport boundary, not a synchronized-pause vote.
            // Both game processes already own the authoritative pause generation, and the sidecar replay plan has no PauseVote snapshot.
            // Preserve it across replica teardown so a short pipe/TCP recovery cannot split the two pause states.
            const bool preservedSynchronizedPaused = synchronizedPaused_;
            const bool preservedHostPauseVoted = hostPauseVoted_;
            const bool preservedGuestPauseVoted = guestPauseVoted_;
            const auto preservedPauseVoteGeneration =
                pauseVoteGeneration_;
            const bool awaitGuestAfterResync =
                *localSlot_ == PlayerSlot::Host &&
                localMissionCinematicState_.has_value() &&
                IsCinematicPresentationPhase(
                    localMissionCinematicState_->phase) &&
                !localCinematicResumeReady_ &&
                (remoteReplicaId_.IsValid() ||
                 awaitingRestoredGuestStream_);
            if (*localSlot_ == PlayerSlot::Host) {
                const bool preserveActiveAnimSceneGraph =
                    HasActiveHostAnimSceneDefinition();
                // ResyncRequest and the first replacement PlayerState may be delivered by one Poll.
                // Force the same recovery edge and authority dependencies even though the previous tick still considered the remote stream live.
                previousRemoteStreaming_ = false;
                nextMissionStateHeartbeatMs_ = previousTickMs_;
                nextMissionCinematicHeartbeatMs_ = previousTickMs_;
                nextMissionCameraSampleMs_ = previousTickMs_;
                nextAnimSceneSampleMs_ = previousTickMs_;
                if (!preserveActiveAnimSceneGraph) {
                    ResetHostWorldMirror(true);
                    nextWorldMirrorSampleMs_ = 0U;
                } else {
                    forceHostWorldMirrorReplay_ = true;
                    (void)BeginHostAnimScenePrepareAttempt(
                        "entity-resync");
                    facade_.Log(
                        "[ANIMSCENE_HYBRID][RESYNC] retained the active host world graph and scheduled stable spawn replay so cached role NetEntityIds remain stable across reconnect");
                }
            } else {
                ResetGuestWorldMirror(true);
            }
            DespawnRemoteReplica();
            synchronizedPaused_ = preservedSynchronizedPaused;
            hostPauseVoted_ = preservedHostPauseVoted;
            guestPauseVoted_ = preservedGuestPauseVoted;
            pauseVoteGeneration_ = preservedPauseVoteGeneration;
            awaitingRestoredGuestStream_ = awaitGuestAfterResync;
            break;
        }
        default:
            break;
    }
}

// Turns an in-game session-menu click into a safe request for the Sidecar.
// The Bridge owns the UI; the Sidecar owns actual LAN session creation.
void BridgeRuntime::HandleSessionOverlayAction(
    const SessionOverlayAction action) {
    if (action == SessionOverlayAction::StopSession) {
        if (!localSlot_.has_value() &&
            !guestMissionIsolationLeaseActive_) {
            sessionMenu_.SetStatus(
                SessionOverlayPhase::Error,
                "There is no active session to stop.");
            return;
        }
        HandleMenuCommand(BridgeCommand::StopSession);
        return;
    }
    if (action != SessionOverlayAction::Host &&
        action != SessionOverlayAction::JoinFromClipboard) {
        return;
    }
    if (localSlot_.has_value()) {
        if (*localSlot_ == PlayerSlot::Host &&
            action == SessionOverlayAction::Host &&
            !hostInviteCode_.empty()) {
            const bool copied =
                facade_.WriteClipboardText(hostInviteCode_);
            sessionMenu_.MarkSessionReady(
                true,
                copied
                    ? "The host code was copied to the clipboard again."
                    : "The host code could not be copied again.");
            return;
        }
        sessionMenu_.SetStatus(
            SessionOverlayPhase::Error,
            "A session is already active. Restart the test to change roles.");
        return;
    }
    if (!transport_.IsConnected()) {
        sessionMenu_.SetStatus(
            SessionOverlayPhase::Error,
            "Sidecar unavailable. Start the game from the launcher.");
        return;
    }

    std::string inviteCode;
    SessionMenuAction wireAction{SessionMenuAction::Host};
    if (action == SessionOverlayAction::JoinFromClipboard) {
        const auto joinSample = facade_.SampleLocalPlayer();
        if (!joinSample.has_value() ||
            joinSample->missionActive ||
            joinSample->cutsceneActive ||
            joinSample->downed) {
            sessionMenu_.SetStatus(
                SessionOverlayPhase::Error,
                "GUEST: load a safe save outside a mission and cutscene, then join again.");
            facade_.Log(
                "[WARNING][MISSION_PREFLIGHT] guest join rejected: local save is not in a safe campaign-shell state");
            return;
        }
        facade_.Log(
            "[MISSION_PREFLIGHT] guest local save accepted: no active mission/cutscene/downed state");
        wireAction = SessionMenuAction::JoinFromClipboard;
        inviteCode = facade_.ReadClipboardText();
        if (inviteCode.empty()) {
            sessionMenu_.SetStatus(
                SessionOverlayPhase::Error,
                "No R2C1 code was found in the clipboard. Copy a fresh host code.");
            return;
        }
        sessionMenu_.SetStatus(
            SessionOverlayPhase::StartingGuest,
            "Checking the code and connecting to the host...");
    } else {
        ReleaseGuestMissionIsolationLease(
            "user selected HOST instead of pending JOIN");
        sessionMenu_.SetStatus(
            SessionOverlayPhase::StartingHost,
            "Creating a private LAN session...");
    }

    try {
        Frame frame;
        frame.header.type = MessageType::SessionMenuRequest;
        frame.header.sequence = sequencer_.Next();
        frame.header.tick = facade_.TickMilliseconds();
        frame.payload =
            EncodeSessionMenuRequest(wireAction, inviteCode);
        if (wireAction == SessionMenuAction::JoinFromClipboard) {
            const auto isolation =
                AcquireGuestMissionIsolationLease(
                    "safe JOIN preflight passed");
            if (isolation.localMissionDetected ||
                isolation.quarantineActive) {
                ReleaseGuestMissionIsolationLease(
                    "JOIN preflight raced a local Story transition");
                sessionMenu_.SetStatus(
                    SessionOverlayPhase::Error,
                    "GUEST: a local mission started while joining. Load a safe save and try again.");
                facade_.Log(
                    "[WARNING][MISSION_PREFLIGHT] JOIN rejected because the vanilla mission gate was already active at lease acquisition");
                return;
            }
        }
        SendBestEffort(std::move(frame));
    } catch (const std::exception&) {
        if (!localSlot_.has_value()) {
            ReleaseGuestMissionIsolationLease(
                "JOIN request encoding failed");
        }
        sessionMenu_.SetStatus(
            SessionOverlayPhase::Error,
            "The clipboard code is too long or invalid.");
    }
}

void BridgeRuntime::HandleMenuCommand(const BridgeCommand command) {
    if (command == BridgeCommand::DisarmMissionProgression) {
        if (localSlot_ != PlayerSlot::Host || !transport_.IsConnected()) {
            facade_.Log("[MISSION_PROGRESSION] clear rejected: host session is not ready");
            return;
        }
        localMissionProgressionOffer_.reset();
        localMissionStartBarrier_.reset();
        localMissionStartBarrierDeadlineMs_ = 0U;
        localMissionStartBarrierGuestStarted_ = false;
        guestMissionProgressionEligible_ = false;
        localProgressionMissionId_ = 0U;
        facade_.Log("[MISSION_PROGRESSION] armed progression cleared; companion-only mode retained");
        return;
    }
    if (command == BridgeCommand::ArmHunt1MissionProgression ||
        command == BridgeCommand::ArmFud1MissionProgression) {
        if (localSlot_ != PlayerSlot::Host || !transport_.IsConnected() ||
            !localMissionInitialized_) {
            facade_.Log("[MISSION_PROGRESSION] automatic campaign flow is waiting for a ready host session");
            return;
        }
        facade_.Log(command == BridgeCommand::ArmHunt1MissionProgression
            ? "[MISSION_PROGRESSION] automatic all-campaign detection is enabled; start any available Story mission"
            : "[MISSION_DIALOGUE] experimental host cues publish only for source-mapped roots; all other lines retain vanilla local audio");
        return;
    }
    if (command == BridgeCommand::SkipCutscene) {
        RequestCutsceneSkip(facade_.TickMilliseconds());
        return;
    }
    if (command == BridgeCommand::EmergencyRecover) {
        if (!localSlot_.has_value() || !localEntityId_.IsValid()) {
            facade_.Log(
                "[WARNING][EMERGENCY_RECOVER] session identity is not ready");
            return;
        }
        InteractionIntentPayload intent;
        intent.actorEntityId = localEntityId_;
        intent.targetEntityId = localEntityId_;
        intent.interactionId = AdvanceNonZero(localInteractionId_);
        localInteractionId_ = intent.interactionId;
        intent.revision = 1U;
        intent.actorSlot = *localSlot_;
        intent.kind = InteractionKind::EmergencyRecover;
        intent.phase = InteractionIntentPhase::Begin;
        intent.flags = static_cast<std::uint8_t>(
            InteractionIntentFlag::TargetPlayer);
        try {
            Frame frame;
            frame.header.type = MessageType::InteractionIntent;
            frame.header.sequence = sequencer_.Next();
            frame.header.tick = facade_.TickMilliseconds();
            frame.payload = EncodeInteractionIntent(intent);
            SendBestEffort(std::move(frame));
            facade_.Log(
                "[INTERACTION_TX] emergency local recovery requested");
        } catch (const std::exception&) {
            facade_.Log(
                "[ERROR][EMERGENCY_RECOVER] request encoding failed");
        }
        return;
    }
    if (command == BridgeCommand::SaveProblemMarker) {
        const auto now = facade_.TickMilliseconds();
        userProblemMarkerId_ = AdvanceNonZero(userProblemMarkerId_);
        const MissionStatePayload* mission{};
        if (localSlot_ == PlayerSlot::Host &&
            localMissionState_.has_value()) {
            mission = &*localMissionState_;
        } else if (localSlot_ == PlayerSlot::Guest &&
                   remoteMissionState_.has_value()) {
            mission = &*remoteMissionState_;
        }
        const bool remoteFresh =
            remoteReplicaId_.IsValid() &&
            lastRemoteStateMs_.has_value() &&
            DiagnosticAge(lastRemoteStateMs_, now) <=
                kRemotePlayerFreshnessMilliseconds;
        const auto markerSlot =
            localSlot_.value_or(PlayerSlot::Host);
        const auto correlationId = MakeProblemMarkerCorrelationId(
            markerSlot,
            userProblemMarkerId_,
            now);
        facade_.Log(
            "[USER_MARKER] id=" +
            std::to_string(userProblemMarkerId_) +
            ", correlation=" + std::to_string(correlationId) +
            ", tick=" + std::to_string(now) +
            ", role=" + std::string{RoleName(localSlot_)} +
            ", mission-epoch=" +
            std::to_string(
                mission != nullptr ? mission->missionEpoch : 0U) +
            ", mission-revision=" +
            std::to_string(
                mission != nullptr ? mission->revision : 0U) +
            ", mission-phase=" +
            std::string{MissionPhaseName(
                mission != nullptr
                    ? mission->phase
                    : MissionPhase::Idle)} +
            ", remote=" + (remoteFresh ? "fresh" : "stale"));
        if (localSlot_.has_value() && transport_.IsConnected()) {
            Frame frame;
            frame.header.type = MessageType::Command;
            frame.header.sequence = sequencer_.Next();
            frame.header.tick = now;
            frame.payload = EncodeCommand(
                CommandPayload{
                    CommandOpcode::DiagnosticMarker,
                    0U,
                    NetEntityId{correlationId},
                    hasPreviousLocalTransform_
                        ? previousLocalPosition_
                        : Vec3{},
                    hasPreviousLocalTransform_
                        ? previousLocalHeading_
                        : 0.0F,
                    static_cast<float>(userProblemMarkerId_)});
            SendBestEffort(std::move(frame));
        }
        notificationText_ =
            "MARKER ERROR #" +
            std::to_string(userProblemMarkerId_) +
            " SAVED";
        notificationUntilMs_ = now + 2'500U;
        BeginProblemDiagnosticBurst(
            correlationId,
            false,
            now);
        nextRuntimeDiagnosticsMs_ = now;
        EmitRuntimeDiagnostics(remoteFresh, now);
        return;
    }
    if (command == BridgeCommand::StopSession) {
        ResetPauseVoteState(false);
        facade_.MaintainRealtimeSession(false, false);
        facade_.MaintainCutsceneSkipInput(false);
        facade_.MaintainReplicatedMissionCamera(false, std::nullopt);
        facade_.MaintainMissionSpectator(false);
        facade_.MaintainMissionResumeBarrier(false);
        facade_.MaintainRemoteMissionParticipant(false);
        localCutsceneSkipUntilMs_ = 0U;
        localCutsceneSkipVoteUntilMs_ = 0U;
        remoteCutsceneSkipVoteUntilMs_ = 0U;
        guestQuarantineSkipUntilMs_ = 0U;
        guestPostCinematicSkipUntilMs_ = 0U;
        guestMissionQuarantineSkipLatched_ = false;
        localCinematicTerminalLatchActive_ = false;
        localCinematicTerminalClearSinceMs_ = 0U;
        cutsceneSpectator_ = false;
        Frame frame;
        frame.header.type = MessageType::SessionMenuRequest;
        frame.header.sequence = sequencer_.Next();
        frame.header.tick = facade_.TickMilliseconds();
        frame.payload = EncodeSessionMenuRequest(
            SessionMenuAction::StopSession);
        SendBestEffort(std::move(frame));
        sessionMenu_.SetStatus(
            SessionOverlayPhase::ChooseMode,
            "Stopping the co-op session...");
        facade_.Log(
            "[INFO][SESSION] F8/F9 reusable stop request sent to the sidecar");
        return;
    }
    if (command == BridgeCommand::GrantTestPistol ||
        command == BridgeCommand::GrantTestLasso ||
        command == BridgeCommand::ProbeRepeatingShotgunShopUnlock ||
        command == BridgeCommand::EnableRepeatingShotgunShopUnlock ||
        command == BridgeCommand::ProbePoisonThrowingKnifePamphlet ||
        command == BridgeCommand::EnablePoisonThrowingKnifePamphlet) {
        const bool executed = facade_.ExecuteCommand(command);
        if (!executed) {
            facade_.Log(command == BridgeCommand::ProbeRepeatingShotgunShopUnlock
                            ? "[ERROR][SHOP_UNLOCK] Repeating Shotgun unlock probe failed"
                        : command == BridgeCommand::EnableRepeatingShotgunShopUnlock
                            ? "[ERROR][SHOP_UNLOCK] Repeating Shotgun unlock enable test failed"
                        : command == BridgeCommand::ProbePoisonThrowingKnifePamphlet
                            ? "[ERROR][RECIPE_UNLOCK] Poison Throwing Knife pamphlet probe failed"
                        : command == BridgeCommand::EnablePoisonThrowingKnifePamphlet
                            ? "[ERROR][RECIPE_UNLOCK] Poison Throwing Knife pamphlet enable test failed"
                        : command == BridgeCommand::GrantTestLasso
                            ? "[ERROR][TEST_WEAPON] failed to grant the test lasso"
                            : "[ERROR][TEST_WEAPON] failed to grant the test pistol");
        } else if (localSlot_ == PlayerSlot::Host &&
                   (command == BridgeCommand::EnableRepeatingShotgunShopUnlock ||
                    command == BridgeCommand::EnablePoisonThrowingKnifePamphlet)) {
            const auto recordHash =
                command == BridgeCommand::EnableRepeatingShotgunShopUnlock
                    ? 1'674'213'418U
                    : 0x366089E7U;
            const auto kind =
                command == BridgeCommand::EnableRepeatingShotgunShopUnlock
                    ? CampaignCapabilityKind::WeaponShopEligibility
                    : CampaignCapabilityKind::Recipe;
            // The journal persists beyond a bridge process.
            // A simple counter would restart at one after each game launch and could be mistaken for an already-applied grant.
            // Keep a per-millisecond sequence in the low bits and use wall-clock milliseconds as the restart-safe prefix.
            const auto grantedAtUnixMilliseconds = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            const auto sequence = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(localCapabilityEventId_) + 1U);
            localCapabilityEventId_ =
                (static_cast<std::uint64_t>(grantedAtUnixMilliseconds) << 16U) |
                static_cast<std::uint64_t>(sequence == 0U ? 1U : sequence);
            Frame frame;
            frame.header.type = MessageType::CampaignCapability;
            frame.header.sequence = sequencer_.Next();
            frame.header.tick = facade_.TickMilliseconds();
            frame.payload = EncodeCampaignCapability(CampaignCapabilityPayload{
                kind, recordHash, localCapabilityEventId_,
                grantedAtUnixMilliseconds});
            SendBestEffort(std::move(frame));
            facade_.Log("[CAPABILITY] host developer entitlement emitted");
        }
        return;
    }
    if (command == BridgeCommand::ToggleSoloTest ||
        command == BridgeCommand::ToggleGhostRecord ||
        command == BridgeCommand::ToggleGhostReplay ||
        command == BridgeCommand::ToggleGuestWorldView) {
        if (command == BridgeCommand::ToggleGuestWorldView) {
            constexpr auto kSyntheticTest = static_cast<std::uint32_t>(
                PlayerStateFlag::SyntheticTest);
            const bool syntheticPeer =
                latestRemoteState_.has_value() &&
                (latestRemoteState_->flags & kSyntheticTest) != 0U;
            if (!soloGuestWorldViewEnabled_ && !syntheticPeer) {
                facade_.Log(
                    "[GUEST_WORLD_VIEW] rejected: start SOLO TEST and Live mirror first");
                return;
            }
            soloGuestWorldViewEnabled_ =
                !soloGuestWorldViewEnabled_;
            if (!soloGuestWorldViewEnabled_) {
                ResetGuestWorldMirror();
            } else {
                guestWorldMirrorActive_ = true;
            }
        }
        const auto action =
            command == BridgeCommand::ToggleGhostRecord
                ? SessionMenuAction::ToggleGhostRecord
                : command == BridgeCommand::ToggleGhostReplay
                      ? SessionMenuAction::ToggleGhostReplay
                      : command == BridgeCommand::ToggleGuestWorldView
                            ? SessionMenuAction::ToggleGuestWorldView
                      : SessionMenuAction::ToggleSoloTest;
        Frame frame;
        frame.header.type = MessageType::SessionMenuRequest;
        frame.header.sequence = sequencer_.Next();
        frame.header.tick = facade_.TickMilliseconds();
        frame.payload = EncodeSessionMenuRequest(action);
        SendBestEffort(std::move(frame));
        facade_.Log(command == BridgeCommand::ToggleGhostRecord
                        ? "[INFO][GHOST_RECORD] F9 toggle request sent to the sidecar"
                    : command == BridgeCommand::ToggleGhostReplay
                        ? "[INFO][GHOST_REPLAY] F9 toggle request sent to the sidecar"
                    : command == BridgeCommand::ToggleGuestWorldView
                        ? (soloGuestWorldViewEnabled_
                               ? "[INFO][GUEST_WORLD_VIEW] co-op population layer enabled"
                               : "[INFO][GUEST_WORLD_VIEW] host population layer restored")
                        : "[INFO][SOLO_TEST] F9 toggle request sent to the sidecar");
        return;
    }
    if (command == BridgeCommand::TeleportToPlayer) {
        HandleTeleportToPlayerRequest();
        return;
    }
    if (command == BridgeCommand::TeleportGuest) {
        HandleTeleportGuestRequest();
        return;
    }

    bool handledLocally = false;
    if (command == BridgeCommand::ToggleSoloOverride) {
        soloOverride_ = !soloOverride_;
        HandlePlayerSignals(
            players_.SetSpectator(PlayerSlot::Guest, soloOverride_));
    } else if (command == BridgeCommand::ResyncEntities) {
        // The F9 button uses the same repair code: clear local NPC copies, then ask the other PC to rebuild its side.
        if (localSlot_ == PlayerSlot::Host) {
            ResetHostWorldMirror(true);
            nextWorldMirrorSampleMs_ = 0U;
        } else {
            ResetGuestWorldMirror(true);
        }
        handledLocally = true;
    } else if (command == BridgeCommand::ResyncEquipment) {
        lastLocalEquipment_.reset();
        nextEquipmentRefreshMs_ = 0U;
        handledLocally = true;
    } else if (command == BridgeCommand::RetryCheckpoint) {
        if (localSlot_ != PlayerSlot::Host) {
            facade_.Log(
                "[MISSION_RETRY] rejected guest retry request; only the host may choose a checkpoint retry");
            return;
        }
        if (!localMissionState_.has_value()) {
            facade_.Log(
                "[MISSION_RETRY] rejected host retry request without an active host mission authority record");
            return;
        }
        facade_.Log(
            "[MISSION_RETRY] host-approved checkpoint retry requested; guest remains isolated until the host recovery transition is complete");
        facade_.RequestCheckpointRetry();
    } else if (command == BridgeCommand::ToggleDiagnostics) {
        diagnostics_ = !diagnostics_;
    }

    const bool executed =
        handledLocally ||
        facade_.ExecuteCommand(command);
    if (!executed && diagnostics_) {
        facade_.Log("game facade could not execute an F9 command");
    }

    const bool guestLocalEquipmentRefresh =
        command == BridgeCommand::ResyncEquipment &&
        localSlot_ == PlayerSlot::Guest;
    if (localSlot_.has_value() &&
        !guestLocalEquipmentRefresh) {
        Frame frame;
        frame.header.type =
            command == BridgeCommand::ResyncEntities
                ? MessageType::ResyncRequest
                : MessageType::Command;
        frame.header.sequence = sequencer_.Next();
        frame.header.tick = facade_.TickMilliseconds();
        frame.payload = EncodeCommand(
            CommandPayload{
                MenuOpcode(command, soloOverride_),
                0U,
                {},
                {},
                0.0F,
                0.0F});
        SendBestEffort(std::move(frame));
    }

    if (command == BridgeCommand::Unload) {
        shouldUnload_ = true;
    }
}

void BridgeRuntime::HandleTeleportToPlayerRequest() {
    if (!localSlot_.has_value() ||
        !localEntityId_.IsValid()) {
        facade_.Log(
            "teleport to player rejected locally: role is not ready");
        return;
    }

    const auto now = facade_.TickMilliseconds();
    if (!latestRemoteState_.has_value() ||
        !lastRemoteStateMs_.has_value() ||
        ElapsedMilliseconds(*lastRemoteStateMs_, now) >
            kRemotePlayerFreshnessMilliseconds) {
        facade_.Log(
            "teleport to player rejected locally: remote stream is stale");
        return;
    }

    const auto& remote = *latestRemoteState_;
    if (!IsFinite(remote.position) ||
        !std::isfinite(remote.heading)) {
        facade_.Log(
            "teleport to player rejected locally: remote transform is invalid");
        return;
    }

    const auto sideHeading =
        (remote.heading + 90.0F) *
        std::numbers::pi_v<float> / 180.0F;
    const Vec3 destination{
        remote.position.x +
            std::cos(sideHeading) *
                kGuestTeleportSideOffsetMeters,
        remote.position.y +
            std::sin(sideHeading) *
                kGuestTeleportSideOffsetMeters,
        remote.position.z};
    CommandPayload command{
        CommandOpcode::TeleportGuest,
        0U,
        localEntityId_,
        destination,
        remote.heading,
        0.0F};
    if (!facade_.ApplyNetworkCommand(command)) {
        facade_.Log(
            "teleport to player failed in the game facade");
        return;
    }

    ArmTeleportVerification(destination, now);
    facade_.Log(
        "teleport to player applied locally toward entity " +
        std::to_string(remote.entityId.Value()));
}

void BridgeRuntime::HandleTeleportGuestRequest() {
    if (!localSlot_.has_value() ||
        *localSlot_ != PlayerSlot::Host) {
        facade_.Log(
            "teleport guest rejected locally: only the host may request it");
        return;
    }
    if (!transport_.IsConnected()) {
        facade_.Log(
            "teleport guest rejected locally: sidecar is disconnected");
        return;
    }

    const auto guestId = playerEntityIds_[SlotIndex(PlayerSlot::Guest)];
    if (!guestId.IsValid() ||
        !remoteReplicaId_.IsValid() ||
        remoteReplicaId_ != guestId) {
        facade_.Log(
            "teleport guest rejected locally: no known guest entity");
        return;
    }

    const auto now = facade_.TickMilliseconds();
    if (!lastRemoteStateMs_.has_value() ||
        now < *lastRemoteStateMs_ ||
        now - *lastRemoteStateMs_ >
            kRemotePlayerFreshnessMilliseconds) {
        facade_.Log(
            "teleport guest rejected locally: guest stream is stale");
        return;
    }

    const auto host = facade_.SampleLocalPlayer();
    if (!host.has_value() ||
        !IsFinite(host->position) ||
        !std::isfinite(host->heading)) {
        facade_.Log(
            "teleport guest rejected locally: host transform is unavailable");
        return;
    }

    const auto headingRadians =
        host->heading * std::numbers::pi_v<float> / 180.0F;
    const Vec3 destination{
        host->position.x +
            std::cos(headingRadians) *
                kGuestTeleportSideOffsetMeters,
        host->position.y +
            std::sin(headingRadians) *
                kGuestTeleportSideOffsetMeters,
        host->position.z};
    if (!IsFinite(destination)) {
        facade_.Log(
            "teleport guest rejected locally: destination is invalid");
        return;
    }

    Frame frame;
    frame.header.type = MessageType::Command;
    frame.header.sequence = sequencer_.Next();
    frame.header.tick = now;
    frame.payload = EncodeCommand(
        CommandPayload{
            CommandOpcode::TeleportGuest,
            0U,
            guestId,
            destination,
            host->heading,
            0.0F});
    SendBestEffort(std::move(frame));
    facade_.Log(
        "teleport guest queued for entity " +
        std::to_string(guestId.Value()) +
        " at (" +
        std::to_string(destination.x) + ", " +
        std::to_string(destination.y) + ", " +
        std::to_string(destination.z) +
        "), heading " +
        std::to_string(host->heading));
}

void BridgeRuntime::HandleInboundTeleportGuest(
    const CommandPayload& command) {
    if (!localSlot_.has_value() ||
        *localSlot_ != PlayerSlot::Guest) {
        facade_.Log(
            "teleport guest command rejected: local player is not the guest");
        return;
    }
    if (!localEntityId_.IsValid() ||
        command.target != localEntityId_) {
        facade_.Log(
            "teleport guest command rejected: target does not match local guest");
        return;
    }
    if (!IsFinite(command.position) ||
        !std::isfinite(command.heading)) {
        facade_.Log(
            "teleport guest command rejected: transform is invalid");
        return;
    }

    const bool hostSceneTransition =
        (remoteMissionCinematicState_.has_value() &&
         IsCinematicPresentationPhase(
             remoteMissionCinematicState_->phase)) ||
        (remoteMissionState_.has_value() &&
         (remoteMissionState_->phase == MissionPhase::Cutscene ||
          remoteMissionState_->phase == MissionPhase::Loading ||
          remoteMissionState_->phase == MissionPhase::Recovery ||
          remoteMissionState_->phase == MissionPhase::SoloOverride));
    const bool hostMissionProtocolActive =
        remoteMissionState_.has_value() &&
        (remoteMissionState_->flags &
         static_cast<std::uint8_t>(
             MissionStateFlag::MissionActive)) != 0U;
    if (!guestMissionIsolationLeaseActive_) {
        facade_.Log(
            "teleport guest command rejected: guest mission-isolation lease is not active");
        return;
    }
    const auto preTeleportIsolation =
        facade_.MaintainMissionAuthority(
            true,
            hostMissionProtocolActive,
            hostSceneTransition);
    guestMissionQuarantineActive_ =
        guestMissionQuarantineActive_ ||
        preTeleportIsolation.quarantineActive;
    if (guestMissionQuarantineActive_ ||
        cutsceneSpectator_ ||
        hostSceneTransition) {
        pendingMissionGuestTeleport_ = command;
        facade_.Log(
            "[MISSION_ANCHOR][MISSION_ISOLATION] guest teleport deferred until local mission quarantine and spectator presentation are both released");
        return;
    }

    if (facade_.ApplyNetworkCommand(command)) {
        ArmTeleportVerification(
            command.position,
            facade_.TickMilliseconds());
        facade_.Log(
            "teleport guest command applied to local guest entity " +
            std::to_string(localEntityId_.Value()));
    } else {
        facade_.Log(
            "teleport guest command failed in the game facade for local guest entity " +
            std::to_string(localEntityId_.Value()));
    }
}

void BridgeRuntime::ArmTeleportVerification(
    const Vec3& destination,
    const std::uint64_t requestedAtMs) noexcept {
    if (!IsFinite(destination)) {
        return;
    }
    pendingTeleportDestination_ = destination;
    pendingTeleportRequestedAtMs_ = requestedAtMs;
}

void BridgeRuntime::VerifyPendingTeleport(
    const std::uint64_t nowMs) {
    if (!pendingTeleportDestination_.has_value() ||
        ElapsedMilliseconds(
            pendingTeleportRequestedAtMs_,
            nowMs) <
            kTeleportVerificationDelayMilliseconds) {
        return;
    }

    const auto expected = *pendingTeleportDestination_;
    pendingTeleportDestination_.reset();
    const auto sample = facade_.SampleLocalPlayer();
    if (!sample.has_value() ||
        !IsFinite(sample->position)) {
        facade_.Log(
            "teleport postcondition unavailable: local transform could not be sampled");
        return;
    }

    const bool unsafeLanding =
        sample->inWater || sample->swimming ||
        sample->swimmingUnderwater || sample->falling ||
        sample->ragdoll;
    const auto error = Distance(sample->position, expected);
    if (!unsafeLanding && std::isfinite(error) &&
        error <= kTeleportVerificationToleranceMeters) {
        facade_.Log(
            "teleport confirmed after native apply");
    } else if (unsafeLanding) {
        facade_.Log(
            "[MISSION_ANCHOR][UNSAFE_LANDING] teleport reached an unsafe local state; keep recovery diagnostics active");
    } else {
        facade_.Log(
            "teleport was overridden by the game; position error " +
            std::to_string(error) + " m");
    }
}

// Converts local RDR2 downed/revive/respawn signals into shared lifecycle messages, then applies matching remote lifecycle changes to the local proxy.
void BridgeRuntime::HandlePlayerSignals(
    const std::span<const PlayerRuntimeSignal> signals) {
    for (const auto& signal : signals) {
        switch (signal.kind) {
            case PlayerRuntimeSignalKind::ReviveStarted:
                // The request already arrived from the sidecar.
                // Do not echo it back and create an IPC feedback loop.
                break;
            case PlayerRuntimeSignalKind::ReviveCancelled:
                pendingRevive_.reset();
                SendLifecycleState(
                    MessageType::DownedState,
                    signal.subject,
                    PlayerLifecycle::Downed,
                    0.0F);
                break;
            case PlayerRuntimeSignalKind::ReviveCompleted:
                pendingRevive_.reset();
                SendReviveCompleted(
                    signal.subject,
                    signal.value);
                break;
            case PlayerRuntimeSignalKind::SpectatorEntered:
                SendLifecycleState(
                    MessageType::SpectatorState,
                    signal.subject,
                    PlayerLifecycle::Spectator,
                    players_.State(signal.subject).healthFraction);
                break;
            case PlayerRuntimeSignalKind::SpectatorExited:
                SendLifecycleState(
                    MessageType::SpectatorState,
                    signal.subject,
                    players_.State(signal.subject).lifecycle,
                    players_.State(signal.subject).healthFraction);
                break;
        }
    }
}

void BridgeRuntime::SendLifecycleState(
    const MessageType type,
    const PlayerSlot subject,
    const PlayerLifecycle lifecycle,
    const float healthFraction) {
    const auto entityId = playerEntityIds_[SlotIndex(subject)];
    if (!entityId.IsValid()) {
        if (diagnostics_) {
            facade_.Log(
                "cannot send lifecycle state before NetEntityId is known");
        }
        return;
    }
    Frame frame;
    frame.header.type = type;
    frame.header.sequence = sequencer_.Next();
    frame.header.tick = facade_.TickMilliseconds();
    frame.payload = EncodeDownedState(
        DownedStatePayload{
            entityId,
            lifecycle,
            std::clamp(healthFraction, 0.0F, 1.0F)});
    SendBestEffort(std::move(frame));
}

void BridgeRuntime::SendReviveCompleted(
    const PlayerSlot target,
    const float healthFraction) {
    const auto reviverId =
        playerEntityIds_[SlotIndex(OtherSlot(target))];
    const auto targetId = playerEntityIds_[SlotIndex(target)];
    if (!reviverId.IsValid() || !targetId.IsValid()) {
        if (diagnostics_) {
            facade_.Log(
                "cannot send revive completion before both NetEntityIds are known");
        }
        return;
    }
    Frame frame;
    frame.header.type = MessageType::ReviveComplete;
    frame.header.sequence = sequencer_.Next();
    frame.header.tick = facade_.TickMilliseconds();
    frame.payload = EncodeReviveComplete(
        ReviveCompletePayload{
            reviverId,
            targetId,
            std::clamp(healthFraction, 0.0F, 1.0F)});
    SendBestEffort(std::move(frame));
}

std::optional<PlayerSlot> BridgeRuntime::FindSlot(
    const NetEntityId id) const noexcept {
    if (!id.IsValid()) {
        return std::nullopt;
    }
    if (id == playerEntityIds_[0]) {
        return PlayerSlot::Host;
    }
    if (id == playerEntityIds_[1]) {
        return PlayerSlot::Guest;
    }
    return std::nullopt;
}

bool BridgeRuntime::SendBestEffort(Frame frame) {
    if (!transport_.IsConnected()) {
        return false;
    }
    std::string error;
    const bool delivered = transport_.Send(frame, error);
    if (!delivered) {
        if (!error.empty()) {
            facade_.Log(error);
        }
        nextReconnectMs_ = facade_.TickMilliseconds() + 1'000U;
        return false;
    }
    return true;
}

void BridgeRuntime::Stop(const std::string_view reason) noexcept {
    runtimeDiagnosticTickCount_ = 0U;
    runtimeDiagnosticTickElapsedSumMs_ = 0U;
    runtimeDiagnosticHitchCount_ = 0U;
    runtimeDiagnosticTickMaximumMs_ = 0U;
    previousQuickMarkerPressed_ = false;
    notificationUntilMs_ = 0U;
    notificationText_.clear();
    problemDiagnosticCorrelationId_ = 0U;
    problemDiagnosticUntilMs_ = 0U;
    nextProblemDiagnosticSnapshotMs_ = 0U;
    problemDiagnosticSampleIndex_ = 0U;
    problemDiagnosticRemoteOrigin_ = false;
    if (!active_) {
        return;
    }
    facade_.ClearHostMissionDialoguePresentation();
    try {
        if (localSlot_ == PlayerSlot::Host &&
            localMissionCinematicState_.has_value() &&
            IsCinematicPresentationPhase(
                localMissionCinematicState_->phase)) {
            const auto flags = static_cast<std::uint16_t>(
                localMissionCinematicState_->flags &
                static_cast<std::uint16_t>(
                    MissionCinematicStateFlag::AnchorValid));
            PublishMissionCinematicState(
                MissionCinematicPhase::Aborted,
                flags,
                localMissionCinematicState_->resumeAnchor,
                localMissionCinematicState_->resumeHeading,
                facade_.TickMilliseconds());
        }
        if (localSlot_ == PlayerSlot::Host) {
            ResetHostWorldMirror(true);
        } else {
            ResetGuestWorldMirror();
        }
        DespawnRemoteReplica();
        Frame frame;
        frame.header.type = MessageType::Goodbye;
        frame.header.sequence = sequencer_.Next();
        frame.header.tick = facade_.TickMilliseconds();
        const auto length = std::min<std::size_t>(
            reason.size(),
            kMaximumFramePayload);
        frame.payload.assign(reason.begin(), reason.begin() + length);
        SendBestEffort(std::move(frame));
    } catch (...) {
        // Unload must remain noexcept even if allocation fails.
    }
    transport_.Disconnect();
    pendingHostWorldDespawns_.clear();
    hostWorldReplayAwaitingGuest_ = false;
    hostWorldReplayGuestDeadlineMs_ = 0U;
    localInteraction_ = LocalInteractionRuntime{};
    localRestraintState_.reset();
    remoteRestraintState_.reset();
    pendingRestraintStates_.clear();
    interactionResultSequences_.Reset();
    restraintStateSequences_.Reset();
    facade_.MaintainLocalDownedState(false, kRevivedHealthFraction);
    ResetGuestWorldMirror();
    pendingMissionGuestTeleport_.reset();
    guestMissionQuarantineActive_ = false;
    guestQuarantineSkipUntilMs_ = 0U;
    guestPostCinematicSkipUntilMs_ = 0U;
    guestMissionQuarantineSkipLatched_ = false;
    remoteParticipantSceneIsolated_ = false;
    localMissionCinematicState_.reset();
    remoteMissionCinematicState_.reset();
    remoteMissionCameraState_.reset();
    remoteAnimSceneState_.reset();
    remoteMissionCameraReceivedAtMs_.reset();
    remoteMissionCinematicReceivedAtMs_.reset();
    remoteAnimSceneReceivedAtMs_.reset();
    remoteNativeAnimSceneActive_ = false;
    ResetAnimSceneHybridState(true);
    localAnimSceneControlActionId_ = 0U;
    localCutsceneSkipUntilMs_ = 0U;
    localCutsceneSkipVoteUntilMs_ = 0U;
    remoteCutsceneSkipVoteUntilMs_ = 0U;
    localCinematicTerminalLatchActive_ = false;
    localCinematicTerminalClearSinceMs_ = 0U;
    localCinematicCameraWaitStartedMs_ = 0U;
    localCinematicCameraWaitWarned_ = false;
    localCinematicCameraReady_ = false;
    active_ = false;
    ReleaseGuestMissionIsolationLease("bridge unload/stop");
    facade_.MaintainMissionCompanionPresentation({});
    facade_.MaintainReplicatedMissionCamera(false, std::nullopt);
    (void)facade_.MaintainReplicatedAnimScene(false, std::nullopt);
    facade_.MaintainMissionSpectator(false);
    facade_.MaintainMissionResumeBarrier(false);
    facade_.MaintainCutsceneSkipInput(false);
    facade_.MaintainRemoteMissionParticipant(false);
    facade_.MaintainRealtimeSession(false, false);
    facade_.Log("CoopStory bridge stopped");
}

}  // namespace coopstory::bridge
