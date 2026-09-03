#pragma once

#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>

namespace coopstory::bridge {
    //Sorry for the megaton of comments! u tried to fool proof it by explaining everything that i personally understood...
    //and now it looks like insane scribbles on the wall.
struct Vec3 final {
    float x{};
    float y{};
    float z{};
};
//returns the square root of the sum of the squares of the differences between the x, y, and z coordinates of two Vec3 objects (basically the straight line distance)
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

    // A NetEntityId is one shared multiplayer ID for an entity, such as an NPC.
    // It has two 32-bit halves:
        // - epoch: says which session/world version the entity belongs to.
        // - counter: says which entity it is in that session/world.
    // Both parts must be non-zero. Zero means "no valid entity ID".
    // This class packs the two halves into one 64-bit number and can pull them apart again.
    //If the epoch is 0, then the id is invalid. If the counter is 0, then the id is invalid.



    //First 2 lines create a default constructor and a constructor that takes a uint64_t(an unsigned 64-bit value) value.
    //The default constructor initializes the value to 0, which is invalid.
    //The constructor that takes a uint64_t value initializes the value to the given value, which may be valid or invalid.


    //(basically, these two lines create the NetEntityId class, which is a wrapper around a uint64_t value that represents a network entity id.
    //The class provides methods to compose an id from an epoch and counter, to extract the epoch and counter from an id, and to check if an id is valid.))
    constexpr NetEntityId() noexcept = default;
    explicit constexpr NetEntityId(const std::uint64_t value) noexcept : value_(value) {}


    //this function takes in an epoch and a counter, and returns a NetEntityId that is composed of the two values.
    //The epoch is shifted left by 32 bits, and the counter is bitwise OR'd with the shifted epoch to create a 64-bit value that represents the NetEntityId.
    [[nodiscard]] static constexpr NetEntityId Compose(
        const std::uint32_t epoch,
        const std::uint32_t counter) noexcept {
        return NetEntityId{(static_cast<std::uint64_t>(epoch) << 32U) | counter};
    }


    //returns the underlying 64-bit value of the NetEntityId.
    [[nodiscard]] constexpr std::uint64_t Value() const noexcept { return value_; }

    //returns the epoch of the NetEntityId, which is the upper 32 bits of the 64-bit value.
    [[nodiscard]] constexpr std::uint32_t Epoch() const noexcept {
        return static_cast<std::uint32_t>(value_ >> 32U);
    }

    // Returns the counter: the lower 32 bits of the full ID.
    // 0xFFFFFFFFULL is a bit mask that keeps only those lower 32 bits.
    [[nodiscard]] constexpr std::uint32_t Counter() const noexcept {
        return static_cast<std::uint32_t>(value_ & 0xFFFFFFFFULL);
    }

    //Ensures that the NetEntityId is valid by checking that both the epoch and counter are non-zero. If either is zero, the id is considered invalid.
    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return Epoch() != 0U && Counter() != 0U;
    }

    //uses the "friend" keyword to allow the operator== and operator<=> functions to access the private members of the NetEntityId class.
    friend constexpr bool operator==(NetEntityId, NetEntityId) noexcept = default;

    // The operator<=> function is a three-way comparison operator that allows for comparisons between two NetEntityId objects.
    //It returns a value indicating whether the first object is less than, equal to, or greater than the second object.
    //The "default" specifier indicates that the compiler should generate the default implementation of the operator based on the member variables of the class.
    friend constexpr auto operator<=>(NetEntityId, NetEntityId) noexcept = default;


//in this private it generates a hash function for the NetEntityId class, which allows it to be used as a key in hash-based containers like std::unordered_map or std::unordered_set.
private:
    std::uint64_t value_{};
};

// This struct defines a hash function for the NetEntityId class, which allows it to be used as a key in hash-based containers like std::unordered_map or std::unordered_set.
struct NetEntityIdHash final {
    [[nodiscard]] std::size_t operator()(const NetEntityId id) const noexcept {
        return static_cast<std::size_t>(id.Value() ^ (id.Value() >> 32U));
    }
};

}  // namespace coopstory::bridge
