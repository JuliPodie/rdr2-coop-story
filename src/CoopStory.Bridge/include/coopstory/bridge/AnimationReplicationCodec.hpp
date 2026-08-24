#pragma once

#include "coopstory/bridge/Domain.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace coopstory::bridge {

inline constexpr std::uint8_t kPlayerAnimationStateSchemaVersion = 1U;
inline constexpr std::uint8_t kMotionReplicationConfigSchemaVersion = 1U;
inline constexpr std::size_t kPlayerAnimationStatePayloadSize = 72U;
inline constexpr std::size_t kMotionReplicationConfigPayloadSize = 8U;

enum class PlayerAnimationCapability : std::uint32_t {
    GraphIdentifier = 1U << 0U,
    StateIdentifier = 1U << 1U,
    ClipIdentifiers = 1U << 2U,
    NormalizedPhase = 1U << 3U,
    PlaybackRate = 1U << 4U,
    BlendWeights = 1U << 5U,
    TransitionProgress = 1U << 6U,
    RuntimeFlags = 1U << 7U,
};

enum class PlayerAnimationStateFlag : std::uint32_t {
    GraphHashValid = 1U << 0U,
    StateHashValid = 1U << 1U,
    PrimaryClipHashValid = 1U << 2U,
    SecondaryClipHashValid = 1U << 3U,
    PrimaryPhaseValid = 1U << 4U,
    SecondaryPhaseValid = 1U << 5U,
    PrimaryPlaybackRateValid = 1U << 6U,
    SecondaryPlaybackRateValid = 1U << 7U,
    PrimaryBlendWeightValid = 1U << 8U,
    SecondaryBlendWeightValid = 1U << 9U,
    TransitionProgressValid = 1U << 10U,
    Transitioning = 1U << 16U,
    RootMotionActive = 1U << 17U,
    Looping = 1U << 18U,
};

enum class PlayerAnimationSampleSource : std::uint8_t {
    None = 0,
    LocomotionNative = 1,
    MoveNetworkNative = 2,
    VersionedMemoryReader = 3,
};

// Optional exact graph sample. All unavailable fields must remain zero and
// must not have a validity bit. This lets version-specific graph readers be
// added without inventing hashes when a field cannot yet be resolved.
struct PlayerAnimationStatePayload final {
    NetEntityId entityId{};
    PlayerSlot slot{PlayerSlot::Host};
    std::uint8_t schemaVersion{kPlayerAnimationStateSchemaVersion};
    PlayerAnimationSampleSource source{PlayerAnimationSampleSource::None};
    std::uint16_t locomotionEpoch{};
    std::uint32_t sampleSequence{};
    std::uint32_t capabilities{};
    std::uint32_t flags{};
    std::uint32_t graphHash{};
    std::uint32_t stateHash{};
    std::uint32_t primaryClipHash{};
    std::uint32_t secondaryClipHash{};
    float primaryNormalizedPhase{};
    float secondaryNormalizedPhase{};
    float primaryPlaybackRate{};
    float secondaryPlaybackRate{};
    float primaryBlendWeight{};
    float secondaryBlendWeight{};
    float transitionProgress{};
};

[[nodiscard]] std::vector<std::uint8_t> EncodePlayerAnimationState(
    const PlayerAnimationStatePayload& payload);
[[nodiscard]] std::optional<PlayerAnimationStatePayload>
DecodePlayerAnimationState(std::span<const std::uint8_t> bytes);

enum class MotionReplicationWireMode : std::uint8_t {
    TaskNavmesh = 0,
    AnimGraphReplica = 1,
};

enum class MotionReplicationConfigFlag : std::uint16_t {
    AllowTaskNavmeshFallback = 1U << 0U,
    EnableAnimSceneStoryVmProbe = 1U << 1U,
};

struct MotionReplicationConfigPayload final {
    std::uint8_t schemaVersion{kMotionReplicationConfigSchemaVersion};
    MotionReplicationWireMode mode{MotionReplicationWireMode::TaskNavmesh};
    std::uint16_t flags{};
    std::uint32_t revision{};
};

[[nodiscard]] std::vector<std::uint8_t> EncodeMotionReplicationConfig(
    const MotionReplicationConfigPayload& payload);
[[nodiscard]] std::optional<MotionReplicationConfigPayload>
DecodeMotionReplicationConfig(std::span<const std::uint8_t> bytes);

}  // namespace coopstory::bridge
