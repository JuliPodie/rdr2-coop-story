#include "coopstory/bridge/RemoteAnimation.hpp"

#include <algorithm>

namespace coopstory::bridge {

void RemoteAnimationBuffer::Push(
    const PlayerAnimationStatePayload& state,
    const std::uint64_t receivedAtMs,
    const std::uint64_t senderTickMs) noexcept {
    if (size_ != 0U) {
        const auto& latest = samples_[size_ - 1U];
        if (latest.state.entityId != state.entityId ||
            latest.state.slot != state.slot ||
            latest.state.locomotionEpoch != state.locomotionEpoch ||
            receivedAtMs < latest.receivedAtMs ||
            (senderTickMs != 0U && latest.senderTickMs != 0U &&
             senderTickMs < latest.senderTickMs)) {
            Reset();
        }
    }
    if (size_ == kCapacity) {
        std::move(samples_.begin() + 1, samples_.end(), samples_.begin());
        --size_;
    }
    samples_[size_++] = {state, receivedAtMs, senderTickMs};
}

std::optional<PlayerAnimationStatePayload> RemoteAnimationBuffer::Sample(
    const RemoteSnapshotSample& rendered,
    const std::uint64_t nowMs) const noexcept {
    // Prediction has ended. Continuing a cached moving gait would animate
    // footsteps against a frozen transform until the independent cache TTL.
    if (rendered.mode == RemoteSnapshotSampleMode::Frozen) {
        return std::nullopt;
    }
    for (auto index = size_; index != 0U; --index) {
        const auto& candidate = samples_[index - 1U];
        if (candidate.state.entityId != rendered.state.entityId ||
            candidate.state.slot != rendered.state.slot ||
            candidate.state.locomotionEpoch !=
                rendered.state.locomotionEpoch ||
            (candidate.senderTickMs != 0U && rendered.senderTickMs != 0U &&
             candidate.senderTickMs > rendered.senderTickMs) ||
            !IsRemoteAnimationStateFresh(
                candidate.receivedAtMs,
                candidate.senderTickMs,
                rendered.senderTickMs,
                nowMs)) {
            continue;
        }
        return candidate.state;
    }
    return std::nullopt;
}

void RemoteAnimationBuffer::Reset() noexcept {
    size_ = 0U;
}

}  // namespace coopstory::bridge
