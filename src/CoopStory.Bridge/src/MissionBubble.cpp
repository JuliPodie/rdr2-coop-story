#include "coopstory/bridge/MissionBubble.hpp"

#include <cmath>
#include <stdexcept>

namespace coopstory::bridge {

MissionBubbleController::MissionBubbleController(
    const MissionBubbleOverLimitAction action,
    const float warningMeters,
    const float limitMeters)
    : action_(action),
      warningMeters_(warningMeters),
      limitMeters_(limitMeters) {
    if (!std::isfinite(warningMeters_) || !std::isfinite(limitMeters_) ||
        warningMeters_ < 0.0F || limitMeters_ <= warningMeters_) {
        throw std::invalid_argument(
            "mission bubble requires 0 <= warning < limit");
    }
}

MissionBubbleDecision MissionBubbleController::Evaluate(
    const bool missionActive,
    const float playerDistanceMeters) {
    MissionBubbleZone zone = MissionBubbleZone::Disabled;
    if (missionActive) {
        if (!std::isfinite(playerDistanceMeters) ||
            playerDistanceMeters > limitMeters_) {
            zone = MissionBubbleZone::Exceeded;
        } else if (playerDistanceMeters >= warningMeters_) {
            zone = MissionBubbleZone::Warning;
        } else {
            zone = MissionBubbleZone::Inside;
        }
    }

    const bool changed = zone != lastZone_;
    const bool execute =
        zone == MissionBubbleZone::Exceeded &&
        lastZone_ != MissionBubbleZone::Exceeded;
    lastZone_ = zone;
    return {
        zone,
        changed,
        zone == MissionBubbleZone::Warning ||
            zone == MissionBubbleZone::Exceeded,
        execute,
        action_};
}

void MissionBubbleController::Reset() noexcept {
    lastZone_ = MissionBubbleZone::Disabled;
}

}  // namespace coopstory::bridge
