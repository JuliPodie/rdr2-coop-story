#include "coopstory/bridge/RemoteMotion.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace coopstory::bridge {
namespace {

[[nodiscard]] bool IsFinite(const float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] float NormalizeHeading(float heading) noexcept {
    if (!IsFinite(heading)) {
        return 0.0F;
    }
    heading = std::fmod(heading, 360.0F);
    return heading < 0.0F ? heading + 360.0F : heading;
}

[[nodiscard]] float SmoothHeading(
    const float current,
    const float target,
    const std::uint32_t elapsedMs,
    const float degreesPerSecond =
        kRemoteMotionHeadingSpeedDegreesPerSecond) noexcept {
    const auto normalizedCurrent = NormalizeHeading(current);
    const auto normalizedTarget = NormalizeHeading(target);
    auto difference = normalizedTarget - normalizedCurrent;
    if (difference > 180.0F) {
        difference -= 360.0F;
    } else if (difference < -180.0F) {
        difference += 360.0F;
    }

    const auto maximumStep =
        degreesPerSecond *
        (static_cast<float>(elapsedMs) / 1'000.0F);
    return NormalizeHeading(
        normalizedCurrent +
        std::clamp(difference, -maximumStep, maximumStep));
}

[[nodiscard]] float HeadingFromDirection(
    const float x,
    const float y,
    const float fallback) noexcept {
    if (!IsFinite(x) || !IsFinite(y) ||
        std::hypot(x, y) < 0.001F) {
        return NormalizeHeading(fallback);
    }
    return NormalizeHeading(
        std::atan2(-x, y) * (180.0F / std::numbers::pi_v<float>));
}

[[nodiscard]] float InterpolateHeading(
    const float from,
    const float to,
    const float amount) noexcept {
    const auto normalizedFrom = NormalizeHeading(from);
    const auto normalizedTo = NormalizeHeading(to);
    auto difference = normalizedTo - normalizedFrom;
    if (difference > 180.0F) {
        difference -= 360.0F;
    } else if (difference < -180.0F) {
        difference += 360.0F;
    }
    return NormalizeHeading(
        normalizedFrom + (difference * std::clamp(amount, 0.0F, 1.0F)));
}

[[nodiscard]] Vec3 StopHorizontalMotion(const Vec3& current) noexcept {
    return {
        0.0F,
        0.0F,
        IsFinite(current.z)
            ? std::clamp(
                  current.z,
                  -kRemoteMotionMaximumVerticalSpeed,
                  kRemoteMotionMaximumVerticalSpeed)
            : 0.0F};
}

[[nodiscard]] Vec3 ClampRemoteVelocity(const Vec3& velocity) noexcept {
    if (!IsFinite(velocity)) {
        return {};
    }

    auto result = velocity;
    const auto horizontalSpeed = std::hypot(result.x, result.y);
    if (horizontalSpeed > kRemoteMotionMaximumHorizontalSpeed) {
        const auto scale =
            kRemoteMotionMaximumHorizontalSpeed / horizontalSpeed;
        result.x *= scale;
        result.y *= scale;
    }
    result.z = std::clamp(
        result.z,
        -kRemoteMotionMaximumVerticalSpeed,
        kRemoteMotionMaximumVerticalSpeed);
    return result;
}

[[nodiscard]] Vec3 Lerp(
    const Vec3& from,
    const Vec3& to,
    const float amount) noexcept {
    const auto clamped = std::clamp(amount, 0.0F, 1.0F);
    return {
        from.x + ((to.x - from.x) * clamped),
        from.y + ((to.y - from.y) * clamped),
        from.z + ((to.z - from.z) * clamped)};
}

[[nodiscard]] Vec3 HermitePosition(
    const Vec3& from,
    const Vec3& fromVelocity,
    const Vec3& to,
    const Vec3& toVelocity,
    const float amount,
    const float intervalSeconds) noexcept {
    const auto t = std::clamp(amount, 0.0F, 1.0F);
    const auto t2 = t * t;
    const auto t3 = t2 * t;
    const auto h00 = (2.0F * t3) - (3.0F * t2) + 1.0F;
    const auto h10 = t3 - (2.0F * t2) + t;
    const auto h01 = (-2.0F * t3) + (3.0F * t2);
    const auto h11 = t3 - t2;
    Vec3 value{
        (h00 * from.x) + (h10 * intervalSeconds * fromVelocity.x) +
            (h01 * to.x) + (h11 * intervalSeconds * toVelocity.x),
        (h00 * from.y) + (h10 * intervalSeconds * fromVelocity.y) +
            (h01 * to.y) + (h11 * intervalSeconds * toVelocity.y),
        (h00 * from.z) + (h10 * intervalSeconds * fromVelocity.z) +
            (h01 * to.z) + (h11 * intervalSeconds * toVelocity.z)};

    // Bad velocity samples must not make the cubic curve overshoot through a
    // wall or floor. Keep it inside a small corridor around both endpoints.
    const auto clampAxis = [](const float sample,
                              const float first,
                              const float second) noexcept {
        const auto margin = std::max(
            0.20F,
            std::abs(second - first) * 0.25F);
        return std::clamp(
            sample,
            std::min(first, second) - margin,
            std::max(first, second) + margin);
    };
    value.x = clampAxis(value.x, from.x, to.x);
    value.y = clampAxis(value.y, from.y, to.y);
    value.z = clampAxis(value.z, from.z, to.z);
    return value;
}

[[nodiscard]] std::uint64_t Elapsed(
    const std::uint64_t previous,
    const std::uint64_t current) noexcept {
    return current >= previous ? current - previous : 0U;
}

[[nodiscard]] RemoteLocomotion InitialLocomotion(
    const float horizontalSpeed) noexcept {
    if (horizontalSpeed > kRemoteMotionRunToSprintMetersPerSecond) {
        return RemoteLocomotion::Sprint;
    }
    if (horizontalSpeed > kRemoteMotionWalkToRunMetersPerSecond) {
        return RemoteLocomotion::Run;
    }
    if (horizontalSpeed > kRemoteMotionIdleToWalkMetersPerSecond) {
        return RemoteLocomotion::Walk;
    }
    return RemoteLocomotion::Idle;
}

[[nodiscard]] RemoteLocomotion SelectLocomotion(
    const RemoteMotionInput& input,
    const float horizontalSpeed) noexcept {
    if (!input.locomotionInitialized) {
        return InitialLocomotion(horizontalSpeed);
    }
    if (input.locomotionAgeMs <
        kRemoteMotionMinimumGaitDwellMs) {
        return input.currentLocomotion;
    }

    switch (input.currentLocomotion) {
        case RemoteLocomotion::Idle:
            return InitialLocomotion(horizontalSpeed);
        case RemoteLocomotion::Walk:
            if (horizontalSpeed <
                kRemoteMotionWalkToIdleMetersPerSecond) {
                return RemoteLocomotion::Idle;
            }
            if (horizontalSpeed >
                kRemoteMotionRunToSprintMetersPerSecond) {
                return RemoteLocomotion::Sprint;
            }
            if (horizontalSpeed >
                kRemoteMotionWalkToRunMetersPerSecond) {
                return RemoteLocomotion::Run;
            }
            return RemoteLocomotion::Walk;
        case RemoteLocomotion::Run:
            if (horizontalSpeed <
                kRemoteMotionWalkToIdleMetersPerSecond) {
                return RemoteLocomotion::Idle;
            }
            if (horizontalSpeed >
                kRemoteMotionRunToSprintMetersPerSecond) {
                return RemoteLocomotion::Sprint;
            }
            if (horizontalSpeed <
                kRemoteMotionRunToWalkMetersPerSecond) {
                return RemoteLocomotion::Walk;
            }
            return RemoteLocomotion::Run;
        case RemoteLocomotion::Sprint:
            if (horizontalSpeed <
                kRemoteMotionWalkToIdleMetersPerSecond) {
                return RemoteLocomotion::Idle;
            }
            if (horizontalSpeed <
                kRemoteMotionRunToWalkMetersPerSecond) {
                return RemoteLocomotion::Walk;
            }
            if (horizontalSpeed <
                kRemoteMotionSprintToRunMetersPerSecond) {
                return RemoteLocomotion::Run;
            }
            return RemoteLocomotion::Sprint;
    }
    return RemoteLocomotion::Idle;
}

void ApplyLocomotion(
    RemoteMotionStep& step,
    const RemoteMotionInput& input) noexcept {
    const auto boundedTargetVelocity =
        ClampRemoteVelocity(input.targetVelocity);
    step.targetHorizontalSpeed = std::hypot(
        boundedTargetVelocity.x,
        boundedTargetVelocity.y);

    // Once coordinate nudges are removed, authoritative velocity alone is
    // not enough: an idle remote player would otherwise leave a delayed
    // proxy standing at the wrong position forever. Convert local separation
    // into a temporary visual gait and a small animation-rate boost. The ped
    // still reaches the target through native locomotion; no coordinates are
    // written here.
    step.catchUpActive =
        step.positionErrorMeters >
        kRemoteMotionCatchUpDeadZoneMeters;
    auto catchUpHorizontalSpeed = 0.0F;
    if (step.positionErrorMeters >=
        kRemoteMotionCatchUpSprintDistanceMeters) {
        catchUpHorizontalSpeed =
            kRemoteMotionCatchUpSprintSpeedMetersPerSecond;
    } else if (
        step.positionErrorMeters >=
        kRemoteMotionCatchUpRunDistanceMeters) {
        catchUpHorizontalSpeed =
            kRemoteMotionCatchUpRunSpeedMetersPerSecond;
    } else if (step.catchUpActive) {
        catchUpHorizontalSpeed = 1.0F;
    }
    const auto semanticHorizontalSpeed =
        !input.hasSemanticIntent
            ? 0.0F
            : input.targetDesiredMoveBlend >= 2.5F
                  ? 6.0F
                  : input.targetDesiredMoveBlend >= 1.5F
                        ? 3.0F
                        : input.targetDesiredMoveBlend >= 0.5F
                              ? 1.0F
                              : 0.0F;
    const auto controlHorizontalSpeed = std::max(
        {step.targetHorizontalSpeed,
         catchUpHorizontalSpeed,
         semanticHorizontalSpeed});
    if (step.positionErrorMeters >
        kRemoteMotionMoveRateBoostStartMeters) {
        step.moveRateOverride = std::clamp(
            1.0F +
                ((step.positionErrorMeters -
                  kRemoteMotionMoveRateBoostStartMeters) *
                 kRemoteMotionCatchUpMoveRateGainPerMeter),
            1.0F,
            kRemoteMotionCatchUpMaximumMoveRate);
    }
    step.locomotion = SelectLocomotion(
        input,
        controlHorizontalSpeed);
    step.locomotionChanged =
        step.locomotion != input.currentLocomotion;

    switch (step.locomotion) {
        case RemoteLocomotion::Idle:
            step.moveBlendRatio = 0.0F;
            step.taskSpeed = 0.0F;
            break;
        case RemoteLocomotion::Walk:
            step.moveBlendRatio = 1.0F;
            step.taskSpeed = 1.0F;
            break;
        case RemoteLocomotion::Run:
            step.moveBlendRatio = 2.0F;
            step.taskSpeed = 2.0F;
            break;
        case RemoteLocomotion::Sprint:
            step.moveBlendRatio = 3.0F;
            step.taskSpeed = 3.0F;
            break;
    }

    if (input.hasSemanticIntent) {
        step.moveBlendRatio = std::max(
            step.moveBlendRatio,
            std::clamp(input.targetDesiredMoveBlend, 0.0F, 3.0F));
    }

    // The facade replaces this checkpoint with a curvature-aware point from
    // its monotonic route queue. Never extend the velocity tangent here: on a
    // corner that line points through the outside wall.
    step.taskDestination = step.snapPosition;
}

[[nodiscard]] Vec3 PlanCatchUpVelocity(
    const RemoteMotionInput& input) noexcept {
    const auto errorX =
        input.targetPosition.x - input.currentPosition.x;
    const auto errorY =
        input.targetPosition.y - input.currentPosition.y;
    const auto errorZ =
        input.targetPosition.z - input.currentPosition.z;
    const auto horizontalError = std::hypot(errorX, errorY);
    auto desired = ClampRemoteVelocity(input.targetVelocity);
    if (horizontalError > kRemoteMotionCatchUpDeadZoneMeters) {
        desired.x += errorX * kRemoteMotionVelocityCorrectionGain;
        desired.y += errorY * kRemoteMotionVelocityCorrectionGain;
        desired = ClampRemoteVelocity(desired);
    }
    if (std::abs(errorZ) >
        kRemoteMotionCatchUpDeadZoneMeters) {
        desired.z +=
            errorZ * kRemoteMotionVerticalCorrectionGain;
        desired = ClampRemoteVelocity(desired);
    }

    // Smooth the command carried by the facade, not the engine-reported
    // velocity that native locomotion may reset between frames. This retains
    // V9.1's reliable catch-up while removing the visible jump from ordinary
    // gait speed straight to the 12 m/s ceiling.
    const auto boundedElapsedMs = std::clamp<std::uint32_t>(
        input.elapsedMs,
        1U,
        50U);
    const auto seconds =
        static_cast<float>(boundedElapsedMs) / 1'000.0F;
    auto result = ClampRemoteVelocity(input.currentVelocity);
    const auto deltaX = desired.x - result.x;
    const auto deltaY = desired.y - result.y;
    const auto deltaLength = std::hypot(deltaX, deltaY);
    const auto maximumDelta =
        kRemoteMotionMaximumAccelerationMetersPerSecond2 * seconds;
    if (deltaLength > maximumDelta && deltaLength > 0.0F) {
        const auto scale = maximumDelta / deltaLength;
        result.x += deltaX * scale;
        result.y += deltaY * scale;
    } else {
        result.x = desired.x;
        result.y = desired.y;
    }
    const auto maximumVerticalDelta =
        kRemoteMotionMaximumVerticalAccelerationMetersPerSecond2 * seconds;
    result.z += std::clamp(
        desired.z - result.z,
        -maximumVerticalDelta,
        maximumVerticalDelta);
    return result;
}

[[nodiscard]] bool IsValidSnapshot(
    const PlayerStatePayload& state) noexcept {
    return state.entityId.IsValid() &&
           IsFinite(state.position) &&
           IsFinite(state.velocity) &&
           IsFinite(state.heading) &&
           IsFinite(state.healthFraction) &&
           IsFinite(state.movementHeading) &&
           IsFinite(state.localForwardSpeed) &&
           IsFinite(state.localRightSpeed) &&
           std::abs(state.localForwardSpeed) <= 50.0F &&
           std::abs(state.localRightSpeed) <= 50.0F &&
           IsFinite(state.desiredMoveBlend) &&
           state.desiredMoveBlend >= 0.0F &&
           state.desiredMoveBlend <= 3.0F &&
           static_cast<std::uint8_t>(state.traversalKind) <=
               static_cast<std::uint8_t>(PlayerTraversalKind::Climb) &&
           static_cast<std::uint8_t>(state.locomotionMode) <=
               static_cast<std::uint8_t>(PlayerLocomotionMode::Mounted) &&
           IsFinite(state.traversalAnchor) &&
           IsFinite(state.traversalHeading) &&
           (state.traversalKind == PlayerTraversalKind::None ||
            state.traversalActionId != 0U);
}

[[nodiscard]] std::uint64_t MaximumExtrapolationFor(
    const PlayerStatePayload& state) noexcept {
    switch (state.locomotionMode) {
        case PlayerLocomotionMode::Traversal:
        case PlayerLocomotionMode::Airborne:
        case PlayerLocomotionMode::Ragdoll:
            return 75U;
        case PlayerLocomotionMode::Mounted:
            return 125U;
        case PlayerLocomotionMode::Aiming:
            return 125U;
        case PlayerLocomotionMode::Grounded:
            break;
    }
    if (state.desiredMoveBlend >= 2.5F) {
        return 125U;
    }
    if (std::hypot(state.velocity.x, state.velocity.y) < 0.20F) {
        return kRemoteSnapshotMaximumExtrapolationMs;
    }
    return 150U;
}

}  // namespace

bool IsRemoteAnimationStateFresh(
    const std::uint64_t receivedAtMs,
    const std::uint64_t animationSenderTickMs,
    const std::uint64_t renderedSenderTickMs,
    const std::uint64_t nowMs) noexcept {
    if (nowMs < receivedAtMs ||
        nowMs - receivedAtMs > kRemoteAnimationStateCacheTtlMs) {
        return false;
    }
    if (animationSenderTickMs == 0U || renderedSenderTickMs == 0U) {
        return true;
    }
    const auto tickDistance =
        animationSenderTickMs >= renderedSenderTickMs
            ? animationSenderTickMs - renderedSenderTickMs
            : renderedSenderTickMs - animationSenderTickMs;
    return tickDistance <= kRemoteAnimationStateCacheTtlMs;
}

bool ShouldApplyAnimGraphDirectRootCorrection(
    const bool mounted,
    const bool protectedPhysicalAnimation) noexcept {
    return !mounted && !protectedPhysicalAnimation;
}

bool ShouldApplyDirectReplicaPhysicalRootLeash(
    const bool mounted,
    const bool protectedPhysicalAnimation,
    const float positionErrorMeters) noexcept {
    return !mounted && protectedPhysicalAnimation &&
           IsFinite(positionErrorMeters) && positionErrorMeters >= 0.0F &&
           positionErrorMeters >= kDirectReplicaPhysicalRootLeashMeters;
}

RemoteLocomotion SelectDirectReplicaVisualLocomotion(
    const std::optional<RemoteLocomotion> reportedLocomotion,
    const float desiredMoveBlend) noexcept {
    if (reportedLocomotion.has_value()) {
        return *reportedLocomotion;
    }
    if (!IsFinite(desiredMoveBlend) || desiredMoveBlend < 0.10F) {
        return RemoteLocomotion::Idle;
    }
    if (desiredMoveBlend < 1.50F) {
        return RemoteLocomotion::Walk;
    }
    if (desiredMoveBlend < 2.50F) {
        return RemoteLocomotion::Run;
    }
    return RemoteLocomotion::Sprint;
}

Vec3 ComputeDirectReplicaVisualTaskDestination(
    const Vec3& authoritativePosition,
    const Vec3& authoritativeVelocity,
    const float authoritativeHeading,
    const RemoteLocomotion locomotion) noexcept {
    if (!IsFinite(authoritativePosition) ||
        locomotion == RemoteLocomotion::Idle) {
        return authoritativePosition;
    }

    auto directionX = authoritativeVelocity.x;
    auto directionY = authoritativeVelocity.y;
    const auto horizontalSpeed =
        IsFinite(authoritativeVelocity)
            ? std::hypot(directionX, directionY)
            : 0.0F;
    if (horizontalSpeed >= 0.20F) {
        directionX /= horizontalSpeed;
        directionY /= horizontalSpeed;
    } else {
        const auto heading = NormalizeHeading(authoritativeHeading) *
            (std::numbers::pi_v<float> / 180.0F);
        directionX = -std::sin(heading);
        directionY = std::cos(heading);
    }

    const auto lookAhead =
        locomotion == RemoteLocomotion::Walk
            ? 8.0F
            : locomotion == RemoteLocomotion::Run
                  ? 14.0F
                  : 20.0F;
    return {
        authoritativePosition.x + (directionX * lookAhead),
        authoritativePosition.y + (directionY * lookAhead),
        authoritativePosition.z};
}

float DirectReplicaVisualTaskSpeed(
    const RemoteLocomotion locomotion) noexcept {
    switch (locomotion) {
        case RemoteLocomotion::Idle:
            return 0.0F;
        case RemoteLocomotion::Walk:
            return 1.0F;
        case RemoteLocomotion::Run:
            return 2.0F;
        case RemoteLocomotion::Sprint:
            return 3.0F;
    }
    return 0.0F;
}

bool ShouldRefreshDirectReplicaVisualTask(
    const DirectReplicaVisualTaskRefreshInput& input) noexcept {
    if (!input.hasTask ||
        input.previousLocomotion != input.desiredLocomotion) {
        return true;
    }
    if (input.taskAgeMs >= kDirectReplicaVisualTaskRefreshMs) {
        return true;
    }
    if (input.taskAgeMs <
        kDirectReplicaVisualTaskMinimumRefreshMs) {
        return false;
    }
    if (input.previousDirection != input.desiredDirection) {
        return true;
    }
    if (!IsFinite(input.headingDifferenceDegrees)) {
        return false;
    }
    const auto threshold =
        input.desiredLocomotion == RemoteLocomotion::Idle
            ? kDirectReplicaTurnInPlaceHeadingDegrees
            : kDirectReplicaVisualTaskHeadingRefreshDegrees;
    return std::abs(input.headingDifferenceDegrees) >= threshold;
}

bool ShouldStartDirectReplicaTraversal(
    const DirectReplicaTraversalStartInput& input) noexcept {
    if (input.mounted || input.localPhysicalAnimation ||
        input.taskGuardActive ||
        input.transactionAgeMs >
            kDirectReplicaTraversalMaximumAgeMs ||
        !IsFinite(input.horizontalDistanceMeters) ||
        input.horizontalDistanceMeters < 0.0F) {
        return false;
    }
    return input.senderActionActive ||
           input.horizontalDistanceMeters <=
               kDirectReplicaTraversalActivationDistanceMeters;
}

bool ShouldLaunchDirectReplicaAirborne(
    const PlayerLocomotionMode previousMode,
    const PlayerLocomotionMode desiredMode,
    const bool localPhysicalAnimation,
    const bool mounted) noexcept {
    return !mounted && !localPhysicalAnimation &&
           desiredMode == PlayerLocomotionMode::Airborne &&
           previousMode != PlayerLocomotionMode::Airborne;
}

bool IsFinite(const Vec3& value) noexcept {
    return IsFinite(value.x) &&
           IsFinite(value.y) &&
           IsFinite(value.z);
}

float MovementHeadingFromVelocity(
    const Vec3& velocity,
    const float fallbackHeading) noexcept {
    if (!IsFinite(velocity) ||
        std::hypot(velocity.x, velocity.y) < 0.10F) {
        return NormalizeHeading(fallbackHeading);
    }
    return NormalizeHeading(
        std::atan2(-velocity.x, velocity.y) *
        (180.0F / std::numbers::pi_v<float>));
}

PedRelativeVelocity ComputePedRelativeVelocity(
    const Vec3& velocity,
    const float pedHeading) noexcept {
    if (!IsFinite(velocity) || !IsFinite(pedHeading)) {
        return {};
    }
    const auto heading =
        NormalizeHeading(pedHeading) *
        (std::numbers::pi_v<float> / 180.0F);
    const auto forwardX = -std::sin(heading);
    const auto forwardY = std::cos(heading);
    const auto rightX = std::cos(heading);
    const auto rightY = std::sin(heading);
    return {
        (velocity.x * forwardX) + (velocity.y * forwardY),
        (velocity.x * rightX) + (velocity.y * rightY)};
}

RemoteMovementDirection ClassifyRemoteMovementDirection(
    const float localForwardSpeed,
    const float localRightSpeed) noexcept {
    if (!IsFinite(localForwardSpeed) ||
        !IsFinite(localRightSpeed) ||
        std::hypot(localForwardSpeed, localRightSpeed) < 0.20F) {
        return RemoteMovementDirection::None;
    }

    const auto angle =
        std::atan2(localRightSpeed, localForwardSpeed) *
        (180.0F / std::numbers::pi_v<float>);
    if (angle >= -22.5F && angle < 22.5F) {
        return RemoteMovementDirection::Forward;
    }
    if (angle >= 22.5F && angle < 67.5F) {
        return RemoteMovementDirection::ForwardRight;
    }
    if (angle >= 67.5F && angle < 112.5F) {
        return RemoteMovementDirection::Right;
    }
    if (angle >= 112.5F && angle < 157.5F) {
        return RemoteMovementDirection::BackwardRight;
    }
    if (angle >= 157.5F || angle < -157.5F) {
        return RemoteMovementDirection::Backward;
    }
    if (angle >= -157.5F && angle < -112.5F) {
        return RemoteMovementDirection::BackwardLeft;
    }
    if (angle >= -112.5F && angle < -67.5F) {
        return RemoteMovementDirection::Left;
    }
    return RemoteMovementDirection::ForwardLeft;
}

Vec3 SelectGroundSafePosition(
    const Vec3& reportedPosition,
    const std::optional<float> groundZ) noexcept {
    if (!IsFinite(reportedPosition) ||
        !groundZ.has_value() ||
        !IsFinite(*groundZ) ||
        std::abs(*groundZ - reportedPosition.z) >
            kRemoteMotionMaximumGroundCorrectionMeters) {
        return reportedPosition;
    }
    return {
        reportedPosition.x,
        reportedPosition.y,
        *groundZ};
}

RemoteMotionStep PlanRemoteMotion(
    const RemoteMotionInput& input) noexcept {
    RemoteMotionStep step;
    step.position =
        IsFinite(input.currentPosition)
            ? input.currentPosition
            : Vec3{};
    step.snapPosition = input.targetPosition;
    step.heading =
        IsFinite(input.currentHeading)
            ? NormalizeHeading(input.currentHeading)
            : 0.0F;
    step.velocity = StopHorizontalMotion(input.currentVelocity);

    if (!IsFinite(input.currentPosition) ||
        !IsFinite(input.targetPosition) ||
        !IsFinite(input.targetVelocity) ||
        !IsFinite(input.targetHeading)) {
        return step;
    }

    const auto separation =
        Distance(input.currentPosition, input.targetPosition);
    if (!IsFinite(separation)) {
        return step;
    }
    step.positionErrorMeters = separation;
    ApplyLocomotion(step, input);

    if (input.discontinuity &&
        separation >= kRemoteMotionSnapDistanceMeters) {
        step.mode = RemoteMotionMode::Snap;
        step.position = input.targetPosition;
        step.heading = NormalizeHeading(input.targetHeading);
        step.velocity = ClampRemoteVelocity(input.targetVelocity);
        step.velocity.z = 0.0F;
        return step;
    }

    step.mode = RemoteMotionMode::SmoothVelocity;
    step.velocity = PlanCatchUpVelocity(input);
    const auto boundedElapsedMs = std::clamp<std::uint32_t>(
        input.elapsedMs,
        1U,
        50U);
    const auto seconds =
        static_cast<float>(boundedElapsedMs) / 1'000.0F;
    step.position = {
        input.currentPosition.x + (step.velocity.x * seconds),
        input.currentPosition.y + (step.velocity.y * seconds),
        input.currentPosition.z + (step.velocity.z * seconds)};
    auto desiredHeading =
        input.hasSemanticIntent
            ? input.targetMovementHeading
            : input.targetHeading;
    if (separation >= kRemoteMotionRootHeadingDistanceMeters) {
        desiredHeading = HeadingFromDirection(
            step.taskDestination.x - input.currentPosition.x,
            step.taskDestination.y - input.currentPosition.y,
            input.targetHeading);
        step.headingFollowsDestination = true;
    }
    step.heading = SmoothHeading(
        input.currentHeading,
        desiredHeading,
        input.elapsedMs,
        step.headingFollowsDestination
            ? kRemoteMotionCatchUpHeadingSpeedDegreesPerSecond
            : kRemoteMotionHeadingSpeedDegreesPerSecond);
    return step;
}

bool ShouldRefreshRemoteLocomotionTask(
    const RemoteLocomotionTaskRefreshInput& input) noexcept {
    return input.forceRefresh ||
           !input.hasTask ||
           input.previousLocomotion !=
               input.desiredLocomotion ||
           input.refreshExpired ||
           input.headingChanged;
}

bool ShouldSuppressRemoteAimRoot(
    const bool currentlySuppressed,
    const bool aiming,
    const bool mounted,
    const float positionErrorMeters) noexcept {
    if (!aiming || mounted) {
        return false;
    }
    if (!IsFinite(positionErrorMeters)) {
        return currentlySuppressed;
    }
    if (currentlySuppressed) {
        return positionErrorMeters >
               kRemoteAimRootSuppressExitMeters;
    }
    return positionErrorMeters >=
           kRemoteAimRootSuppressEnterMeters;
}

bool ShouldApplyRemotePhysicsAssist(
    const bool currentlyActive,
    const float positionErrorMeters) noexcept {
    if (!IsFinite(positionErrorMeters) || positionErrorMeters < 0.0F) {
        return false;
    }
    if (currentlyActive) {
        return positionErrorMeters >
               kRemoteMotionPhysicsAssistExitDistanceMeters;
    }
    return positionErrorMeters >=
           kRemoteMotionPhysicsAssistEnterDistanceMeters;
}

bool IsRemoteNavigationStalledSample(
    const float positionErrorMeters,
    const float pedTravelMeters,
    const float targetTravelMeters,
    const float errorImprovementMeters) noexcept {
    if (!IsFinite(positionErrorMeters) ||
        !IsFinite(pedTravelMeters) ||
        !IsFinite(targetTravelMeters) ||
        !IsFinite(errorImprovementMeters) ||
        positionErrorMeters < kRemoteNavigationEnterErrorMeters ||
        pedTravelMeters < 0.0F ||
        targetTravelMeters < 0.0F) {
        return false;
    }
    const auto targetEscapingStationaryPed =
        targetTravelMeters >=
            kRemoteNavigationMinimumTargetTravelMeters &&
        pedTravelMeters <
            kRemoteNavigationMinimumPedTravelMeters;
    const auto largeErrorNotImproving =
        positionErrorMeters >=
            kRemoteNavigationLargeErrorMeters &&
        errorImprovementMeters <
            kRemoteNavigationMinimumErrorImprovementMeters;
    return targetEscapingStationaryPed || largeErrorNotImproving;
}

bool ShouldUseRemoteNavigationRecovery(
    const bool currentlyActive,
    const float positionErrorMeters,
    const std::uint64_t stalledForMs,
    const bool physicsInterrupted,
    const bool mounted) noexcept {
    if (mounted ||
        !IsFinite(positionErrorMeters) ||
        positionErrorMeters < 0.0F) {
        return false;
    }
    if (physicsInterrupted) {
        // Pause the active recovery while ragdoll/jump/climb owns the task
        // graph. Retaining the mode prevents a sheriff takedown from creating
        // a false exit/re-enter pair and losing route progress.
        return currentlyActive;
    }
    if (currentlyActive) {
        return positionErrorMeters >
               kRemoteNavigationExitErrorMeters;
    }
    return positionErrorMeters >=
               kRemoteNavigationEnterErrorMeters &&
           stalledForMs >= kRemoteNavigationStallEnterMs;
}

bool ShouldUseDirectRemoteNavigationTarget(
    const float positionErrorMeters) noexcept {
    return IsFinite(positionErrorMeters) &&
           positionErrorMeters >=
               kRemoteNavigationDirectTargetErrorMeters;
}

bool ShouldRefreshRemoteNavigationDestination(
    const bool hasDestination,
    const std::uint64_t destinationAgeMs,
    const float destinationToCurrentTargetMeters,
    const bool forceRefresh) noexcept {
    if (!hasDestination || forceRefresh) {
        return true;
    }
    if (destinationAgeMs >=
        kRemoteNavigationDestinationRefreshMs) {
        return true;
    }
    return IsFinite(destinationToCurrentTargetMeters) &&
           destinationToCurrentTargetMeters >=
               kRemoteNavigationDestinationDriftMeters &&
           destinationAgeMs >=
               kRemoteNavigationUrgentRefreshMs;
}

bool HasRemoteNavigationRecoveryTimedOut(
    const std::uint64_t activeForMs,
    const bool physicsInterrupted) noexcept {
    return !physicsInterrupted &&
           activeForMs >= kRemoteNavigationMaximumActiveMs;
}

bool ShouldApplyRemoteNavigationSafeRecovery(
    const bool navigationTimedOut,
    const bool hasRouteDestination,
    const float positionErrorMeters,
    const float distanceToRouteDestinationMeters,
    const bool physicsInterrupted,
    const bool mounted) noexcept {
    return navigationTimedOut &&
           hasRouteDestination &&
           IsFinite(positionErrorMeters) &&
           positionErrorMeters >=
               kRemoteNavigationSafeRecoveryMinimumErrorMeters &&
           IsFinite(distanceToRouteDestinationMeters) &&
           distanceToRouteDestinationMeters >=
               kRemoteNavigationSafeRecoveryMinimumDistanceMeters &&
           distanceToRouteDestinationMeters <=
               kRemoteNavigationSafeRecoveryMaximumDistanceMeters &&
           !physicsInterrupted &&
           !mounted;
}

bool ShouldApplyRemoteHardResync(
    const float positionErrorMeters,
    const std::uint64_t sustainedForMs,
    const bool cooldownActive,
    const bool physicsInterrupted,
    const bool mounted,
    const bool authoritativeDiscontinuity) noexcept {
    if (!IsFinite(positionErrorMeters) ||
        positionErrorMeters < kRemoteMotionHardResyncDistanceMeters ||
        cooldownActive || physicsInterrupted || mounted ||
        authoritativeDiscontinuity) {
        return false;
    }
    if (positionErrorMeters >=
        kRemoteMotionEmergencyHardResyncDistanceMeters) {
        return sustainedForMs >=
               kRemoteMotionEmergencyHardResyncSustainMs;
    }
    return sustainedForMs >= kRemoteMotionHardResyncSustainMs;
}

bool ShouldExecuteDeferredRemoteTraversal(
    const float distanceToActionMeters,
    const std::uint64_t actionAgeMs,
    const bool physicsInterrupted,
    const bool reloading) noexcept {
    return IsFinite(distanceToActionMeters) &&
           distanceToActionMeters >= 0.0F &&
           distanceToActionMeters <=
               kRemoteTraversalActivationDistanceMeters &&
           actionAgeMs <= kRemoteTraversalMaximumAgeMs &&
           !physicsInterrupted &&
           !reloading;
}

float ComputeRemoteRouteLookAheadMeters(
    const RemoteLocomotion locomotion,
    const float alongRouteErrorMeters,
    const float curvatureDegrees) noexcept {
    float base{};
    switch (locomotion) {
        case RemoteLocomotion::Idle:
            base = 0.40F;
            break;
        case RemoteLocomotion::Walk:
            base = 0.75F;
            break;
        case RemoteLocomotion::Run:
            base = 1.10F;
            break;
        case RemoteLocomotion::Sprint:
            base = 1.50F;
            break;
    }
    const auto catchUp =
        IsFinite(alongRouteErrorMeters)
            ? std::clamp(alongRouteErrorMeters * 0.35F, 0.0F, 0.75F)
            : 0.0F;
    const auto curve =
        IsFinite(curvatureDegrees)
            ? std::clamp(std::abs(curvatureDegrees), 0.0F, 180.0F)
            : 0.0F;
    const auto curveScale =
        curve >= 90.0F
            ? 0.45F
            : curve >= 45.0F
                  ? 0.65F
                  : curve >= 20.0F
                        ? 0.80F
                        : 1.0F;
    return std::clamp((base + catchUp) * curveScale, 0.40F, 2.0F);
}

PuppetControlMode SelectPuppetControlMode(
    const PlayerLocomotionMode semanticMode,
    const bool mounted,
    const bool navigationActive,
    const bool traversalApproach,
    const bool traversalCommitted,
    const bool physicsInterrupted,
    const bool hardResync) noexcept {
    if (hardResync) {
        return PuppetControlMode::HardResync;
    }
    if (mounted || semanticMode == PlayerLocomotionMode::Mounted) {
        return PuppetControlMode::Mounted;
    }
    if (traversalCommitted) {
        return PuppetControlMode::TraversalCommitted;
    }
    if (physicsInterrupted) {
        return semanticMode == PlayerLocomotionMode::Ragdoll
                   ? PuppetControlMode::RagdollOrLasso
                   : PuppetControlMode::Airborne;
    }
    if (traversalApproach) {
        return PuppetControlMode::TraversalApproach;
    }
    if (navigationActive) {
        return PuppetControlMode::NavRecovery;
    }
    if (semanticMode == PlayerLocomotionMode::Aiming) {
        return PuppetControlMode::AimingLocomotion;
    }
    return PuppetControlMode::GroundedLocomotion;
}

bool RemoteSnapshotBuffer::Push(
    const PlayerStatePayload& state,
    const std::uint64_t receivedAtMs,
    const std::uint64_t senderTickMs) noexcept {
    if (!IsValidSnapshot(state)) {
        return false;
    }

    const auto establishSenderTimeline = [&]() noexcept {
        hasSenderTimeline_ = senderTickMs != 0U;
        senderAnchorTickMs_ = senderTickMs;
        receiverAnchorMs_ = receivedAtMs;
        previousSenderTickMs_ = senderTickMs;
        previousArrivalMs_ = receivedAtMs;
    };

    auto timelineAtMs = receivedAtMs;
    if (senderTickMs != 0U) {
        if (!hasSenderTimeline_) {
            establishSenderTimeline();
        } else {
            const bool senderWentBackwards =
                senderTickMs < previousSenderTickMs_;
            const auto senderGap =
                senderWentBackwards
                    ? std::uint64_t{0}
                    : senderTickMs - previousSenderTickMs_;
            const auto arrivalGap =
                receivedAtMs >= previousArrivalMs_
                    ? receivedAtMs - previousArrivalMs_
                    : std::uint64_t{0};
            if (senderWentBackwards ||
                senderGap > kRemoteSnapshotResetGapMs * 4U ||
                receivedAtMs < previousArrivalMs_) {
                ++senderTimelineResets_;
                establishSenderTimeline();
            } else {
                const auto intervalError = std::abs(
                    static_cast<double>(arrivalGap) -
                    static_cast<double>(senderGap));
                arrivalJitterMs_ =
                    (arrivalJitterMs_ * 0.90F) +
                    (static_cast<float>(intervalError) * 0.10F);
                const auto targetDelay = std::clamp(
                    kRemoteSnapshotBaseInterpolationDelayMs +
                        (arrivalJitterMs_ * 2.5F),
                    kRemoteSnapshotMinimumInterpolationDelayMs,
                    kRemoteSnapshotMaximumInterpolationDelayMs);
                const auto delayAlpha =
                    targetDelay > interpolationDelayMs_
                        ? 0.25F
                        : 0.05F;
                interpolationDelayMs_ +=
                    (targetDelay - interpolationDelayMs_) * delayAlpha;
                previousSenderTickMs_ = senderTickMs;
                previousArrivalMs_ = receivedAtMs;
            }
        }
        timelineAtMs =
            receiverAnchorMs_ +
            (senderTickMs - senderAnchorTickMs_);
        const bool mappedFarIntoFuture =
            timelineAtMs > receivedAtMs + 100U;
        const bool mappedFarBehind =
            receivedAtMs > timelineAtMs + 750U;
        if (mappedFarIntoFuture || mappedFarBehind) {
            ++senderTimelineResets_;
            establishSenderTimeline();
            timelineAtMs = receivedAtMs;
        }
    }

    if (size_ == 0U) {
        snapshots_[0U] = TimedSnapshot{
            state,
            receivedAtMs,
            timelineAtMs,
            senderTickMs};
        size_ = 1U;
        return true;
    }

    const auto& latest = snapshots_[size_ - 1U];
    const bool changedIdentity =
        latest.state.entityId != state.entityId ||
        latest.state.slot != state.slot;
    const bool changedLocomotionEpoch =
        latest.state.locomotionEpoch != 0U &&
        state.locomotionEpoch != 0U &&
        latest.state.locomotionEpoch != state.locomotionEpoch;
    if (receivedAtMs < latest.receivedAtMs) {
        return false;
    }
    const bool longGap =
        Elapsed(latest.receivedAtMs, receivedAtMs) >
        kRemoteSnapshotResetGapMs;
    const bool spatialDiscontinuity =
        Distance(
            latest.state.position,
            state.position) >=
        kRemoteMotionSnapDistanceMeters;
    if (changedIdentity || changedLocomotionEpoch ||
        longGap || spatialDiscontinuity) {
        const auto timelineResetCount = senderTimelineResets_;
        Reset();
        senderTimelineResets_ = timelineResetCount;
        establishSenderTimeline();
        snapshots_[0U] = TimedSnapshot{
            state,
            receivedAtMs,
            receivedAtMs,
            senderTickMs};
        size_ = 1U;
        return true;
    }

    if (timelineAtMs < latest.timelineAtMs) {
        return false;
    }
    if (timelineAtMs == latest.timelineAtMs) {
        snapshots_[size_ - 1U] =
            TimedSnapshot{
                state,
                receivedAtMs,
                timelineAtMs,
                senderTickMs};
        return true;
    }

    if (size_ == kCapacity) {
        std::move(
            snapshots_.begin() + 1,
            snapshots_.end(),
            snapshots_.begin());
        --size_;
    }
    snapshots_[size_] = TimedSnapshot{
        state,
        receivedAtMs,
        timelineAtMs,
        senderTickMs};
    ++size_;
    return true;
}

std::optional<RemoteSnapshotSample> RemoteSnapshotBuffer::Sample(
    const std::uint64_t nowMs) const noexcept {
    if (size_ == 0U) {
        return std::nullopt;
    }

    const auto& newest = snapshots_[size_ - 1U];
    RemoteSnapshotSample sample;
    sample.state = newest.state;
    sample.sourceAgeMs =
        Elapsed(newest.receivedAtMs, nowMs);
    sample.senderTickMs = newest.senderTickMs;

    if (size_ == 1U) {
        sample.mode = RemoteSnapshotSampleMode::Hold;
        return sample;
    }

    const auto interpolationDelay =
        static_cast<std::uint64_t>(
            std::lround(interpolationDelayMs_));
    const auto renderAtMs =
        nowMs > interpolationDelay
            ? nowMs - interpolationDelay
            : 0U;
    const auto& oldest = snapshots_[0U];
    if (renderAtMs <= oldest.timelineAtMs) {
        sample.state = oldest.state;
        sample.mode = RemoteSnapshotSampleMode::Hold;
        sample.sourceAgeMs =
            Elapsed(oldest.receivedAtMs, nowMs);
        sample.senderTickMs = oldest.senderTickMs;
        return sample;
    }

    for (std::size_t index = 1U; index < size_; ++index) {
        const auto& to = snapshots_[index];
        if (renderAtMs > to.timelineAtMs) {
            continue;
        }
        const auto& from = snapshots_[index - 1U];
        const auto interval =
            to.timelineAtMs - from.timelineAtMs;
        const auto amount =
            static_cast<float>(
                renderAtMs - from.timelineAtMs) /
            static_cast<float>(interval);
        sample.state = to.state;
        sample.state.position = HermitePosition(
            from.state.position,
            ClampRemoteVelocity(from.state.velocity),
            to.state.position,
            ClampRemoteVelocity(to.state.velocity),
            amount,
            static_cast<float>(interval) / 1'000.0F);
        sample.state.velocity = ClampRemoteVelocity(
            Lerp(
                from.state.velocity,
                to.state.velocity,
                amount));
        sample.state.heading = InterpolateHeading(
            from.state.heading,
            to.state.heading,
            amount);
        sample.state.movementHeading = InterpolateHeading(
            from.state.movementHeading,
            to.state.movementHeading,
            amount);
        sample.state.localForwardSpeed =
            from.state.localForwardSpeed +
            ((to.state.localForwardSpeed -
              from.state.localForwardSpeed) * amount);
        sample.state.localRightSpeed =
            from.state.localRightSpeed +
            ((to.state.localRightSpeed -
              from.state.localRightSpeed) * amount);
        sample.state.desiredMoveBlend = std::clamp(
            from.state.desiredMoveBlend +
                ((to.state.desiredMoveBlend -
                  from.state.desiredMoveBlend) * amount),
            0.0F,
            3.0F);
        sample.state.healthFraction = std::clamp(
            from.state.healthFraction +
                ((to.state.healthFraction -
                  from.state.healthFraction) *
                 std::clamp(amount, 0.0F, 1.0F)),
            0.0F,
            1.0F);
        if (from.senderTickMs != 0U &&
            to.senderTickMs >= from.senderTickMs) {
            sample.senderTickMs =
                from.senderTickMs +
                static_cast<std::uint64_t>(std::lround(
                    static_cast<double>(
                        to.senderTickMs - from.senderTickMs) *
                    static_cast<double>(
                        std::clamp(amount, 0.0F, 1.0F))));
        } else {
            sample.senderTickMs = to.senderTickMs;
        }
        sample.mode = RemoteSnapshotSampleMode::Interpolated;
        return sample;
    }

    const auto extrapolationAge =
        renderAtMs - newest.timelineAtMs;
    const auto maximumExtrapolation =
        MaximumExtrapolationFor(newest.state);
    const auto boundedAge = std::min(
        extrapolationAge,
        maximumExtrapolation);
    const auto velocity =
        ClampRemoteVelocity(newest.state.velocity);
    const auto seconds =
        static_cast<float>(boundedAge) / 1'000.0F;
    sample.state.position = {
        newest.state.position.x + (velocity.x * seconds),
        newest.state.position.y + (velocity.y * seconds),
        newest.state.position.z + (velocity.z * seconds)};
    if (extrapolationAge >
        maximumExtrapolation) {
        sample.state.velocity = {};
        sample.mode = RemoteSnapshotSampleMode::Frozen;
    } else {
        sample.state.velocity = velocity;
        sample.mode = RemoteSnapshotSampleMode::Extrapolated;
    }
    if (newest.senderTickMs != 0U) {
        sample.senderTickMs = newest.senderTickMs + boundedAge;
    }
    return sample;
}

void RemoteSnapshotBuffer::Reset() noexcept {
    size_ = 0U;
    senderAnchorTickMs_ = 0U;
    receiverAnchorMs_ = 0U;
    previousSenderTickMs_ = 0U;
    previousArrivalMs_ = 0U;
    interpolationDelayMs_ =
        kRemoteSnapshotBaseInterpolationDelayMs;
    arrivalJitterMs_ = 0.0F;
    senderTimelineResets_ = 0U;
    hasSenderTimeline_ = false;
}

}  // namespace coopstory::bridge
