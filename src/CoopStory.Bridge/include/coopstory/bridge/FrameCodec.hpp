#pragma once

#include "coopstory/bridge/Domain.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace coopstory::bridge {

inline constexpr std::uint32_t kFrameMagic = 0x50433252U;  // LE bytes: "R2CP"
inline constexpr std::uint16_t kProtocolVersion = 27U;
inline constexpr std::size_t kFrameHeaderSize = 24U;
inline constexpr std::uint32_t kMaximumFramePayload = 1'048'576U;
inline constexpr std::size_t kMaximumUdpDatagram = 1'200U;

enum class MessageType : std::uint16_t {
    Hello = 1,
    HelloAck = 2,
    Heartbeat = 3,
    PlayerState = 4,
    EntitySpawn = 5,
    EntityUpdate = 6,
    EntityDespawn = 7,
    DamageIntent = 8,
    DamageApplied = 9,
    DownedState = 10,
    ReviveRequest = 11,
    ReviveComplete = 12,
    MissionState = 13,
    SpectatorState = 14,
    Command = 15,
    ResyncRequest = 16,
    ResyncSnapshot = 17,
    Error = 18,
    Goodbye = 19,
    SessionMenuRequest = 20,
    SessionMenuStatus = 21,
    PlayerIdentity = 22,
    WorldState = 23,
    EquipmentState = 24,
    PauseVote = 25,
    PlayerMountState = 26,
    PlayerTraversal = 27,
    PlayerAnimationState = 28,
    MotionReplicationConfig = 29,
    PlayerAction = 30,
    MissionCameraState = 31,
    InteractionIntent = 32,
    InteractionResult = 33,
    RestraintState = 34,
    MissionCinematicState = 35,
    MissionCinematicAction = 36,
    PlayerAppearanceState = 37,
    AnimSceneReplicaState = 38,
    AnimSceneDefinition = 39,
    AnimSceneControl = 40,
    CampaignCapability = 41,
    CampaignCapabilityAck = 42,
    PickupCollected = 43,
    MissionProgression = 44,
};

[[nodiscard]] bool IsKnownMessageType(std::uint16_t value) noexcept;

struct FrameHeader final {
    std::uint32_t magic{kFrameMagic};
    std::uint16_t version{kProtocolVersion};
    MessageType type{MessageType::Heartbeat};
    std::uint32_t sequence{};
    std::uint64_t tick{};
    std::uint32_t payloadLength{};
};

struct Frame final {
    FrameHeader header{};
    std::vector<std::uint8_t> payload{};
};

enum class DecodeStatus {
    Complete,
    NeedMoreData,
    Invalid,
};

struct DecodeResult final {
    DecodeStatus status{DecodeStatus::NeedMoreData};
    std::size_t consumed{};
    std::optional<Frame> frame{};
    std::string error{};
};

class FrameCodec final {
public:
    [[nodiscard]] static std::vector<std::uint8_t> Encode(const Frame& frame);
    [[nodiscard]] static DecodeResult DecodeOne(std::span<const std::uint8_t> bytes);
};

class FrameStreamDecoder final {
public:
    void Append(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::optional<Frame> Pop();
    [[nodiscard]] bool HasError() const noexcept { return !error_.empty(); }
    [[nodiscard]] const std::string& Error() const noexcept { return error_; }
    void Reset();

private:
    std::vector<std::uint8_t> buffer_{};
    std::string error_{};
};

enum class SequenceDisposition {
    First,
    Newer,
    Duplicate,
    Stale,
};

class SequenceWindow final {
public:
    [[nodiscard]] SequenceDisposition Observe(std::uint32_t sequence) noexcept;
    void Reset() noexcept;

private:
    bool hasLast_{};
    std::uint32_t last_{};
};

class FrameSequencer final {
public:
    explicit constexpr FrameSequencer(std::uint32_t first = 1U) noexcept
        : next_(first == 0U ? 1U : first) {}

    [[nodiscard]] std::uint32_t Next() noexcept;

private:
    std::uint32_t next_;
};

enum class PlayerTraversalKind : std::uint8_t {
    None = 0,
    Jump = 1,
    Climb = 2,
};

enum class PlayerLocomotionMode : std::uint8_t {
    Grounded = 0,
    Aiming = 1,
    Traversal = 2,
    Airborne = 3,
    Ragdoll = 4,
    Mounted = 5,
};

struct PlayerStatePayload final {
    NetEntityId entityId{};
    PlayerSlot slot{PlayerSlot::Host};
    PlayerLifecycle lifecycle{PlayerLifecycle::Alive};
    Vec3 position{};
    Vec3 velocity{};
    float heading{};
    float healthFraction{1.0F};
    std::uint32_t flags{};
    Vec3 aimTarget{};
    std::uint32_t fireSequence{};
    // Semantic locomotion intent. Position/velocity remain authoritative
    // checkpoints, while these fields let the receiver reproduce the route
    // and action without trying to infer everything from delayed transforms.
    float movementHeading{};
    float localForwardSpeed{};
    float localRightSpeed{};
    float desiredMoveBlend{};
    std::uint16_t locomotionEpoch{};
    std::uint16_t traversalActionId{};
    PlayerTraversalKind traversalKind{PlayerTraversalKind::None};
    PlayerLocomotionMode locomotionMode{PlayerLocomotionMode::Grounded};
    Vec3 traversalAnchor{};
    float traversalHeading{};
};

enum class PlayerStateFlag : std::uint32_t {
    InMission = 1U << 0U,
    InCutscene = 1U << 1U,
    Mounted = 1U << 2U,
    Aiming = 1U << 3U,
    Firing = 1U << 4U,
    AimTargetValid = 1U << 5U,
    MeleeCombat = 1U << 6U,
    SyntheticTest = 1U << 7U,
    Jumping = 1U << 8U,
    Climbing = 1U << 9U,
    StealthMovement = 1U << 10U,
    PeerCombatTarget = 1U << 11U,
    PeerLassoActive = 1U << 12U,
    MeleeBlocking = 1U << 13U,
    MeleeGrappling = 1U << 14U,
    PeerKnockdown = 1U << 15U,
    InCover = 1U << 16U,
    GoingIntoCover = 1U << 17U,
    CoverFacingLeft = 1U << 18U,
    AimingFromCover = 1U << 19U,
    InWater = 1U << 20U,
    Swimming = 1U << 21U,
    SwimmingUnderwater = 1U << 22U,
    OnlineModeDetected = 1U << 31U,
};

inline constexpr std::size_t kPlayerStatePayloadSize = 104U;

[[nodiscard]] std::vector<std::uint8_t> EncodePlayerState(
    const PlayerStatePayload& payload);
[[nodiscard]] std::optional<PlayerStatePayload> DecodePlayerState(
    std::span<const std::uint8_t> bytes);

enum class PlayerTraversalFlag : std::uint32_t {
    InputEdgeDetected = 1U << 0U,
    ObstacleValid = 1U << 1U,
    ExpectedLandingValid = 1U << 2U,
};

struct PlayerTraversalPayload final {
    NetEntityId entityId{};
    PlayerSlot slot{PlayerSlot::Host};
    PlayerTraversalKind kind{PlayerTraversalKind::None};
    std::uint16_t actionId{};
    std::uint16_t revision{};
    std::uint16_t locomotionEpoch{};
    std::uint32_t flags{};
    float takeoffHeading{};
    Vec3 takeoffPosition{};
    Vec3 approachVelocity{};
    Vec3 obstaclePoint{};
    Vec3 obstacleNormal{};
    float obstacleTopZ{};
    Vec3 expectedLanding{};
};

inline constexpr std::size_t kPlayerTraversalPayloadSize = 88U;

[[nodiscard]] std::vector<std::uint8_t> EncodePlayerTraversal(
    const PlayerTraversalPayload& payload);
[[nodiscard]] std::optional<PlayerTraversalPayload> DecodePlayerTraversal(
    std::span<const std::uint8_t> bytes);

enum class PlayerActionKind : std::uint8_t {
    None = 0,
    Aim = 1,
    MeleeAttack = 2,
    MeleeBlock = 3,
    Grapple = 4,
    Lasso = 5,
    Hogtie = 6,
    Knockdown = 7,
    Crafting = 8,
};

enum class PlayerActionPhase : std::uint8_t {
    None = 0,
    Begin = 1,
    Active = 2,
    Impact = 3,
    Attached = 4,
    Sustain = 5,
    Bound = 6,
    Recover = 7,
    End = 8,
    Cancel = 9,
    Reject = 10,
    Snapshot = 11,
};

enum class PlayerActionFlag : std::uint32_t {
    Intent = 1U << 0U,
    Authoritative = 1U << 1U,
    TargetEntityValid = 1U << 2U,
    TargetPointValid = 1U << 3U,
    ActorAnchorValid = 1U << 4U,
    Persistent = 1U << 5U,
    PhysicalTargetEffect = 1U << 6U,
    ResyncSnapshot = 1U << 7U,
    VariantValid = 1U << 8U,
    AnimationSampleValid = 1U << 9U,
    NormalizedPhaseValid = 1U << 10U,
};

// Stable semantic discriminator carried in the existing variantHash field.
// It distinguishes a deliberate mounted-player pull from an ordinary
// Knockdown, whose input controls overlap vanilla talk/mount prompts.
inline constexpr std::uint32_t kPlayerActionVariantPeerMountPull =
    0x4D50554CU; // "MPUL"

struct PlayerActionPayload final {
    NetEntityId actorEntityId{};
    NetEntityId targetEntityId{};
    std::uint32_t sequence{};
    std::uint32_t actionId{};
    std::uint16_t revision{};
    PlayerSlot actorSlot{PlayerSlot::Host};
    PlayerSlot authoritySlot{PlayerSlot::Host};
    PlayerActionKind kind{PlayerActionKind::None};
    PlayerActionPhase phase{PlayerActionPhase::None};
    std::uint32_t flags{};
    std::uint32_t durationMilliseconds{};
    std::uint32_t phaseElapsedMilliseconds{};
    std::uint32_t weaponHash{};
    std::uint32_t variantHash{};
    std::uint32_t animationSampleSequence{};
    Vec3 actorAnchor{};
    Vec3 targetPoint{};
    float facingHeading{};
    float normalizedPhase{};
};

inline constexpr std::size_t kPlayerActionPayloadSize = 88U;

[[nodiscard]] std::vector<std::uint8_t> EncodePlayerAction(
    const PlayerActionPayload& payload);
[[nodiscard]] std::optional<PlayerActionPayload> DecodePlayerAction(
    std::span<const std::uint8_t> bytes);

enum class InteractionKind : std::uint8_t {
    None = 0,
    ReleaseRestraint = 1,
    Revive = 2,
    DismountPeer = 3,
    MountDriver = 4,
    MountPassenger = 5,
    DismountSelf = 6,
    EmergencyRecover = 7,
};

enum class InteractionIntentPhase : std::uint8_t {
    None = 0,
    Begin = 1,
    Sustain = 2,
    Cancel = 3,
};

enum class InteractionIntentFlag : std::uint8_t {
    TargetPlayer = 1U << 0U,
    TargetMount = 1U << 1U,
    HoldRequired = 1U << 2U,
};

struct InteractionIntentPayload final {
    NetEntityId actorEntityId{};
    NetEntityId targetEntityId{};
    NetEntityId secondaryEntityId{};
    std::uint32_t interactionId{};
    std::uint16_t revision{};
    PlayerSlot actorSlot{PlayerSlot::Host};
    InteractionKind kind{InteractionKind::None};
    InteractionIntentPhase phase{InteractionIntentPhase::None};
    std::uint8_t flags{};
    std::uint32_t requestedDurationMilliseconds{};
};

inline constexpr std::size_t kInteractionIntentPayloadSize = 40U;

[[nodiscard]] std::vector<std::uint8_t> EncodeInteractionIntent(
    const InteractionIntentPayload& payload);
[[nodiscard]] std::optional<InteractionIntentPayload>
DecodeInteractionIntent(std::span<const std::uint8_t> bytes);

enum class InteractionResultStatus : std::uint8_t {
    None = 0,
    Accepted = 1,
    InProgress = 2,
    Completed = 3,
    Rejected = 4,
    Cancelled = 5,
};

enum class InteractionRejectReason : std::uint8_t {
    None = 0,
    InvalidActor = 1,
    InvalidTarget = 2,
    TooFar = 3,
    InvalidState = 4,
    NotAuthority = 5,
    Busy = 6,
    Stale = 7,
    NativeFailed = 8,
    SessionReset = 9,
};

enum class InteractionResultFlag : std::uint16_t {
    Authoritative = 1U << 0U,
    StateChanged = 1U << 1U,
    HoldRequired = 1U << 2U,
};

struct InteractionResultPayload final {
    NetEntityId actorEntityId{};
    NetEntityId targetEntityId{};
    NetEntityId secondaryEntityId{};
    std::uint32_t interactionId{};
    std::uint16_t revision{};
    InteractionKind kind{InteractionKind::None};
    InteractionResultStatus status{InteractionResultStatus::None};
    InteractionRejectReason rejectReason{InteractionRejectReason::None};
    std::uint16_t flags{};
    std::uint32_t progressMilliseconds{};
    std::uint32_t requiredDurationMilliseconds{};
};

inline constexpr std::size_t kInteractionResultPayloadSize = 44U;

[[nodiscard]] std::vector<std::uint8_t> EncodeInteractionResult(
    const InteractionResultPayload& payload);
[[nodiscard]] std::optional<InteractionResultPayload>
DecodeInteractionResult(std::span<const std::uint8_t> bytes);

enum class PlayerRestraintState : std::uint8_t {
    Free = 0,
    Lassoed = 1,
    Hogtied = 2,
};

enum class RestraintStateFlag : std::uint8_t {
    Authoritative = 1U << 0U,
    EngineOwned = 1U << 1U,
    Snapshot = 1U << 2U,
};

struct RestraintStatePayload final {
    NetEntityId subjectEntityId{};
    NetEntityId ownerEntityId{};
    std::uint32_t sourceInteractionId{};
    std::uint32_t revision{};
    PlayerRestraintState state{PlayerRestraintState::Free};
    std::uint8_t flags{};
};

inline constexpr std::size_t kRestraintStatePayloadSize = 28U;

[[nodiscard]] std::vector<std::uint8_t> EncodeRestraintState(
    const RestraintStatePayload& payload);
[[nodiscard]] std::optional<RestraintStatePayload>
DecodeRestraintState(std::span<const std::uint8_t> bytes);

struct PlayerIdentityPayload final {
    NetEntityId entityId{};
    PlayerSlot slot{PlayerSlot::Host};
    std::string nickname{};
};

inline constexpr std::size_t kPlayerIdentityHeaderSize = 10U;
inline constexpr std::size_t kMaximumPlayerNicknameUtf8Bytes = 64U;
inline constexpr std::size_t kMaximumPlayerNicknameCodePoints = 24U;

[[nodiscard]] std::vector<std::uint8_t> EncodePlayerIdentity(
    const PlayerIdentityPayload& payload);
[[nodiscard]] std::optional<PlayerIdentityPayload> DecodePlayerIdentity(
    std::span<const std::uint8_t> bytes);

enum class PlayerAppearanceStateFlag : std::uint16_t {
    CompleteComponentSet = 1U << 0U,
    StoryMetaPed = 1U << 1U,
};

// A portable MetaPed description. Local handles, pointers and texture objects
// never leave the process; only stable shop-component hashes are replicated.
// The original component order is retained because some Story MetaPed layers
// override earlier layers during UPDATE_PED_VARIATION.
struct PlayerAppearanceStatePayload final {
    NetEntityId entityId{};
    PlayerSlot slot{PlayerSlot::Host};
    std::uint8_t schemaVersion{1U};
    std::uint16_t flags{};
    std::uint32_t revision{};
    std::uint32_t modelHash{};
    std::uint64_t fingerprint{};
    std::vector<std::uint32_t> componentHashes{};
};

inline constexpr std::size_t kPlayerAppearanceStateHeaderSize = 32U;
inline constexpr std::size_t kMaximumPlayerAppearanceComponents = 64U;

[[nodiscard]] std::vector<std::uint8_t> EncodePlayerAppearanceState(
    const PlayerAppearanceStatePayload& payload);
[[nodiscard]] std::optional<PlayerAppearanceStatePayload>
DecodePlayerAppearanceState(std::span<const std::uint8_t> bytes);

enum class WorldEntityKind : std::uint8_t {
    Ped = 1,
    Object = 2,
};

enum class WorldEntityStateFlag : std::uint8_t {
    Human = 1U << 0U,
    Horse = 1U << 1U,
    Dead = 1U << 2U,
    InCombat = 1U << 3U,
    Firing = 1U << 4U,
    Aiming = 1U << 5U,
    Mounted = 1U << 6U,
    ScriptOwned = 1U << 7U,
};

enum class WorldCombatTargetSlot : std::uint8_t {
    None = 0,
    Host = 1,
    Guest = 2,
};

enum class WorldTaskKind : std::uint8_t {
    Idle = 0,
    Locomotion = 1,
    Scenario = 2,
    Fleeing = 3,
    Combat = 4,
    Mounted = 5,
    Dead = 6,
    Cinematic = 7,
};

struct WorldEntityStatePayload final {
    NetEntityId entityId{};
    std::uint32_t modelHash{};
    WorldEntityKind kind{WorldEntityKind::Ped};
    std::uint8_t flags{};
    WorldCombatTargetSlot combatTargetSlot{
        WorldCombatTargetSlot::None};
    Vec3 position{};
    Vec3 velocity{};
    float heading{};
    float healthFraction{1.0F};
    std::uint32_t weaponHash{};
    WorldTaskKind taskKind{WorldTaskKind::Idle};
    NetEntityId parentEntityId{};
    Vec3 taskTarget{};
};

inline constexpr std::size_t kWorldEntityStatePayloadSize = 76U;

[[nodiscard]] std::vector<std::uint8_t> EncodeWorldEntityState(
    const WorldEntityStatePayload& payload);
[[nodiscard]] std::optional<WorldEntityStatePayload> DecodeWorldEntityState(
    std::span<const std::uint8_t> bytes);

enum class PlayerMountStateFlag : std::uint8_t {
    Present = 1U << 0U,
    Mounted = 1U << 1U,
    Dead = 1U << 2U,
    BorrowedPeerMount = 1U << 3U,
    Vehicle = 1U << 4U,
    VehicleDriver = 1U << 5U,
    VehiclePassenger = 1U << 6U,
};

struct PlayerMountStatePayload final {
    NetEntityId playerEntityId{};
    NetEntityId mountEntityId{};
    PlayerSlot slot{PlayerSlot::Host};
    std::uint8_t flags{};
    std::uint32_t modelHash{};
    Vec3 position{};
    Vec3 velocity{};
    float heading{};
    float healthFraction{};
    std::uint32_t generation{};
};

inline constexpr std::size_t kPlayerMountStatePayloadSize = 60U;

[[nodiscard]] std::vector<std::uint8_t> EncodePlayerMountState(
    const PlayerMountStatePayload& payload);
[[nodiscard]] std::optional<PlayerMountStatePayload> DecodePlayerMountState(
    std::span<const std::uint8_t> bytes);

struct EntityDespawnPayload final {
    NetEntityId entityId{};
};

inline constexpr std::size_t kEntityDespawnPayloadSize = 8U;

[[nodiscard]] std::vector<std::uint8_t> EncodeEntityDespawn(
    const EntityDespawnPayload& payload);
[[nodiscard]] std::optional<EntityDespawnPayload> DecodeEntityDespawn(
    std::span<const std::uint8_t> bytes);

struct DamageIntentPayload final {
    NetEntityId attackerId{};
    NetEntityId targetId{};
    std::uint32_t weaponHash{};
    float damage{};
    std::uint32_t shotSequence{};
};

inline constexpr std::size_t kDamageIntentPayloadSize = 32U;

[[nodiscard]] std::vector<std::uint8_t> EncodeDamageIntent(
    const DamageIntentPayload& payload);
[[nodiscard]] std::optional<DamageIntentPayload> DecodeDamageIntent(
    std::span<const std::uint8_t> bytes);

enum class WorldStateFlag : std::uint8_t {
    WeatherValid = 1U << 0U,
};

struct WorldStatePayload final {
    std::uint8_t hour{};
    std::uint8_t minute{};
    std::uint8_t second{};
    std::uint8_t flags{};
    std::uint32_t weatherFrom{};
    std::uint32_t weatherTo{};
    float weatherBlend{};
    std::uint8_t day{1U};
    std::uint8_t month{};
    std::uint16_t year{1899U};
};

inline constexpr std::size_t kWorldStatePayloadSize = 24U;

[[nodiscard]] std::vector<std::uint8_t> EncodeWorldState(
    const WorldStatePayload& payload);
[[nodiscard]] std::optional<WorldStatePayload> DecodeWorldState(
    std::span<const std::uint8_t> bytes);

enum class MissionPhase : std::uint8_t {
    Idle = 0,
    Active = 1,
    Cutscene = 2,
    Loading = 3,
    Recovery = 4,
    SoloOverride = 5,
};

enum class MissionStateFlag : std::uint8_t {
    MissionActive = 1U << 0U,
    AnchorValid = 1U << 1U,
    CheckpointRecovery = 1U << 2U,
    ScriptedControlLock = 1U << 3U,
    ScreenTransition = 1U << 4U,
    ScenarioActivity = 1U << 5U,
    ScriptedVehicleTransition = 1U << 6U,
    MinigameActivity = 1U << 7U,
};

struct MissionStatePayload final {
    NetEntityId hostEntityId{};
    std::uint32_t missionEpoch{};
    std::uint32_t revision{};
    std::uint32_t checkpointGeneration{};
    MissionPhase phase{MissionPhase::Idle};
    std::uint8_t flags{};
    Vec3 hostAnchor{};
    float hostHeading{};

    [[nodiscard]] bool operator==(
        const MissionStatePayload& other) const noexcept {
        return hostEntityId == other.hostEntityId &&
               missionEpoch == other.missionEpoch &&
               revision == other.revision &&
               checkpointGeneration == other.checkpointGeneration &&
               phase == other.phase &&
               flags == other.flags &&
               hostAnchor.x == other.hostAnchor.x &&
               hostAnchor.y == other.hostAnchor.y &&
               hostAnchor.z == other.hostAnchor.z &&
               hostHeading == other.hostHeading;
    }
};

inline constexpr std::size_t kMissionStatePayloadSize = 48U;

[[nodiscard]] std::vector<std::uint8_t> EncodeMissionState(
    const MissionStatePayload& payload);
[[nodiscard]] std::optional<MissionStatePayload> DecodeMissionState(
    std::span<const std::uint8_t> bytes);

enum class MissionCinematicPhase : std::uint8_t {
    Playing = 1,
    Loading = 2,
    PrepareResume = 3,
    Completed = 4,
    Aborted = 5,
};

enum class MissionCinematicStateFlag : std::uint16_t {
    CameraExpected = 1U << 0U,
    AnchorValid = 1U << 1U,
    SkipPending = 1U << 2U,
    ResumeTimedOut = 1U << 3U,
};

struct MissionCinematicStatePayload final {
    NetEntityId hostEntityId{};
    std::uint32_t missionEpoch{};
    std::uint32_t cinematicGeneration{};
    std::uint32_t revision{};
    std::uint32_t checkpointGeneration{};
    MissionCinematicPhase phase{MissionCinematicPhase::Playing};
    std::uint16_t flags{};
    Vec3 resumeAnchor{};
    float resumeHeading{};

    [[nodiscard]] bool operator==(
        const MissionCinematicStatePayload& other) const noexcept {
        return hostEntityId == other.hostEntityId &&
               missionEpoch == other.missionEpoch &&
               cinematicGeneration == other.cinematicGeneration &&
               revision == other.revision &&
               checkpointGeneration == other.checkpointGeneration &&
               phase == other.phase &&
               flags == other.flags &&
               resumeAnchor.x == other.resumeAnchor.x &&
               resumeAnchor.y == other.resumeAnchor.y &&
               resumeAnchor.z == other.resumeAnchor.z &&
               resumeHeading == other.resumeHeading;
    }
};

inline constexpr std::size_t kMissionCinematicStatePayloadSize = 48U;

[[nodiscard]] std::vector<std::uint8_t> EncodeMissionCinematicState(
    const MissionCinematicStatePayload& payload);
[[nodiscard]] std::optional<MissionCinematicStatePayload>
DecodeMissionCinematicState(std::span<const std::uint8_t> bytes);

enum class MissionCinematicActionKind : std::uint8_t {
    PresentationReady = 1,
    ResumeReady = 2,
    SkipRequest = 3,
};

enum class MissionCinematicActionFlag : std::uint16_t {
    FallbackUsed = 1U << 0U,
};

struct MissionCinematicActionPayload final {
    NetEntityId hostEntityId{};
    std::uint32_t missionEpoch{};
    std::uint32_t cinematicGeneration{};
    std::uint32_t actionId{};
    MissionCinematicActionKind kind{
        MissionCinematicActionKind::PresentationReady};
    std::uint8_t senderSlot{};
    std::uint16_t flags{};

    [[nodiscard]] bool operator==(
        const MissionCinematicActionPayload&) const noexcept = default;
};

inline constexpr std::size_t kMissionCinematicActionPayloadSize = 32U;

[[nodiscard]] std::vector<std::uint8_t> EncodeMissionCinematicAction(
    const MissionCinematicActionPayload& payload);
[[nodiscard]] std::optional<MissionCinematicActionPayload>
DecodeMissionCinematicAction(std::span<const std::uint8_t> bytes);

enum class AnimSceneReplicaStateFlag : std::uint32_t {
    Active = 1U << 0U,
    Running = 1U << 1U,
    Loaded = 1U << 2U,
    CameraActive = 1U << 3U,
    OriginValid = 1U << 4U,
};

// Identifies an engine-owned AnimScene by portable characteristics only.
// The local AnimScene handle is deliberately not transmitted. A guest may
// attach only to an already existing local scene whose dictionary hash and
// duration match this state; otherwise the camera-replication fallback stays
// active.
struct AnimSceneReplicaStatePayload final {
    NetEntityId hostEntityId{};
    std::uint32_t missionEpoch{};
    std::uint32_t cinematicGeneration{};
    // Zero keeps the dictionary-signature SAFE_FALLBACK path valid when the
    // runtime capture layer could not produce a complete definition.
    std::uint32_t definitionRevision{};
    std::uint32_t revision{};
    std::uint32_t dictionaryHash{};
    std::uint32_t flags{};
    float phase{};
    float durationSeconds{};
    float rate{1.0F};
    Vec3 originPosition{};
    Vec3 originRotation{};
    std::uint16_t activeCameraCount{};

    [[nodiscard]] bool operator==(
        const AnimSceneReplicaStatePayload& other) const noexcept {
        return hostEntityId == other.hostEntityId &&
               missionEpoch == other.missionEpoch &&
               cinematicGeneration == other.cinematicGeneration &&
               definitionRevision == other.definitionRevision &&
               revision == other.revision &&
               dictionaryHash == other.dictionaryHash &&
               flags == other.flags && phase == other.phase &&
               durationSeconds == other.durationSeconds &&
               rate == other.rate &&
               originPosition.x == other.originPosition.x &&
               originPosition.y == other.originPosition.y &&
               originPosition.z == other.originPosition.z &&
               originRotation.x == other.originRotation.x &&
               originRotation.y == other.originRotation.y &&
               originRotation.z == other.originRotation.z &&
               activeCameraCount == other.activeCameraCount;
    }
};

inline constexpr std::size_t kAnimSceneReplicaStatePayloadSize = 72U;

[[nodiscard]] std::vector<std::uint8_t> EncodeAnimSceneReplicaState(
    const AnimSceneReplicaStatePayload& payload);
[[nodiscard]] std::optional<AnimSceneReplicaStatePayload>
DecodeAnimSceneReplicaState(std::span<const std::uint8_t> bytes);

enum class AnimSceneRoleKind : std::uint8_t {
    Ped = 1,
    Horse = 2,
    Object = 3,
    Vehicle = 4,
    Pickup = 5,
};

enum class AnimSceneRoleFlag : std::uint16_t {
    Required = 1U << 0U,
    Player = 1U << 1U,
};

struct AnimSceneRoleBindingPayload final {
    std::string roleName{};
    NetEntityId entityId{};
    std::uint32_t modelHash{};
    AnimSceneRoleKind kind{AnimSceneRoleKind::Ped};
    std::uint16_t flags{};
    std::uint32_t bindingFlags{};

    [[nodiscard]] bool operator==(
        const AnimSceneRoleBindingPayload&) const noexcept = default;
};

struct AnimSceneDefinitionPayload final {
    NetEntityId hostEntityId{};
    std::uint32_t missionEpoch{};
    std::uint32_t cinematicGeneration{};
    std::uint32_t definitionRevision{};
    std::uint32_t dictionaryHash{};
    std::uint64_t fingerprintLow{};
    std::uint64_t fingerprintHigh{};
    float durationSeconds{};
    std::uint32_t sceneFlags{};
    std::uint8_t createOptionFlags{};
    std::string resourceName{};
    std::string playbackList{};
    std::vector<AnimSceneRoleBindingPayload> roles{};

    [[nodiscard]] bool operator==(
        const AnimSceneDefinitionPayload&) const noexcept = default;
};

struct AnimSceneDefinitionFingerprint final {
    std::uint64_t low{};
    std::uint64_t high{};

    [[nodiscard]] bool operator==(
        const AnimSceneDefinitionFingerprint&) const noexcept = default;
};

inline constexpr std::size_t kAnimSceneDefinitionHeaderSize = 60U;
inline constexpr std::size_t kAnimSceneRoleBindingHeaderSize = 20U;
inline constexpr std::size_t kMaximumAnimSceneDefinitionPayloadSize = 8'192U;
inline constexpr std::size_t kMaximumAnimSceneDefinitionRoles = 48U;
inline constexpr std::size_t kMaximumAnimSceneResourceBytes = 256U;
inline constexpr std::size_t kMaximumAnimScenePlaybackListBytes = 128U;
inline constexpr std::size_t kMaximumAnimSceneRoleNameBytes = 64U;

// Computes SHA-256 over the canonical wire payload with the 16 fingerprint
// bytes cleared, then returns the first 16 digest bytes as two LE words.
// All other definition fields must already satisfy canonical validation.
[[nodiscard]] AnimSceneDefinitionFingerprint
ComputeAnimSceneDefinitionFingerprint(
    const AnimSceneDefinitionPayload& payload);

[[nodiscard]] std::vector<std::uint8_t> EncodeAnimSceneDefinition(
    const AnimSceneDefinitionPayload& payload);
[[nodiscard]] std::optional<AnimSceneDefinitionPayload>
DecodeAnimSceneDefinition(std::span<const std::uint8_t> bytes);

enum class AnimSceneControlKind : std::uint8_t {
    GuestReady = 1,
    GuestRejected = 2,
    HostPlayCommit = 3,
    HostAbort = 4,
};

enum class AnimSceneControlReason : std::uint8_t {
    None = 0,
    InvalidDefinition = 1,
    UnsupportedResource = 2,
    LoadTimeout = 3,
    MissingBinding = 4,
    EntityMismatch = 5,
    NativeFailure = 6,
    Superseded = 7,
    StaleGeneration = 8,
};

enum class AnimSceneControlFlag : std::uint32_t {
    ResourceLoaded = 1U << 0U,
    RequiredRolesBound = 1U << 1U,
    CacheHit = 1U << 2U,
    LateJoin = 1U << 3U,
    FallbackUsed = 1U << 4U,
};

struct AnimSceneControlPayload final {
    NetEntityId hostEntityId{};
    std::uint32_t missionEpoch{};
    std::uint32_t cinematicGeneration{};
    std::uint32_t definitionRevision{};
    std::uint32_t actionId{};
    std::uint64_t fingerprintLow{};
    std::uint64_t fingerprintHigh{};
    std::uint64_t playAtHostTick{};
    float startPhase{};
    float rate{};
    AnimSceneControlKind kind{AnimSceneControlKind::GuestReady};
    std::uint8_t senderSlot{};
    AnimSceneControlReason reason{AnimSceneControlReason::None};
    std::uint32_t flags{};

    [[nodiscard]] bool operator==(
        const AnimSceneControlPayload&) const noexcept = default;
};

inline constexpr std::size_t kAnimSceneControlPayloadSize = 64U;

[[nodiscard]] std::vector<std::uint8_t> EncodeAnimSceneControl(
    const AnimSceneControlPayload& payload);
[[nodiscard]] std::optional<AnimSceneControlPayload>
DecodeAnimSceneControl(std::span<const std::uint8_t> bytes);

enum class MissionCameraStateFlag : std::uint32_t {
    Active = 1U << 0U,
    ScreenFadedOut = 1U << 1U,
    ScreenFadingOut = 1U << 2U,
    ScreenFadingIn = 1U << 3U,
    SourceRenderingScriptCamera = 1U << 4U,
    SourceCinematicGameplayCamera = 1U << 5U,
    SourceGameplayCameraFallback = 1U << 6U,
};

// A portable rendering-camera snapshot. This intentionally does not expose
// local camera handles, AnimScene identities or pointers across processes.
struct MissionCameraStatePayload final {
    NetEntityId hostEntityId{};
    std::uint32_t missionEpoch{};
    std::uint32_t cinematicGeneration{};
    std::uint32_t revision{};
    std::uint32_t flags{};
    Vec3 position{};
    Vec3 rotation{};
    float fieldOfView{};

    [[nodiscard]] bool operator==(
        const MissionCameraStatePayload& other) const noexcept {
        return hostEntityId == other.hostEntityId &&
               missionEpoch == other.missionEpoch &&
               cinematicGeneration == other.cinematicGeneration &&
               revision == other.revision &&
               flags == other.flags &&
               position.x == other.position.x &&
               position.y == other.position.y &&
               position.z == other.position.z &&
               rotation.x == other.rotation.x &&
               rotation.y == other.rotation.y &&
               rotation.z == other.rotation.z &&
               fieldOfView == other.fieldOfView;
    }
};

inline constexpr std::size_t kMissionCameraStatePayloadSize = 56U;

[[nodiscard]] std::vector<std::uint8_t> EncodeMissionCameraState(
    const MissionCameraStatePayload& payload);
[[nodiscard]] std::optional<MissionCameraStatePayload>
DecodeMissionCameraState(std::span<const std::uint8_t> bytes);

enum class EquipmentStateFlag : std::uint32_t {
    Equipped = 1U << 0U,
    Reloading = 1U << 1U,
};

struct EquipmentStatePayload final {
    NetEntityId entityId{};
    std::uint32_t weaponHash{};
    std::uint32_t ammo{};
    std::uint32_t flags{};

    [[nodiscard]] bool operator==(
        const EquipmentStatePayload&) const noexcept = default;
};

inline constexpr std::size_t kEquipmentStatePayloadSize = 24U;

[[nodiscard]] std::vector<std::uint8_t> EncodeEquipmentState(
    const EquipmentStatePayload& payload);
[[nodiscard]] std::optional<EquipmentStatePayload> DecodeEquipmentState(
    std::span<const std::uint8_t> bytes);

enum class PauseVoteKind : std::uint8_t {
    RequestToggle = 1,
    AuthoritativeState = 2,
};

enum class PauseVoteFlag : std::uint8_t {
    HostVoted = 1U << 0U,
    GuestVoted = 1U << 1U,
    Paused = 1U << 2U,
};

struct PauseVotePayload final {
    PauseVoteKind kind{PauseVoteKind::RequestToggle};
    PlayerSlot voterSlot{PlayerSlot::Host};
    std::uint8_t flags{};
    std::uint32_t generation{};
};

inline constexpr std::size_t kPauseVotePayloadSize = 12U;

[[nodiscard]] std::vector<std::uint8_t> EncodePauseVote(
    const PauseVotePayload& payload);
[[nodiscard]] std::optional<PauseVotePayload> DecodePauseVote(
    std::span<const std::uint8_t> bytes);

enum class BridgeCommand : std::uint16_t {
    ToggleSoloOverride = 1,
    TeleportGuest = 2,
    ResyncEntities = 3,
    ResyncEquipment = 4,
    RetryCheckpoint = 5,
    ToggleDiagnostics = 6,
    Unload = 7,
    TeleportToPlayer = 8,
    ToggleSoloTest = 9,
    ToggleGhostRecord = 10,
    ToggleGhostReplay = 11,
    StopSession = 12,
    GrantTestPistol = 13,
    SaveProblemMarker = 14,
    GrantTestLasso = 15,
    SkipCutscene = 16,
    EmergencyRecover = 17,
    ProbeRepeatingShotgunShopUnlock = 18,
    EnableRepeatingShotgunShopUnlock = 19,
    ProbePoisonThrowingKnifePamphlet = 20,
    EnablePoisonThrowingKnifePamphlet = 21,
    ArmHunt1MissionProgression = 22,
    ArmFud1MissionProgression = 23,
    DisarmMissionProgression = 24,
};

enum class CampaignCapabilityKind : std::uint8_t {
    WeaponShopEligibility = 1,
    Recipe = 2,
    CapacityUpgrade = 3,
    ActivityGate = 4,
};

struct CampaignCapabilityPayload final {
    CampaignCapabilityKind kind{CampaignCapabilityKind::WeaponShopEligibility};
    std::uint32_t recordHash{};
    std::uint64_t hostEventId{};
    std::int64_t grantedAtUnixMilliseconds{};
};

inline constexpr std::size_t kCampaignCapabilityPayloadSize = 24U;
[[nodiscard]] std::vector<std::uint8_t> EncodeCampaignCapability(
    const CampaignCapabilityPayload& payload);
[[nodiscard]] std::optional<CampaignCapabilityPayload> DecodeCampaignCapability(
std::span<const std::uint8_t> bytes);

struct CampaignCapabilityAckPayload final {
    CampaignCapabilityKind kind{CampaignCapabilityKind::WeaponShopEligibility};
    std::uint32_t recordHash{};
    std::uint64_t hostEventId{};
};
struct PickupCollectedPayload final {
    NetEntityId actorEntityId{};
    std::uint64_t collectionId{};
    std::uint32_t pickupHash{};
};

// A deny-by-default per-mission handshake.  It contains only a catalog mission
// ID and transaction identity—never a save path, inventory, or script memory.
enum class MissionProgressionPhase : std::uint8_t {
    Offer = 1,
    Eligibility = 2,
    Completion = 3,
};
enum class MissionProgressionFlag : std::uint8_t {
    GuestCanStart = 1U << 0U,
    VerifiedCompletionMapping = 1U << 1U,
};
struct MissionProgressionPayload final {
    std::uint32_t missionId{};
    std::uint32_t missionEpoch{};
    std::uint64_t eventId{};
    MissionProgressionPhase phase{MissionProgressionPhase::Offer};
    std::uint8_t flags{};
    // MissionData rating from the host's completed mission: normal complete
    // (2), bronze (3), silver (4), or gold (5). It is zero for offer and
    // eligibility frames and audit-only completions.
    std::uint8_t completionRating{};
    // Positive local-cash delta observed over this exact host mission run.
    // It is bounded and never represents the host's total balance.
    std::int32_t completionCashAward{};
    [[nodiscard]] constexpr bool operator==(
        const MissionProgressionPayload&) const noexcept = default;
};
inline constexpr std::size_t kMissionProgressionPayloadSize = 24U;
[[nodiscard]] std::vector<std::uint8_t> EncodeMissionProgression(
    const MissionProgressionPayload& payload);
[[nodiscard]] std::optional<MissionProgressionPayload> DecodeMissionProgression(
    std::span<const std::uint8_t> bytes);
inline constexpr std::size_t kPickupCollectedPayloadSize = 24U;
[[nodiscard]] std::vector<std::uint8_t> EncodePickupCollected(const PickupCollectedPayload& payload);
[[nodiscard]] std::optional<PickupCollectedPayload> DecodePickupCollected(std::span<const std::uint8_t> bytes);
inline constexpr std::size_t kCampaignCapabilityAckPayloadSize = 16U;
[[nodiscard]] std::vector<std::uint8_t> EncodeCampaignCapabilityAck(const CampaignCapabilityAckPayload& payload);
[[nodiscard]] std::optional<CampaignCapabilityAckPayload> DecodeCampaignCapabilityAck(std::span<const std::uint8_t> bytes);

// Wire opcodes are shared with the C# sidecar. They are intentionally separate
// from the local F9 menu enum above and must never be converted by numeric cast.
enum class CommandOpcode : std::uint16_t {
    SpawnReplica = 1,
    ApplyTransform = 2,
    DespawnReplica = 3,
    SpectatorOn = 4,
    SpectatorOff = 5,
    TeleportGuest = 6,
    EnterDowned = 7,
    CompleteRevive = 8,
    RetryCheckpoint = 9,
    SoloOverrideOn = 10,
    SoloOverrideOff = 11,
    Resync = 12,
    Unload = 13,
    ToggleDiagnostics = 14,
    ResyncEquipment = 15,
    DiagnosticMarker = 16,
};

struct CommandPayload final {
    CommandOpcode opcode{CommandOpcode::SpawnReplica};
    std::uint16_t flags{};
    NetEntityId target{};
    Vec3 position{};
    float heading{};
    float value{};
};

inline constexpr std::size_t kCommandPayloadSize = 32U;

[[nodiscard]] std::vector<std::uint8_t> EncodeCommand(const CommandPayload& payload);
[[nodiscard]] std::optional<CommandPayload> DecodeCommand(
    std::span<const std::uint8_t> bytes);

struct DownedStatePayload final {
    NetEntityId entityId{};
    PlayerLifecycle lifecycle{PlayerLifecycle::Alive};
    float healthFraction{};
};

inline constexpr std::size_t kDownedStatePayloadSize = 16U;

[[nodiscard]] std::vector<std::uint8_t> EncodeDownedState(
    const DownedStatePayload& payload);
[[nodiscard]] std::optional<DownedStatePayload> DecodeDownedState(
    std::span<const std::uint8_t> bytes);

struct ReviveRequestPayload final {
    NetEntityId reviverId{};
    NetEntityId targetId{};
};

inline constexpr std::size_t kReviveRequestPayloadSize = 16U;

[[nodiscard]] std::vector<std::uint8_t> EncodeReviveRequest(
    const ReviveRequestPayload& payload);
[[nodiscard]] std::optional<ReviveRequestPayload> DecodeReviveRequest(
    std::span<const std::uint8_t> bytes);

struct ReviveCompletePayload final {
    NetEntityId reviverId{};
    NetEntityId targetId{};
    float healthFraction{0.35F};
};

inline constexpr std::size_t kReviveCompletePayloadSize = 20U;

[[nodiscard]] std::vector<std::uint8_t> EncodeReviveComplete(
    const ReviveCompletePayload& payload);
[[nodiscard]] std::optional<ReviveCompletePayload> DecodeReviveComplete(
    std::span<const std::uint8_t> bytes);

enum class SessionMenuAction : std::uint8_t {
    Host = 1,
    JoinFromClipboard = 2,
    ToggleSoloTest = 3,
    ToggleGhostRecord = 4,
    ToggleGhostReplay = 5,
    StopSession = 6,
};

enum class SessionMenuStatusKind : std::uint8_t {
    Waiting = 0,
    StartingHost = 1,
    StartingGuest = 2,
    ReadyHost = 3,
    ReadyGuest = 4,
    Error = 5,
};

struct SessionMenuStatusPayload final {
    SessionMenuStatusKind kind{SessionMenuStatusKind::Waiting};
    std::string message{};
    std::string inviteCode{};
};

inline constexpr std::size_t kMaximumSessionInviteCodeBytes = 768U;
inline constexpr std::size_t kMaximumSessionStatusMessageBytes = 384U;

[[nodiscard]] std::vector<std::uint8_t> EncodeSessionMenuRequest(
    SessionMenuAction action,
    std::string_view inviteCode = {});
[[nodiscard]] std::optional<SessionMenuStatusPayload> DecodeSessionMenuStatus(
    std::span<const std::uint8_t> bytes);

}  // namespace coopstory::bridge
