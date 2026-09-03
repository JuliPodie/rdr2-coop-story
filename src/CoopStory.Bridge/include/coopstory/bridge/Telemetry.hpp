#pragma once

#include "coopstory/bridge/FrameCodec.hpp"

#include <cstdint>

namespace coopstory::bridge {

// Limits how often bridge diagnostics/player telemetry are emitted so logging does not run every RDR2 frame and hide the useful multiplayer information.
inline constexpr std::uint32_t kTelemetryFrequencyHz = 20U;
inline constexpr std::uint64_t kTelemetryIntervalMs = 1'000U / kTelemetryFrequencyHz;

class TelemetryScheduler final {
public:
    [[nodiscard]] bool ShouldEmit(std::uint64_t nowMs) noexcept;
    void Reset() noexcept;

private:
    bool initialized_{};
    std::uint64_t lastNowMs_{};
    std::uint64_t nextDueMs_{};
};

[[nodiscard]] Frame MakePlayerStateFrame(
    const PlayerStatePayload& state,
    std::uint32_t sequence,
    std::uint64_t tickMs);

}  // namespace coopstory::bridge
