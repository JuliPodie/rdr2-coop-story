#include "coopstory/bridge/Telemetry.hpp"

#include <limits>

namespace coopstory::bridge {

bool TelemetryScheduler::ShouldEmit(const std::uint64_t nowMs) noexcept {
    // Handle a monotonic-clock reset or first tick by re-anchoring the cadence instead of emitting a burst of catch-up snapshots.
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

    // Skip missed intervals rather than trying to send historical PlayerState frames: multiplayer replication needs the current state only.
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
    // Construct one fully framed latest-state message for the pipe/sidecar; transport routing (UDP versus local pipe) happens at higher layers.
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
