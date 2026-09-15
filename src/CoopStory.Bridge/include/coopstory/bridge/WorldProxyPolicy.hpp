#pragma once

#include "coopstory/bridge/FrameCodec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace coopstory::bridge {

inline constexpr std::uint64_t kWorldProxyPredictionMs = 250U;
inline constexpr std::uint64_t kWorldProxyMountRetryMs = 500U;

[[nodiscard]] inline float WorldProxyDistance(const Vec3& a, const Vec3& b) noexcept {
    const auto x = a.x - b.x;
    const auto y = a.y - b.y;
    const auto z = a.z - b.z;
    return std::sqrt(x * x + y * y + z * z);
}

struct WorldProxyMotionPlan final {
    Vec3 position{};
    Vec3 destination{};
    float speed{};
    bool stale{};
    bool moving{};
};

[[nodiscard]] inline WorldProxyMotionPlan PlanWorldProxyMotion(
    const WorldEntityStatePayload& state,
    const std::uint64_t receivedAtMs,
    const std::uint64_t nowMs) noexcept {
    const auto age = nowMs >= receivedAtMs ? nowMs - receivedAtMs : 0U;
    const auto seconds = static_cast<float>(std::min(age, kWorldProxyPredictionMs)) / 1'000.0F;
    WorldProxyMotionPlan plan;
    plan.stale = nowMs < receivedAtMs || age > kWorldProxyPredictionMs;
    plan.position = {
        state.position.x + state.velocity.x * seconds,
        state.position.y + state.velocity.y * seconds,
        state.position.z + state.velocity.z * seconds};
    plan.speed = plan.stale ? 0.0F : std::hypot(state.velocity.x, state.velocity.y);
    plan.moving = plan.speed > 0.20F;
    // Combat taskTarget is the enemy, not the path the NPC is following.
    // Cinematic and combat roots must follow the sampled trajectory instead.
    plan.destination = state.taskTarget;
    if (state.taskKind == WorldTaskKind::Combat ||
        state.taskKind == WorldTaskKind::Cinematic) {
        plan.destination = {
            plan.position.x + (plan.stale ? 0.0F : state.velocity.x * 0.35F),
            plan.position.y + (plan.stale ? 0.0F : state.velocity.y * 0.35F),
            plan.position.z + (plan.stale ? 0.0F : state.velocity.z * 0.35F)};
    }
    if (plan.stale) {
        plan.destination = plan.position;
    }
    return plan;
}

// Packet gaps allow normal travel. Only an implausible jump in the host's
// position authorizes a warp; ordinary local tracking error stays smooth.
[[nodiscard]] inline bool WorldProxyTargetDiscontinuous(
    const WorldEntityStatePayload& previous,
    const WorldEntityStatePayload& next,
    const std::uint64_t previousAtMs,
    const std::uint64_t nowMs) noexcept {
    if (nowMs < previousAtMs) {
        return false;
    }
    const auto seconds = static_cast<float>(nowMs - previousAtMs) / 1'000.0F;
    const auto speed = std::max(
        WorldProxyDistance(previous.velocity, {}),
        WorldProxyDistance(next.velocity, {}));
    return WorldProxyDistance(previous.position, next.position) >
           std::max(12.0F, speed * seconds + 6.0F);
}

[[nodiscard]] inline Vec3 CorrectWorldProxyPosition(
    const Vec3& current,
    const Vec3& target,
    const float elapsedSeconds,
    const bool hardCorrection,
    const bool cinematic) noexcept {
    if (hardCorrection) {
        return target;
    }
    const auto distance = WorldProxyDistance(current, target);
    if (!std::isfinite(distance) || distance <= (cinematic ? 0.03F : 0.25F)) {
        return current;
    }
    const auto dt = std::clamp(elapsedSeconds, 0.0F, 0.1F);
    const auto alpha = std::min(
        1.0F - std::exp(-(cinematic ? 8.0F : 4.0F) * dt),
        4.0F * dt / distance);
    return {
        current.x + (target.x - current.x) * alpha,
        current.y + (target.y - current.y) * alpha,
        current.z + (target.z - current.z) * alpha};
}

enum class WorldProxyMountAction { None, Mount, Dismount };

struct WorldProxyMountDecision final {
    WorldProxyMountAction action{WorldProxyMountAction::None};
    bool blocksIndependentTasks{};
};

[[nodiscard]] inline WorldProxyMountDecision PlanWorldProxyMount(
    const bool desiredMounted,
    const bool parentReady,
    const bool actuallyMounted,
    const bool onDesiredParent,
    const std::uint64_t lastAttemptMs,
    const std::uint64_t nowMs) noexcept {
    const bool retry = lastAttemptMs == 0U || nowMs < lastAttemptMs ||
                       nowMs - lastAttemptMs >= kWorldProxyMountRetryMs;
    if (desiredMounted) {
        return {
            parentReady && !onDesiredParent && retry ? WorldProxyMountAction::Mount
                                                     : WorldProxyMountAction::None,
            true};
    }
    return {
        actuallyMounted && retry ? WorldProxyMountAction::Dismount
                                 : WorldProxyMountAction::None,
        actuallyMounted};
}

struct WorldProxyTaskSignature final {
    WorldTaskKind kind{WorldTaskKind::Idle};
    Vec3 destination{};
    Vec3 aimPosition{};
    float speed{};
    bool moving{};
    bool aiming{};
    bool stale{};
    LocalEntityHandle aimEntity{};
};

[[nodiscard]] inline bool ShouldRefreshWorldProxyTask(
    const WorldProxyTaskSignature& previous,
    const WorldProxyTaskSignature& desired,
    const std::uint64_t previousMs,
    const std::uint64_t nowMs,
    const bool scenarioActive) noexcept {
    if (previousMs == 0U || nowMs < previousMs ||
        previous.kind != desired.kind || previous.moving != desired.moving ||
        previous.aiming != desired.aiming || previous.stale != desired.stale ||
        previous.aimEntity != desired.aimEntity) {
        return true;
    }
    const auto age = nowMs - previousMs;
    // A running scenario owns its graph. Do not restart it for tiny drift.
    if (desired.kind == WorldTaskKind::Scenario && !desired.stale) {
        return !scenarioActive && age >= 2'000U;
    }
    const auto watchdog = desired.aiming || desired.kind == WorldTaskKind::Cinematic
                              ? 250U : 750U;
    return age >= watchdog ||
           (age >= 150U &&
            (std::abs(previous.speed - desired.speed) >= 0.35F ||
             WorldProxyDistance(previous.destination, desired.destination) >= 0.75F ||
             (desired.aiming &&
              WorldProxyDistance(previous.aimPosition, desired.aimPosition) >= 0.75F)));
}

}  // namespace coopstory::bridge
