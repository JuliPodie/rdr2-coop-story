#pragma once

#include "coopstory/bridge/AnimationReplicationCodec.hpp"
#include "coopstory/bridge/Domain.hpp"
#include "coopstory/bridge/FrameCodec.hpp"
#include "coopstory/bridge/MenuController.hpp"
#include "coopstory/bridge/SessionMenuController.hpp"
#include "coopstory/bridge/VersionGate.hpp"
#include "coopstory/bridge/WorldMirror.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace coopstory::bridge {

struct LocalMountSample final {
    LocalEntityHandle localHandle{};
    NetEntityId sharedEntityId{};
    std::uint32_t modelHash{};
    std::uint32_t sharedGeneration{};
    Vec3 position{};
    Vec3 velocity{};
    float heading{};
    float desiredMoveBlend{};
    bool desiredMoveBlendValid{};
    float healthFraction{1.0F};
    bool mounted{};
    bool dead{};
    bool borrowedPeerMount{};
};

struct LocalPlayerSample final {
    NetEntityId entityId{};
    PlayerSlot slot{PlayerSlot::Host};
    Vec3 position{};
    Vec3 velocity{};
    float heading{};
    // RDR2 can drive scripted/camp locomotion at a custom blend while the
    // world-space velocity is temporarily zero. Preserve the game-provided
    // intent so the remote replica does not slide in an idle pose.
    float desiredMoveBlend{};
    bool desiredMoveBlendValid{};
    float healthFraction{1.0F};
    bool missionActive{};
    bool cutsceneActive{};
    bool downed{};
    bool mounted{};
    bool aiming{};
    bool firing{};
    bool meleeCombat{};
    bool meleeAttackPressed{};
    bool meleeBlocking{};
    bool meleeGrappling{};
    bool peerCombatTarget{};
    bool peerLassoIntent{};
    bool peerLassoActive{};
    bool peerKnockdown{};
    bool peerMountPull{};
    bool interactionPressed{};
    bool interactionHeld{};
    bool stealthMovement{};
    bool inCover{};
    bool goingIntoCover{};
    bool coverFacingLeft{};
    bool aimingFromCover{};
    bool jumping{};
    bool jumpPressed{};
    bool climbing{};
    bool falling{};
    bool inWater{};
    bool swimming{};
    bool swimmingUnderwater{};
    bool ragdoll{};
    bool gettingUp{};
    bool aimTargetValid{};
    Vec3 aimTarget{};
    bool reloading{};
    std::uint32_t weaponHash{};
    std::uint32_t weaponAmmo{};
    bool weaponIsLasso{};
    bool traversalObstacleValid{};
    Vec3 traversalObstaclePoint{};
    Vec3 traversalObstacleNormal{};
    float traversalObstacleTopZ{};
    std::optional<LocalMountSample> mount{};
    // Process-local identity used only while resolving captured AnimScene
    // roles. It is never copied into PlayerState or any network payload.
    LocalEntityHandle localHandle{};
};

struct BridgeHudState final {
    bool bridgeActive{};
    bool sidecarConnected{};
    std::optional<PlayerSlot> localSlot{};
    bool remoteConnected{};
    bool diagnosticsEnabled{};
    bool soloOverrideEnabled{};
};

struct PauseVoteView final {
    bool sessionActive{};
    bool paused{};
    bool hostVoted{};
    bool guestVoted{};
};

// A host mission cannot be transferred by toggling RDR2's process-local
// MISSION_FLAG.  The guest therefore keeps its own Story mission runtime idle
// and treats any unexpected local transition as a quarantined condition.
struct GuestMissionIsolationStatus final {
    bool localMissionDetected{};
    bool quarantineActive{};
    bool localStoryInteractionSuppressed{};
    // True only while a verified local mission-start prompt, a quarantined
    // local Story transition, or the host's authoritative mission owns the
    // guest. It is never asserted for the whole network lease, so unrelated
    // free-roam talk and horse prompts remain game-owned.
    bool missionGateAsserted{};
};

[[nodiscard]] constexpr bool ShouldAssertGuestMissionGate(
    const bool active,
    const bool hostMissionActive,
    const bool hostPresentationActive,
    const bool quarantineActive,
    const bool verifiedStoryPromptSuppressed) noexcept {
    return active &&
           (hostMissionActive || hostPresentationActive || quarantineActive ||
            verifiedStoryPromptSuppressed);
}

// Context inputs used to mount/talk overlap RDR2's melee/grapple controls. A
// nearby mounted peer can therefore be reported by GET_MELEE_TARGET_FOR_PED
// while the local player is actually interacting with their own horse. Only a
// clean combat edge is allowed to publish the victim-owned dismount action.
[[nodiscard]] constexpr bool ShouldPublishPeerMountPull(
    const bool remoteReplicaMounted,
    const bool peerInRange,
    const bool meleeTargetsPeer,
    const bool meleeAttackEdge,
    const bool contextInteractionActive,
    const bool ownMountInteractionNearby) noexcept {
    return remoteReplicaMounted && peerInRange && meleeTargetsPeer &&
           meleeAttackEdge && !contextInteractionActive &&
           !ownMountInteractionNearby;
}

struct MissionCameraSample final {
    Vec3 position{};
    Vec3 rotation{};
    float fieldOfView{};
    std::uint32_t flags{};
};

// Host-process capture records. These types may contain RDR2 handles, so they
// remain behind the facade boundary. BridgeRuntime converts every role to a
// stable NetEntityId before constructing AnimSceneDefinitionPayload.
struct CapturedAnimSceneRoleBinding final {
    std::string roleName{};
    LocalEntityHandle localHandle{};
    std::uint32_t modelHash{};
    AnimSceneRoleKind kind{AnimSceneRoleKind::Ped};
    std::uint16_t flags{};
    std::uint32_t bindingFlags{};
};

struct CapturedAnimSceneDefinition final {
    std::uint64_t captureSequence{};
    int localSceneHandle{};
    std::uint32_t dictionaryHash{};
    float durationSeconds{};
    std::uint32_t sceneFlags{};
    std::uint8_t createOptionFlags{};
    std::string resourceName{};
    std::string playbackList{};
    std::vector<CapturedAnimSceneRoleBinding> roles{};
    bool complete{};
};

enum class ReplicatedAnimScenePrepareStatus : std::uint8_t {
    Pending,
    Ready,
    Unsupported,
    ResourceUnavailable,
    MissingBinding,
    EntityMismatch,
    NativeFailure,
};

enum class ReplicatedAnimScenePrepareStage : std::uint8_t {
    None,
    Creating,
    WaitingForBindings,
    WaitingForResource,
    Ready,
    Failed,
};

struct ReplicatedAnimScenePrepareResult final {
    ReplicatedAnimScenePrepareStatus status{
        ReplicatedAnimScenePrepareStatus::Unsupported};
    std::uint16_t resolvedRoles{};
    std::uint16_t requiredRoles{};
    bool resourceLoaded{};
    bool cacheHit{};
    ReplicatedAnimScenePrepareStage stage{
        ReplicatedAnimScenePrepareStage::None};
    NetEntityId pendingEntityId{};
    std::string pendingRoleName{};
};

struct MissionResumePreparation final {
    bool ready{};
    bool fallbackUsed{};
};

struct MissionCompanionPresentation final {
    bool active{};
    bool liveHostPosition{};
    Vec3 target{};
    float distanceMeters{};
};

// Sanitized runtime-only measurements used by the structured diagnostics
// stream.  Handles/pointers never leave the SDK facade; the bridge receives
// only stable network ids, aggregate counts and bounded numeric errors.
struct PlayerDivergenceDiagnostic final {
    bool available{};
    float positionErrorMeters{};
    float rotationErrorDegrees{};
    std::uint8_t expectedGait{};
    std::uint8_t observedGait{};
    bool actionActive{};
    std::uint32_t activeActionId{};
    std::uint64_t actionFreshnessMilliseconds{};
};

struct EntityDivergenceDiagnostic final {
    std::size_t desiredCount{};
    std::size_t desiredScriptOwnedCount{};
    std::size_t liveCount{};
    std::size_t liveScriptOwnedCount{};
    std::size_t pendingCount{};
    std::size_t pendingScriptOwnedCount{};
    std::size_t missingCount{};
    std::size_t missingScriptOwnedCount{};
    std::size_t divergentCount{};
    std::size_t divergentScriptOwnedCount{};
    NetEntityId worstEntityId{};
    float worstPositionErrorMeters{};
    float positionErrorP95Meters{};
    NetEntityId oldestMissingEntityId{};
    std::uint64_t oldestMissingAgeMilliseconds{};
};

struct RuntimeDivergenceDiagnostics final {
    PlayerDivergenceDiagnostic player{};
    EntityDivergenceDiagnostic entities{};
};

class IScriptHookFacade {
public:
    virtual ~IScriptHookFacade() = default;

    [[nodiscard]] virtual std::uint64_t TickMilliseconds() noexcept = 0;
    [[nodiscard]] virtual RuntimeMode QueryRuntimeMode() noexcept = 0;
    [[nodiscard]] virtual std::optional<LocalPlayerSample> SampleLocalPlayer() noexcept = 0;
    [[nodiscard]] virtual std::optional<PlayerAppearanceStatePayload>
    SampleLocalAppearance(
        NetEntityId entityId,
        PlayerSlot slot,
        std::uint32_t revision) noexcept {
        (void)entityId;
        (void)slot;
        (void)revision;
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<MissionCameraSample>
    SampleMissionCamera() noexcept {
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<AnimSceneReplicaStatePayload>
    SampleHostAnimScene(
        NetEntityId hostEntityId,
        std::uint32_t missionEpoch,
        std::uint32_t cinematicGeneration,
        std::uint32_t revision) noexcept {
        (void)hostEntityId;
        (void)missionEpoch;
        (void)cinematicGeneration;
        (void)revision;
        return std::nullopt;
    }
    // Process-local identity of the scene represented by the most recent
    // successful SampleHostAnimScene result. Never serialized or cached.
    [[nodiscard]] virtual std::optional<LocalEntityHandle>
    SampledHostAnimSceneLocalHandle() noexcept {
        return std::nullopt;
    }
    [[nodiscard]] virtual std::vector<CapturedAnimSceneDefinition>
    DrainCapturedAnimSceneDefinitions() noexcept {
        return {};
    }
    [[nodiscard]] virtual std::optional<NetEntityId>
    FindKnownReplicaNetworkId(LocalEntityHandle localHandle) noexcept {
        (void)localHandle;
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<PlayerAnimationStatePayload>
    SampleLocalAnimationState(
        NetEntityId entityId,
        PlayerSlot slot,
        std::uint16_t locomotionEpoch,
        std::uint32_t sampleSequence) noexcept = 0;
    [[nodiscard]] virtual std::optional<WorldStatePayload> SampleWorldState()
        noexcept = 0;
    [[nodiscard]] virtual std::vector<HostWorldEntitySample>
    SampleWorldEntities(
        float radiusMeters,
        std::size_t maximumEntities) noexcept = 0;
    [[nodiscard]] virtual std::optional<DamageIntentPayload>
    SampleWorldDamageIntent(NetEntityId attackerId) noexcept = 0;
    [[nodiscard]] virtual std::optional<float> HostGuestDistanceMeters() noexcept = 0;
    [[nodiscard]] virtual MenuInputState ReadMenuInput() noexcept = 0;

    virtual void DrawMenu(
        bool open,
        std::span<const BridgeCommand> commands,
        std::size_t selected) noexcept = 0;
    virtual void DrawSessionMenu(
        const SessionOverlayView& state) noexcept = 0;
    virtual void DrawBridgeHud(const BridgeHudState& state) noexcept = 0;
    virtual void DrawPauseVoteStatus(
        const PauseVoteView& state) noexcept = 0;
    virtual void DrawNotification(
        std::string_view text,
        bool success) noexcept {
        (void)text;
        (void)success;
    }
    [[nodiscard]] virtual std::string ReadClipboardText() noexcept = 0;
    [[nodiscard]] virtual bool WriteClipboardText(
        std::string_view text) noexcept = 0;
    virtual void ShowMissionBubbleWarning(float distanceMeters) noexcept = 0;
    [[nodiscard]] virtual bool ExecuteCommand(BridgeCommand command) noexcept = 0;
    [[nodiscard]] virtual bool ApplyNetworkCommand(
        const CommandPayload& command) noexcept = 0;
    [[nodiscard]] virtual bool ApplyRemoteTransform(
        const PlayerStatePayload& state) noexcept = 0;
    [[nodiscard]] virtual bool ApplyRemoteAnimationState(
        const PlayerAnimationStatePayload& state) noexcept = 0;
    virtual void ConfigureMotionReplication(
        const MotionReplicationConfigPayload& config) noexcept = 0;
    virtual void SetAnimSceneCaptureAuthority(
        bool hostAuthority) noexcept {
        (void)hostAuthority;
    }
    [[nodiscard]] virtual bool ApplyRemoteTraversal(
        const PlayerTraversalPayload& traversal) noexcept = 0;
    [[nodiscard]] virtual bool ApplyRemotePlayerAction(
        const PlayerActionPayload& action) noexcept = 0;
    [[nodiscard]] virtual bool ApplyInteractionResult(
        const InteractionResultPayload& result,
        NetEntityId localEntityId) noexcept {
        (void)result;
        (void)localEntityId;
        return true;
    }
    [[nodiscard]] virtual bool ApplyRestraintState(
        const RestraintStatePayload& state,
        NetEntityId localEntityId) noexcept {
        (void)state;
        (void)localEntityId;
        return true;
    }
    virtual void MaintainLocalDownedState(
        bool active,
        float restoredHealthFraction) noexcept {
        (void)active;
        (void)restoredHealthFraction;
    }
    [[nodiscard]] virtual bool ApplyRemoteIdentity(
        const PlayerIdentityPayload& identity) noexcept = 0;
    [[nodiscard]] virtual bool ApplyRemoteAppearance(
        const PlayerAppearanceStatePayload& appearance) noexcept {
        (void)appearance;
        return false;
    }
    [[nodiscard]] virtual bool ApplyWorldState(
        const WorldStatePayload& state) noexcept = 0;
    [[nodiscard]] virtual bool ApplyRemoteEquipment(
        const EquipmentStatePayload& state) noexcept = 0;
    [[nodiscard]] virtual bool MaintainRemoteMount(
        const PlayerMountStatePayload& state,
        const std::optional<PlayerMountStatePayload>& localState) noexcept = 0;
    virtual void ClearRemoteMount() noexcept = 0;
    [[nodiscard]] virtual bool SpawnWorldEntityProxy(
        const WorldEntityStatePayload& state) noexcept = 0;
    [[nodiscard]] virtual bool UpdateWorldEntityProxy(
        const WorldEntityStatePayload& state) noexcept = 0;
    virtual void DespawnWorldEntityProxy(NetEntityId entityId) noexcept = 0;
    virtual void MaintainWorldMirrorGuest(
        bool active,
        bool authoritativePopulationReady,
        float radiusMeters) noexcept = 0;
    [[nodiscard]] virtual bool ApplyWorldEntityDamage(
        LocalEntityHandle target,
        float damage) noexcept = 0;
    // Mission actors need an attacker that exists inside the host's Story
    // Mode process.  The default keeps non-SDK facades source-compatible;
    // the real SDK facade overrides this with a single attributed bullet
    // owned by the host-side guest replica.
    [[nodiscard]] virtual bool ApplyMissionWorldEntityDamage(
        LocalEntityHandle target,
        std::uint32_t weaponHash,
        float damage) noexcept {
        (void)weaponHash;
        return ApplyWorldEntityDamage(target, damage);
    }
    virtual void MaintainRealtimeSession(
        bool active,
        bool synchronizedPaused) noexcept = 0;
    [[nodiscard]] virtual GuestMissionIsolationStatus
    MaintainMissionAuthority(
        bool active,
        bool hostMissionActive,
        bool hostPresentationActive) noexcept = 0;
    virtual void MaintainMissionSpectator(bool active) noexcept {
        (void)active;
    }
    virtual void MaintainMissionResumeBarrier(bool active) noexcept {
        (void)active;
    }
    [[nodiscard]] virtual MissionResumePreparation
    PrepareMissionCinematicResume(
        const Vec3& anchor,
        float heading,
        std::uint64_t nowMs) noexcept {
        (void)anchor;
        (void)heading;
        (void)nowMs;
        return {true, false};
    }
    [[nodiscard]] virtual bool IsCutsceneSkipPressed() noexcept {
        return false;
    }
    virtual void MaintainCutsceneSkipInput(bool active) noexcept {
        (void)active;
    }
    virtual void MaintainReplicatedMissionCamera(
        bool spectatorActive,
        const std::optional<MissionCameraStatePayload>& state) noexcept {
        (void)spectatorActive;
        (void)state;
    }
    [[nodiscard]] virtual bool MaintainReplicatedAnimScene(
        bool spectatorActive,
        const std::optional<AnimSceneReplicaStatePayload>& state) noexcept {
        (void)spectatorActive;
        (void)state;
        return false;
    }
    [[nodiscard]] virtual ReplicatedAnimScenePrepareResult
    PrepareReplicatedAnimSceneDefinition(
        const AnimSceneDefinitionPayload& definition,
        NetEntityId localEntityId) noexcept {
        (void)definition;
        (void)localEntityId;
        return {};
    }
    [[nodiscard]] virtual bool MaintainHostAnimSceneStartBarrier(
        bool active) noexcept {
        (void)active;
        return true;
    }
    [[nodiscard]] virtual bool CommitReplicatedAnimSceneDefinition(
        const AnimSceneControlPayload& commit) noexcept {
        (void)commit;
        return false;
    }
    virtual void AbortReplicatedAnimSceneDefinition() noexcept {}
    virtual void MaintainMissionCompanionPresentation(
        const MissionCompanionPresentation& state) noexcept {
        (void)state;
    }
    [[nodiscard]] virtual RuntimeDivergenceDiagnostics
    SampleRuntimeDivergenceDiagnostics() noexcept {
        return {};
    }
    // The host hides the cosmetic guest ped (and its replicated mount) while
    // vanilla Story cameras own the scene.  This is distinct from hiding the
    // guest's real local player in its spectator process.
    virtual void MaintainRemoteMissionParticipant(bool hidden) noexcept {
        (void)hidden;
    }
    virtual void RequestCheckpointRetry() noexcept = 0;
    virtual void Log(std::string_view text) noexcept = 0;
    virtual void WaitForNextTick() noexcept = 0;
};

}  // namespace coopstory::bridge
