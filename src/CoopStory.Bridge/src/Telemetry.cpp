#include "coopstory/bridge/Telemetry.hpp"

#include <limits>

namespace coopstory::bridge {

bool TelemetryScheduler::ShouldEmit(const std::uint64_t nowMs) noexcept {
    if (!initialized_ || nowMs < lastNowMs_) {
        initialized_ = true;
        lastNowMs_ = nowMs;
        nextDueMs_ = nowMs + kTelemetryIntervalMs;
        return true;
    }

    lastNowMs_ = nowMs;
    if (nowMs < nextDueMs_) {
        return false;
    }

    const auto overdue = nowMs - nextDueMs_;
    const auto intervals = (overdue / kTelemetryIntervalMs) + 1U;
    if (intervals >
        (std::numeric_limits<std::uint64_t>::max() - nextDueMs_) /
            kTelemetryIntervalMs) {
        nextDueMs_ = nowMs + kTelemetryIntervalMs;
    } else {
        nextDueMs_ += intervals * kTelemetryIntervalMs;
    }
    return true;
}

void TelemetryScheduler::Reset() noexcept {
    initialized_ = false;
    lastNowMs_ = 0U;
    nextDueMs_ = 0U;
}

Frame MakePlayerStateFrame(
    const PlayerStatePayload& state,
    const std::uint32_t sequence,
    const std::uint64_t tickMs) {
    Frame frame;
    frame.header.type = MessageType::PlayerState;
    frame.header.sequence = sequence;
    frame.header.tick = tickMs;
    frame.payload = EncodePlayerState(state);
    frame.header.payloadLength =
        static_cast<std::uint32_t>(frame.payload.size());
    return frame;
}

}  // namespace coopstory::bridge
