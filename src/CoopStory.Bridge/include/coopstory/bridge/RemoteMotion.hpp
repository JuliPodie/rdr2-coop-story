#pragma once

#include "coopstory/bridge/Domain.hpp"
#include "coopstory/bridge/FrameCodec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace coopstory::bridge {

// Ordinary network gaps must be caught up smoothly. A coordinate warp is
// reserved for a large jump in the authoritative target itself (teleport,
// reconnect or another explicit discontinuity), never merely because the
// local replica fell behind.
inline constexpr float kRemoteMotionSnapDistanceMeters = 25.0F;
inline constexpr float kRemoteMotionMaximumHorizontalSpeed = 14.0F;
inline constexpr float kRemoteMotionMaximumVerticalSpeed = 8.0F;
// RDR2 can replace a script-assigned ped velocity before the following frame.
// The SDK facade therefore carries the last commanded assist velocity across
// frames instead of rebuilding the ramp from GET_ENTITY_VELOCITY. This keeps
// acceleration smooth without getting stuck at its first tiny increment.
inline constexpr float kRemoteMotionVelocityCorrectionGain = 2.80F;
inline constexpr float kRemoteMotionVerticalCorrectionGain = 2.00F;
inline constexpr float kRemoteMotionMaximumAccelerationMetersPerSecond2 =
    24.0F;
inline constexpr float kRemoteMotionMaximumVerticalAccelerationMetersPerSecond2 =
    18.0F;
// Native locomotion owns small tracking errors. Physical velocity joins only
// after a meaningful drift and remains active until the replica is close,
// preventing sub-metre start/stop bursts around the marker.
inline constexpr float kRemoteMotionPhysicsAssistEnterDistanceMeters = 1.50F;
inline constexpr float kRemoteMotionPhysicsAssistExitDistanceMeters = 0.50F;
inline constexpr float kRemoteMotionHeadingSpeedDegreesPerSecond = 360.0F;
// While a replica is behind, the root must face its authoritative
// destination rather than the independently replicated aim/character
// heading. This prevents an aiming strafe task from pulling the ped away
// from the marker after a stop-and-reverse transition.
inline constexpr float kRemoteMotionRootHeadingDistanceMeters = 0.50F;
inline constexpr float kRemoteMotionCatchUpHeadingSpeedDegreesPerSecond =
    720.0F;
inline constexpr float kRemoteAimRootSuppressEnterMeters = 1.25F;
inline constexpr float kRemoteAimRootSuppressExitMeters = 0.50F;
inline constexpr float kRemoteMotionMaximumGroundCorrectionMeters = 3.0F;
inline constexpr float kRemoteMotionCatchUpDeadZoneMeters = 0.30F;
inline constexpr float kRemoteMotionCatchUpRunDistanceMeters = 1.50F;
inline constexpr float kRemoteMotionCatchUpSprintDistanceMeters = 4.0F;
inline constexpr float kRemoteMotionCatchUpRunSpeedMetersPerSecond = 3.5F;
inline constexpr float kRemoteMotionCatchUpSprintSpeedMetersPerSecond = 6.5F;
inline constexpr float kRemoteMotionMoveRateBoostStartMeters = 0.35F;
inline constexpr float kRemoteMotionCatchUpMoveRateGainPerMeter = 0.05F;
// NativeDB documents 1.15 as this native's supported ceiling. V12.2 requested
// 1.75, but the game did not visibly accelerate the animation: the engine
// clamps or ignores that unsupported range. Catch-up above sprint speed must
// therefore come from route recovery, never an invalid move-rate value.
inline constexpr float kRemoteMotionCatchUpMaximumMoveRate = 1.15F;
inline constexpr float kRemoteMotionIdleToWalkMetersPerSecond = 0.25F;
inline constexpr float kRemoteMotionWalkToIdleMetersPerSecond = 0.12F;
inline constexpr float kRemoteMotionWalkToRunMetersPerSecond = 2.20F;
inline constexpr float kRemoteMotionRunToWalkMetersPerSecond = 1.70F;
inline constexpr float kRemoteMotionRunToSprintMetersPerSecond = 5.0F;
inline constexpr float kRemoteMotionSprintToRunMetersPerSecond = 4.20F;
// Network speed samples can hover around a gait threshold for several
// snapshots. Holding the selected gait a little longer prevents the native
// task graph from repeatedly restarting walk/run animations.
inline constexpr std::uint32_t kRemoteMotionMinimumGaitDwellMs = 350U;
// Direct velocity is excellent in open terrain but fights collision when the
// target is behind a wall. A short progress probe promotes the puppet to an
// RDR2 navmesh task only after sustained evidence of being blocked.
inline constexpr std::uint32_t kRemoteNavigationProbeIntervalMs = 250U;
inline constexpr std::uint64_t kRemoteNavigationStallEnterMs = 600U;
inline constexpr float kRemoteNavigationEnterErrorMeters = 3.0F;
inline constexpr float kRemoteNavigationExitErrorMeters = 1.25F;
inline constexpr float kRemoteNavigationMinimumPedTravelMeters = 0.12F;
inline constexpr float kRemoteNavigationMinimumTargetTravelMeters = 0.25F;
inline constexpr float kRemoteNavigationMinimumErrorImprovementMeters = 0.15F;
inline constexpr float kRemoteNavigationLargeErrorMeters = 8.0F;
inline constexpr float kRemoteNavigationWaypointSpacingMeters = 0.35F;
inline constexpr float kRemoteNavigationWaypointReachedMeters = 0.75F;
// A ped can pass beside a recorded point without entering the narrow reached
// radius. Search only a bounded prefix and discard points preceding the
// closest route anchor, so recovery can never walk backwards through an
// ever-growing queue.
inline constexpr float kRemoteNavigationWaypointAnchorRadiusMeters = 2.0F;
inline constexpr std::size_t kRemoteNavigationWaypointAnchorSearchCount = 48U;
// The trail is only a local obstacle-detour hint, not a replay recording.
// Keeping minutes of history made a released puppet visit obsolete points
// across Valentine before it could chase the current authoritative target.
inline constexpr std::uint64_t kRemoteNavigationWaypointMaximumAgeMs = 8'000U;
inline constexpr std::size_t kRemoteNavigationWaypointCapacity = 128U;
inline constexpr float kRemoteNavigationWaypointLookAheadMeters = 2.0F;
inline constexpr float kRemoteNavigationAnchorMaximumAdvanceMeters = 4.0F;
inline constexpr float kRemoteNavigationDirectTargetErrorMeters = 15.0F;
inline constexpr std::uint64_t kRemoteNavigationDestinationRefreshMs = 750U;
inline constexpr std::uint64_t kRemoteNavigationUrgentRefreshMs = 300U;
inline constexpr float kRemoteNavigationDestinationDriftMeters = 6.0F;
inline constexpr std::uint64_t kRemoteNavigationMaximumActiveMs = 4'000U;
inline constexpr std::uint64_t kRemoteNavigationRetryCooldownMs = 750U;
inline constexpr float kRemoteNavigationSafeRecoveryMinimumErrorMeters = 6.0F;
inline constexpr float kRemoteNavigationSafeRecoveryMinimumDistanceMeters =
    0.75F;
// This is a last-resort rejoin after native pathfinding has already spent its
// full four-second budget. The previous 4.5 m ceiling could never catch a
// sprinting authoritative route: every timeout moved the proxy only one or two
// old waypoints while the route added another ~20 m. A recorded waypoint is a
// position the authoritative player actually occupied, so a bounded 40 m
// rejoin is safer than leaving the proxy permanently tens of metres behind.
inline constexpr float kRemoteNavigationSafeRecoveryMaximumDistanceMeters =
    40.0F;
// Position accuracy wins once animation-driven catch-up can no longer keep a
// replica near its authoritative marker. A short sustained threshold avoids
// reacting to one interpolation spike, while the emergency threshold bounds
// the time a stuck puppet can remain visibly tens of metres behind.
inline constexpr float kRemoteMotionHardResyncDistanceMeters = 6.0F;
inline constexpr std::uint64_t kRemoteMotionHardResyncSustainMs = 600U;
inline constexpr float kRemoteMotionEmergencyHardResyncDistanceMeters = 12.0F;
inline constexpr std::uint64_t kRemoteMotionEmergencyHardResyncSustainMs =
    200U;
inline constexpr std::uint64_t kRemoteMotionHardResyncCooldownMs = 2'000U;
inline constexpr float kRemoteMountHardCorrectionMeters = 12.0F;
inline constexpr float kRemoteTraversalApproachDistanceMeters = 3.0F;
inline constexpr float kRemoteTraversalActivationDistanceMeters = 1.25F;
// A stale traversal played many metres after its source action looks worse
// than an explicit route recovery. The pre-takeoff edge and reliable TCP
// transaction now make a short validity window sufficient.
inline constexpr std::uint64_t kRemoteTraversalMaximumAgeMs = 3'500U;
inline constexpr std::uint64_t kRemoteTraversalJumpTaskGuardMs = 400U;
inline constexpr std::uint64_t kRemoteTraversalClimbTaskGuardMs = 1'000U;
inline constexpr float kRemoteSnapshotBaseInterpolationDelayMs = 90.0F;
inline constexpr float kRemoteSnapshotMinimumInterpolationDelayMs = 75.0F;
inline constexpr float kRemoteSnapshotMaximumInterpolationDelayMs = 160.0F;
inline constexpr std::uint64_t kRemoteSnapshotMaximumExtrapolationMs = 200U;
inline constexpr std::uint64_t kRemoteSnapshotResetGapMs = 1'000U;
// Animation samples are an optional overlay on the authoritative transform
// stream. They must never outlive the transform interval they describe or be
// applied to a render point far away on the sender's monotonic timeline.
inline constexpr std::uint64_t kRemoteAnimationStateCacheTtlMs = 500U;
// Direct-root replication still needs a live RDR2 locomotion controller.
// FORCE_PED_MOTION_STATE on a taskless CREATE_PED proxy reports success but
// leaves its skeleton in a T-pose. A long, non-navmesh visual task keeps the
// native gait graph alive while network coordinates remain authoritative.
inline constexpr std::uint64_t kDirectReplicaVisualTaskRefreshMs = 8'000U;
inline constexpr std::uint64_t kDirectReplicaVisualTaskMinimumRefreshMs =
    250U;
inline constexpr float kDirectReplicaVisualTaskHeadingRefreshDegrees = 18.0F;
inline constexpr float kDirectReplicaTurnInPlaceHeadingDegrees = 6.0F;
inline constexpr std::uint64_t kDirectReplicaTraversalMaximumAgeMs = 3'500U;
inline constexpr float kDirectReplicaTraversalActivationDistanceMeters =
    1.25F;
enum class RemoteMotionMode {
    Hold,
    SmoothVelocity,
    Snap,
};

enum class RemoteLocomotion {
    Idle,
    Walk,
    Run,
    Sprint,
};

// Direction is expressed in the sender ped's local axes. RDR2's native
// heading uses 0 degrees at +Y and increases towards -X, which differs from
// the mathematical +X convention used by atan2(y, x). Keeping this explicit
// prevents forward motion from being misclassified as a 90-degree strafe.
enum class RemoteMovementDirection {
    None,
    Forward,
    ForwardRight,
    Right,
    BackwardRight,
    Backward,
    BackwardLeft,
    Left,
    ForwardLeft,
};

struct PedRelativeVelocity final {
    float forward{};
    float right{};
};

struct DirectReplicaVisualTaskRefreshInput final {
    bool hasTask{};
    RemoteLocomotion previousLocomotion{RemoteLocomotion::Idle};
    RemoteLocomotion desiredLocomotion{RemoteLocomotion::Idle};
    std::uint64_t taskAgeMs{};
    float headingDifferenceDegrees{};
    RemoteMovementDirection previousDirection{
        RemoteMovementDirection::None};
    RemoteMovementDirection desiredDirection{
        RemoteMovementDirection::None};
};

struct DirectReplicaTraversalStartInput final {
    std::uint64_t transactionAgeMs{};
    float horizontalDistanceMeters{};
    bool senderActionActive{};
    bool localPhysicalAnimation{};
    bool mounted{};
    bool taskGuardActive{};
};

enum class RemoteSnapshotSampleMode {
    Hold,
    Interpolated,
    Extrapolated,
    Frozen,
};

enum class PuppetControlMode {
    GroundedLocomotion,
    AimingLocomotion,
    TraversalApproach,
    TraversalCommitted,
    Airborne,
    RagdollOrLasso,
    Mounted,
    NavRecovery,
    HardResync,
};

struct RemoteMotionInput final {
    Vec3 currentPosition{};
    Vec3 currentVelocity{};
    Vec3 targetPosition{};
    Vec3 targetVelocity{};
    float currentHeading{};
    float targetHeading{};
    std::uint32_t elapsedMs{50U};
    RemoteLocomotion currentLocomotion{RemoteLocomotion::Idle};
    std::uint32_t locomotionAgeMs{kRemoteMotionMinimumGaitDwellMs};
    bool locomotionInitialized{};
    bool discontinuity{};
    float targetMovementHeading{};
    float targetDesiredMoveBlend{};
    bool hasSemanticIntent{};
};

struct RemoteMotionStep final {
    RemoteMotionMode mode{RemoteMotionMode::Hold};
    Vec3 position{};
    Vec3 velocity{};
    Vec3 snapPosition{};
    Vec3 taskDestination{};
    float heading{};
    float positionErrorMeters{};
    float targetHorizontalSpeed{};
    float moveBlendRatio{};
    float taskSpeed{};
    float moveRateOverride{1.0F};
    RemoteLocomotion locomotion{RemoteLocomotion::Idle};
    bool locomotionChanged{};
    bool catchUpActive{};
    bool headingFollowsDestination{};
};

struct RemoteLocomotionTaskRefreshInput final {
    bool forceRefresh{};
    bool hasTask{};
    RemoteLocomotion previousLocomotion{RemoteLocomotion::Idle};
    RemoteLocomotion desiredLocomotion{RemoteLocomotion::Idle};
    bool refreshExpired{};
    bool headingChanged{};
};

struct RemoteSnapshotSample final {
    PlayerStatePayload state{};
    RemoteSnapshotSampleMode mode{RemoteSnapshotSampleMode::Hold};
    std::uint64_t sourceAgeMs{};
    std::uint64_t senderTickMs{};
};

[[nodiscard]] bool IsFinite(const Vec3& value) noexcept;

// Converts world velocity to the heading convention consumed by RDR2's
// ENTITY/TASK natives. A nearly stationary sample retains fallbackHeading.
[[nodiscard]] float MovementHeadingFromVelocity(
    const Vec3& velocity,
    float fallbackHeading) noexcept;

// Projects world velocity into the ped's forward/right axes using RDR2's
// native heading convention (heading 0 points along +Y).
[[nodiscard]] PedRelativeVelocity ComputePedRelativeVelocity(
    const Vec3& velocity,
    float pedHeading) noexcept;

[[nodiscard]] RemoteMovementDirection ClassifyRemoteMovementDirection(
    float localForwardSpeed,
    float localRightSpeed) noexcept;

// Receive age bounds a stopped animation stream, while the sender-tick check
// prevents a current packet from being paired with a distant buffered render
// point. A zero sender tick means that only receive-age validation is
// available (used by old/synthetic loopback peers).
[[nodiscard]] bool IsRemoteAnimationStateFresh(
    std::uint64_t receivedAtMs,
    std::uint64_t animationSenderTickMs,
    std::uint64_t renderedSenderTickMs,
    std::uint64_t nowMs) noexcept;

// The visual gait controller yields completely to mounts and protected native
// physics. Ordinary coordinates are never written by this decision; bounded
// hard recovery is evaluated separately after sustained divergence.
[[nodiscard]] bool ShouldRunAnimGraphVisualController(
    bool mounted,
    bool protectedPhysicalAnimation) noexcept;

// Chooses the visual gait advertised to RDR2's native task graph. A validated
// sender state wins; desiredMoveBlend is a fail-closed fallback for recordings
// or peers that only carry the transform stream.
[[nodiscard]] RemoteLocomotion SelectDirectReplicaVisualLocomotion(
    std::optional<RemoteLocomotion> reportedLocomotion,
    float desiredMoveBlend) noexcept;

// The visual task is deliberately long and straight: it exists only to keep
// the native gait graph evaluating. It never owns the replicated root and it
// never invokes navmesh/pathfinding.
[[nodiscard]] Vec3 ComputeDirectReplicaVisualTaskDestination(
    const Vec3& authoritativePosition,
    const Vec3& authoritativeVelocity,
    float authoritativeHeading,
    RemoteLocomotion locomotion) noexcept;

[[nodiscard]] float DirectReplicaVisualTaskSpeed(
    RemoteLocomotion locomotion) noexcept;

[[nodiscard]] bool ShouldRefreshDirectReplicaVisualTask(
    const DirectReplicaVisualTaskRefreshInput& input) noexcept;

// Traversal is a short physical hand-off inside the direct-root engine. The
// reliable action may arrive before the delayed render point reaches its
// takeoff position, so either the matching semantic action or proximity to
// the recorded anchor may commit it. Locomotion resumes only after RDR2 yields
// the jump/climb task.
[[nodiscard]] bool ShouldStartDirectReplicaTraversal(
    const DirectReplicaTraversalStartInput& input) noexcept;

// A ledge fall has no jump transaction. Launch local physics once on the
// transition into the authoritative airborne mode, then let native falling
// own the skeleton until it lands.
[[nodiscard]] bool ShouldLaunchDirectReplicaAirborne(
    PlayerLocomotionMode previousMode,
    PlayerLocomotionMode desiredMode,
    bool localPhysicalAnimation,
    bool mounted) noexcept;

// RDR2's reported ped position is authoritative. Ground probing is used only
// to remove a small local height error (for example on a nearby slope); it
// must never pull a replica to another floor, a river bed, or distant terrain.
[[nodiscard]] Vec3 SelectGroundSafePosition(
    const Vec3& reportedPosition,
    std::optional<float> groundZ) noexcept;

// Produces a frame-rate-independent velocity for ordinary updates. A stale
// local replica receives a proportional physical velocity assist, while
// coordinate warping still requires an authoritative target discontinuity.
[[nodiscard]] RemoteMotionStep PlanRemoteMotion(
    const RemoteMotionInput& input) noexcept;

// Locomotion has already passed the planner's hysteresis and minimum dwell.
// Therefore a gait transition here is a confirmed animation-speed change and
// should replace the current movement task exactly once.
[[nodiscard]] bool ShouldRefreshRemoteLocomotionTask(
    const RemoteLocomotionTaskRefreshInput& input) noexcept;

// Separates root motion from weapon actions. A moving aim task may own the
// upper-body pose only while the replica is close to its network marker.
// Hysteresis prevents task ownership from flickering around one threshold.
[[nodiscard]] bool ShouldSuppressRemoteAimRoot(
    bool currentlySuppressed,
    bool aiming,
    bool mounted,
    float positionErrorMeters) noexcept;

// Hysteresis for the optional physical tracking layer. It must not repeatedly
// toggle when interpolation noise hovers near one threshold.
[[nodiscard]] bool ShouldApplyRemotePhysicsAssist(
    bool currentlyActive,
    float positionErrorMeters) noexcept;

// A blocked sample means that the authoritative target keeps moving while
// the visible ped does not, or that a large error is no longer improving.
[[nodiscard]] bool IsRemoteNavigationStalledSample(
    float positionErrorMeters,
    float pedTravelMeters,
    float targetTravelMeters,
    float errorImprovementMeters) noexcept;

// Navmesh recovery has a wide enter/exit hysteresis. It is never allowed to
// fight ragdoll, climbing physics or a mounted replica.
[[nodiscard]] bool ShouldUseRemoteNavigationRecovery(
    bool currentlyActive,
    float positionErrorMeters,
    std::uint64_t stalledForMs,
    bool physicsInterrupted,
    bool mounted) noexcept;

// Once a peer is far beyond a small local detour, native navmesh should route
// directly to its current location instead of replaying historical footsteps.
[[nodiscard]] bool ShouldUseDirectRemoteNavigationTarget(
    float positionErrorMeters) noexcept;

// A moving route must replace its navmesh destination much sooner than the
// task timeout. Urgent refresh still has a small floor to prevent restarting
// the native task every rendered frame.
[[nodiscard]] bool ShouldRefreshRemoteNavigationDestination(
    bool hasDestination,
    std::uint64_t destinationAgeMs,
    float destinationToCurrentTargetMeters,
    bool forceRefresh) noexcept;

// Recovery is a short obstacle-detour tool. It must yield after a bounded
// period so native pathfinding cannot suppress all catch-up motion for tens
// of seconds while following obsolete route points.
[[nodiscard]] bool HasRemoteNavigationRecoveryTimedOut(
    std::uint64_t activeForMs,
    bool physicsInterrupted) noexcept;

// A coordinate correction is the last step of obstacle recovery, never the
// ordinary tracking mechanism. It may advance to the newest valid recorded
// route waypoint inside a hard distance bound only after pathfinding has
// exhausted its bounded time budget.
[[nodiscard]] bool ShouldApplyRemoteNavigationSafeRecovery(
    bool navigationTimedOut,
    bool hasRouteDestination,
    float positionErrorMeters,
    float distanceToRouteDestinationMeters,
    bool physicsInterrupted,
    bool mounted) noexcept;

// A local replica that remains far behind is no longer representing the peer.
// Rejoin its current marker after a bounded grace period, but never interrupt
// ragdoll/climb physics, a mount relationship or an authoritative teleport
// already handled by the normal discontinuity path.
[[nodiscard]] bool ShouldApplyRemoteHardResync(
    float positionErrorMeters,
    std::uint64_t sustainedForMs,
    bool cooldownActive,
    bool physicsInterrupted,
    bool mounted,
    bool authoritativeDiscontinuity) noexcept;

// A mount's navmesh task owns all ordinary movement and hoof placement.
// Coordinate correction is legal only after emergency-scale divergence.
[[nodiscard]] bool ShouldApplyRemoteMountHardCorrection(
    float positionErrorMeters,
    float horizontalErrorMeters) noexcept;

// Traversal is recorded at the authoritative route position. If the visual
// proxy is behind, defer jump/climb until it reaches that position instead of
// playing the action several metres too early.
[[nodiscard]] bool ShouldExecuteDeferredRemoteTraversal(
    float distanceToActionMeters,
    std::uint64_t actionAgeMs,
    bool physicsInterrupted,
    bool reloading) noexcept;

// Selects a point ahead along the buffered route rather than extending the
// most recent velocity tangent. Curvature reduces the look-ahead so a sharp
// corner cannot aim the task through nearby geometry.
[[nodiscard]] float ComputeRemoteRouteLookAheadMeters(
    RemoteLocomotion locomotion,
    float alongRouteErrorMeters,
    float curvatureDegrees) noexcept;

// One primary owner may issue a full-body task at a time. This pure selector
// makes ownership order explicit and testable.
[[nodiscard]] PuppetControlMode SelectPuppetControlMode(
    PlayerLocomotionMode semanticMode,
    bool mounted,
    bool navigationActive,
    bool traversalApproach,
    bool traversalCommitted,
    bool physicsInterrupted,
    bool hardResync) noexcept;

// Buffers accepted snapshots on a receiver-local mapping of the sender's
// monotonic timeline. Arrival times are retained only for freshness and
// jitter estimation, so a delayed UDP datagram cannot distort route timing.
class RemoteSnapshotBuffer final {
public:
    [[nodiscard]] bool Push(
        const PlayerStatePayload& state,
        std::uint64_t receivedAtMs,
        std::uint64_t senderTickMs = 0U) noexcept;
    [[nodiscard]] std::optional<RemoteSnapshotSample> Sample(
        std::uint64_t nowMs) const noexcept;
    [[nodiscard]] float InterpolationDelayMs() const noexcept {
        return interpolationDelayMs_;
    }
    [[nodiscard]] float ArrivalJitterMs() const noexcept {
        return arrivalJitterMs_;
    }
    [[nodiscard]] std::uint64_t SenderTimelineResets() const noexcept {
        return senderTimelineResets_;
    }
    [[nodiscard]] std::size_t Size() const noexcept {
        return size_;
    }
    void Reset() noexcept;

private:
    struct TimedSnapshot final {
        PlayerStatePayload state{};
        std::uint64_t receivedAtMs{};
        std::uint64_t timelineAtMs{};
        std::uint64_t senderTickMs{};
    };

    static constexpr std::size_t kCapacity = 8U;
    std::array<TimedSnapshot, kCapacity> snapshots_{};
    std::size_t size_{};
    std::uint64_t senderAnchorTickMs_{};
    std::uint64_t receiverAnchorMs_{};
    std::uint64_t previousSenderTickMs_{};
    std::uint64_t previousArrivalMs_{};
    float interpolationDelayMs_{kRemoteSnapshotBaseInterpolationDelayMs};
    float arrivalJitterMs_{};
    std::uint64_t senderTimelineResets_{};
    bool hasSenderTimeline_{};
};

}  // namespace coopstory::bridge
