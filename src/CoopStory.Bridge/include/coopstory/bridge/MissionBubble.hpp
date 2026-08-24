#pragma once

#include <cstdint>

namespace coopstory::bridge {

inline constexpr float kMissionBubbleWarningMeters = 200.0F;
inline constexpr float kMissionBubbleLimitMeters = 250.0F;

enum class MissionBubbleZone : std::uint8_t {
    Disabled,
    Inside,
    Warning,
    Exceeded,
};

enum class MissionBubbleOverLimitAction : std::uint8_t {
    TeleportGuest,
    SpectateGuest,
};

struct MissionBubbleDecision final {
    MissionBubbleZone zone{MissionBubbleZone::Disabled};
    bool zoneChanged{};
    bool showWarning{};
    bool executeOverLimitAction{};
    MissionBubbleOverLimitAction action{MissionBubbleOverLimitAction::TeleportGuest};
};

class MissionBubbleController final {
public:
    explicit MissionBubbleController(
        MissionBubbleOverLimitAction action =
            MissionBubbleOverLimitAction::TeleportGuest,
        float warningMeters = kMissionBubbleWarningMeters,
        float limitMeters = kMissionBubbleLimitMeters);

    [[nodiscard]] MissionBubbleDecision Evaluate(
        bool missionActive,
        float playerDistanceMeters);
    void Reset() noexcept;

private:
    MissionBubbleOverLimitAction action_;
    float warningMeters_;
    float limitMeters_;
    MissionBubbleZone lastZone_{MissionBubbleZone::Disabled};
};

}  // namespace coopstory::bridge
