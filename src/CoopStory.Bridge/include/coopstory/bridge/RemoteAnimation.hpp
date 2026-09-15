#pragma once

#include "coopstory/bridge/AnimationReplicationCodec.hpp"
#include "coopstory/bridge/RemoteMotion.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace coopstory::bridge {

// Animation and transforms travel separately. Keep discrete animation changes
// on the same delayed sender timeline as the rendered player transform.
class RemoteAnimationBuffer final {
public:
    void Push(
        const PlayerAnimationStatePayload& state,
        std::uint64_t receivedAtMs,
        std::uint64_t senderTickMs) noexcept;
    [[nodiscard]] std::optional<PlayerAnimationStatePayload> Sample(
        const RemoteSnapshotSample& rendered,
        std::uint64_t nowMs) const noexcept;
    void Reset() noexcept;

private:
    struct TimedAnimation final {
        PlayerAnimationStatePayload state{};
        std::uint64_t receivedAtMs{};
        std::uint64_t senderTickMs{};
    };
    static constexpr std::size_t kCapacity = 16U;
    std::array<TimedAnimation, kCapacity> samples_{};
    std::size_t size_{};
};

}  // namespace coopstory::bridge
