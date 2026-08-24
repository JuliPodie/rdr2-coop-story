#pragma once

#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>

namespace coopstory::bridge {

struct Vec3 final {
    float x{};
    float y{};
    float z{};
};

[[nodiscard]] inline float Distance(const Vec3& lhs, const Vec3& rhs) noexcept {
    const auto dx = lhs.x - rhs.x;
    const auto dy = lhs.y - rhs.y;
    const auto dz = lhs.z - rhs.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

enum class PlayerSlot : std::uint8_t {
    Host = 0,
    Guest = 1,
};

enum class PlayerLifecycle : std::uint8_t {
    Alive = 0,
    Downed = 1,
    Reviving = 2,
    Spectator = 3,
};

// Process-local only. This value must never be placed in a wire payload.
using LocalEntityHandle = std::int32_t;

class NetEntityId final {
public:
    constexpr NetEntityId() noexcept = default;
    explicit constexpr NetEntityId(const std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] static constexpr NetEntityId Compose(
        const std::uint32_t epoch,
        const std::uint32_t counter) noexcept {
        return NetEntityId{(static_cast<std::uint64_t>(epoch) << 32U) | counter};
    }

    [[nodiscard]] constexpr std::uint64_t Value() const noexcept { return value_; }
    [[nodiscard]] constexpr std::uint32_t Epoch() const noexcept {
        return static_cast<std::uint32_t>(value_ >> 32U);
    }
    [[nodiscard]] constexpr std::uint32_t Counter() const noexcept {
        return static_cast<std::uint32_t>(value_ & 0xFFFFFFFFULL);
    }
    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return Epoch() != 0U && Counter() != 0U;
    }

    friend constexpr bool operator==(NetEntityId, NetEntityId) noexcept = default;
    friend constexpr auto operator<=>(NetEntityId, NetEntityId) noexcept = default;

private:
    std::uint64_t value_{};
};

struct NetEntityIdHash final {
    [[nodiscard]] std::size_t operator()(const NetEntityId id) const noexcept {
        return static_cast<std::size_t>(id.Value() ^ (id.Value() >> 32U));
    }
};

}  // namespace coopstory::bridge
