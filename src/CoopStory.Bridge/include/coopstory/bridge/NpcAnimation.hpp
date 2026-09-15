#pragma once

#include "coopstory/bridge/FrameCodec.hpp"
#include <deque>

namespace coopstory::bridge {

enum class NpcAnimationFlag : std::uint16_t {
    Reloading = 1U, Ragdoll = 2U, GettingUp = 4U, Melee = 8U,
    Mounted = 16U, Dead = 32U, Jumping = 64U, Climbing = 128U,
};
enum class NpcAnimationEvent : std::uint16_t { Shot = 1U, HitReaction = 2U };
inline constexpr std::size_t kNpcAnimationPayloadSize = 56U;
inline constexpr std::size_t kNpcAnimationQueueCapacity = 16U;

struct NpcAnimationPayload final {
    NetEntityId entityId{};
    std::uint32_t revision{};
    std::uint16_t flags{};
    std::uint16_t events{};
    std::uint16_t leaseMs{1'000U};
    std::uint32_t weaponHash{};
    std::uint32_t modelHash{};
    Vec3 target{};
    Vec3 velocity{};
    float healthFraction{1.0F};
};
[[nodiscard]] bool IsValidNpcAnimation(const NpcAnimationPayload&) noexcept;
[[nodiscard]] std::vector<std::uint8_t> EncodeNpcAnimation(const NpcAnimationPayload&);
[[nodiscard]] std::optional<NpcAnimationPayload> DecodeNpcAnimation(std::span<const std::uint8_t>);

struct NpcAnimationSample final {
    std::uint16_t flags{};
    std::uint32_t modelHash{};
    std::uint32_t weaponHash{};
    Vec3 target{};
    Vec3 velocity{};
    float healthFraction{1.0F};
    int clipAmmo{-1};
    bool firing{};
};
struct TimedNpcAnimation final {
    NpcAnimationPayload payload{};
    std::uint64_t atMs{};
};

// One instance per stable world ID (which already includes its generation).
// Capture runs every bridge tick, independently of 10 Hz world snapshots.
class NpcAnimationCapture final {
public:
    void Observe(NetEntityId, const NpcAnimationSample&, std::uint64_t nowMs);
    const TimedNpcAnimation* Pending(std::uint64_t nowMs);
    void Acknowledge();
    void Rebaseline() noexcept;
    std::uint64_t dropped{};
private:
    NpcAnimationSample previous_{};
    bool initialized_{};
    std::uint32_t revision_{};
    std::uint64_t previousSentMs_{};
    std::deque<TimedNpcAnimation> pending_{};
};

class NpcAnimationInbox final {
public:
    bool Push(const NpcAnimationPayload&, std::uint64_t nowMs);
    std::optional<NpcAnimationPayload> Pop(std::uint64_t nowMs);
    void ClearPending() noexcept { pending_.clear(); }
    std::uint64_t rejected{};
    std::uint64_t expired{};
private:
    SequenceWindow revisions_{};
    std::deque<TimedNpcAnimation> pending_{};
};

enum class NpcAnimationOwner { None, Reload, Physical, Traversal, Melee, Mounted, Dead };
[[nodiscard]] NpcAnimationOwner SelectNpcAnimationOwner(std::uint16_t flags) noexcept;
}  // namespace coopstory::bridge
