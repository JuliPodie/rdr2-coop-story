#include "coopstory/bridge/AnimationReplicationCodec.hpp"

#include <bit>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace coopstory::bridge {
namespace {

inline constexpr float kMaximumAbsolutePlaybackRate = 8.0F;

inline constexpr std::uint32_t kKnownCapabilities =
    static_cast<std::uint32_t>(PlayerAnimationCapability::GraphIdentifier) |
    static_cast<std::uint32_t>(PlayerAnimationCapability::StateIdentifier) |
    static_cast<std::uint32_t>(PlayerAnimationCapability::ClipIdentifiers) |
    static_cast<std::uint32_t>(PlayerAnimationCapability::NormalizedPhase) |
    static_cast<std::uint32_t>(PlayerAnimationCapability::PlaybackRate) |
    static_cast<std::uint32_t>(PlayerAnimationCapability::BlendWeights) |
    static_cast<std::uint32_t>(PlayerAnimationCapability::TransitionProgress) |
    static_cast<std::uint32_t>(PlayerAnimationCapability::RuntimeFlags);

inline constexpr std::uint32_t kKnownStateFlags =
    static_cast<std::uint32_t>(PlayerAnimationStateFlag::GraphHashValid) |
    static_cast<std::uint32_t>(PlayerAnimationStateFlag::StateHashValid) |
    static_cast<std::uint32_t>(PlayerAnimationStateFlag::PrimaryClipHashValid) |
    static_cast<std::uint32_t>(PlayerAnimationStateFlag::SecondaryClipHashValid) |
    static_cast<std::uint32_t>(PlayerAnimationStateFlag::PrimaryPhaseValid) |
    static_cast<std::uint32_t>(PlayerAnimationStateFlag::SecondaryPhaseValid) |
    static_cast<std::uint32_t>(
        PlayerAnimationStateFlag::PrimaryPlaybackRateValid) |
    static_cast<std::uint32_t>(
        PlayerAnimationStateFlag::SecondaryPlaybackRateValid) |
    static_cast<std::uint32_t>(
        PlayerAnimationStateFlag::PrimaryBlendWeightValid) |
    static_cast<std::uint32_t>(
        PlayerAnimationStateFlag::SecondaryBlendWeightValid) |
    static_cast<std::uint32_t>(
        PlayerAnimationStateFlag::TransitionProgressValid) |
    static_cast<std::uint32_t>(PlayerAnimationStateFlag::Transitioning) |
    static_cast<std::uint32_t>(PlayerAnimationStateFlag::RootMotionActive) |
    static_cast<std::uint32_t>(PlayerAnimationStateFlag::Looping);

template <typename T, bool = std::is_enum_v<T>>
struct RawIntegral final {
    using Type = T;
};

template <typename T>
struct RawIntegral<T, true> final {
    using Type = std::underlying_type_t<T>;
};

template <typename T>
    requires(std::is_integral_v<T> || std::is_enum_v<T>)
void AppendLittleEndian(std::vector<std::uint8_t>& bytes, const T value) {
    using Raw = typename RawIntegral<T>::Type;
    using Unsigned = std::make_unsigned_t<Raw>;
    auto remaining = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
        bytes.push_back(static_cast<std::uint8_t>(remaining & 0xFFU));
        remaining >>= 8U;
    }
}

void AppendFloat(std::vector<std::uint8_t>& bytes, const float value) {
    AppendLittleEndian(bytes, std::bit_cast<std::uint32_t>(value));
}

template <typename T>
    requires(std::is_integral_v<T>)
[[nodiscard]] T ReadLittleEndian(
    const std::span<const std::uint8_t> bytes,
    std::size_t& offset) {
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned value{};
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<Unsigned>(bytes[offset++]) << (index * 8U);
    }
    return static_cast<T>(value);
}

[[nodiscard]] float ReadFloat(
    const std::span<const std::uint8_t> bytes,
    std::size_t& offset) {
    return std::bit_cast<float>(
        ReadLittleEndian<std::uint32_t>(bytes, offset));
}

[[nodiscard]] bool HasFlag(
    const PlayerAnimationStatePayload& payload,
    const PlayerAnimationStateFlag flag) noexcept {
    return (payload.flags & static_cast<std::uint32_t>(flag)) != 0U;
}

[[nodiscard]] bool HasAnyFlag(
    const PlayerAnimationStatePayload& payload,
    const std::uint32_t flags) noexcept {
    return (payload.flags & flags) != 0U;
}

[[nodiscard]] bool HasCapability(
    const PlayerAnimationStatePayload& payload,
    const PlayerAnimationCapability capability) noexcept {
    return (payload.capabilities & static_cast<std::uint32_t>(capability)) != 0U;
}

[[nodiscard]] bool IsKnownSlot(const PlayerSlot slot) noexcept {
    return static_cast<std::uint8_t>(slot) <=
           static_cast<std::uint8_t>(PlayerSlot::Guest);
}

[[nodiscard]] bool IsKnownSource(
    const PlayerAnimationSampleSource source) noexcept {
    return static_cast<std::uint8_t>(source) <=
           static_cast<std::uint8_t>(
               PlayerAnimationSampleSource::VersionedMemoryReader);
}

[[nodiscard]] bool IsCanonicalHash(
    const std::uint32_t value,
    const bool valid) noexcept {
    return valid ? value != 0U : value == 0U;
}

[[nodiscard]] bool IsNormalizedOrZero(
    const float value,
    const bool valid) noexcept {
    return std::isfinite(value) &&
           (valid ? value >= 0.0F && value <= 1.0F : value == 0.0F);
}

[[nodiscard]] bool IsPlaybackRateOrZero(
    const float value,
    const bool valid) noexcept {
    return std::isfinite(value) &&
           (valid ? std::abs(value) <= kMaximumAbsolutePlaybackRate
                  : value == 0.0F);
}

[[nodiscard]] bool FlagsRequireCapability(
    const PlayerAnimationStatePayload& payload,
    const std::uint32_t flags,
    const PlayerAnimationCapability capability) noexcept {
    return !HasAnyFlag(payload, flags) || HasCapability(payload, capability);
}

[[nodiscard]] bool IsValid(
    const PlayerAnimationStatePayload& payload) noexcept {
    const auto graphValid =
        HasFlag(payload, PlayerAnimationStateFlag::GraphHashValid);
    const auto stateValid =
        HasFlag(payload, PlayerAnimationStateFlag::StateHashValid);
    const auto primaryClipValid =
        HasFlag(payload, PlayerAnimationStateFlag::PrimaryClipHashValid);
    const auto secondaryClipValid =
        HasFlag(payload, PlayerAnimationStateFlag::SecondaryClipHashValid);
    const auto primaryPhaseValid =
        HasFlag(payload, PlayerAnimationStateFlag::PrimaryPhaseValid);
    const auto secondaryPhaseValid =
        HasFlag(payload, PlayerAnimationStateFlag::SecondaryPhaseValid);
    const auto primaryRateValid =
        HasFlag(payload, PlayerAnimationStateFlag::PrimaryPlaybackRateValid);
    const auto secondaryRateValid =
        HasFlag(payload, PlayerAnimationStateFlag::SecondaryPlaybackRateValid);
    const auto primaryWeightValid =
        HasFlag(payload, PlayerAnimationStateFlag::PrimaryBlendWeightValid);
    const auto secondaryWeightValid =
        HasFlag(payload, PlayerAnimationStateFlag::SecondaryBlendWeightValid);
    const auto transitionProgressValid =
        HasFlag(payload, PlayerAnimationStateFlag::TransitionProgressValid);

    constexpr auto kClipValidityFlags =
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::PrimaryClipHashValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::SecondaryClipHashValid);
    constexpr auto kPhaseValidityFlags =
        static_cast<std::uint32_t>(PlayerAnimationStateFlag::PrimaryPhaseValid) |
        static_cast<std::uint32_t>(PlayerAnimationStateFlag::SecondaryPhaseValid);
    constexpr auto kRateValidityFlags =
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::PrimaryPlaybackRateValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::SecondaryPlaybackRateValid);
    constexpr auto kWeightValidityFlags =
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::PrimaryBlendWeightValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::SecondaryBlendWeightValid);
    constexpr auto kTransitionFlags =
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::TransitionProgressValid) |
        static_cast<std::uint32_t>(PlayerAnimationStateFlag::Transitioning);
    constexpr auto kRuntimeFlags =
        static_cast<std::uint32_t>(PlayerAnimationStateFlag::RootMotionActive) |
        static_cast<std::uint32_t>(PlayerAnimationStateFlag::Looping);

    return payload.entityId.IsValid() &&
           IsKnownSlot(payload.slot) &&
           payload.schemaVersion == kPlayerAnimationStateSchemaVersion &&
           IsKnownSource(payload.source) &&
           payload.locomotionEpoch != 0U &&
           payload.sampleSequence != 0U &&
           (payload.capabilities & ~kKnownCapabilities) == 0U &&
           (payload.flags & ~kKnownStateFlags) == 0U &&
           FlagsRequireCapability(
               payload,
               static_cast<std::uint32_t>(
                   PlayerAnimationStateFlag::GraphHashValid),
               PlayerAnimationCapability::GraphIdentifier) &&
           FlagsRequireCapability(
               payload,
               static_cast<std::uint32_t>(
                   PlayerAnimationStateFlag::StateHashValid),
               PlayerAnimationCapability::StateIdentifier) &&
           FlagsRequireCapability(
               payload,
               kClipValidityFlags,
               PlayerAnimationCapability::ClipIdentifiers) &&
           FlagsRequireCapability(
               payload,
               kPhaseValidityFlags,
               PlayerAnimationCapability::NormalizedPhase) &&
           FlagsRequireCapability(
               payload,
               kRateValidityFlags,
               PlayerAnimationCapability::PlaybackRate) &&
           FlagsRequireCapability(
               payload,
               kWeightValidityFlags,
               PlayerAnimationCapability::BlendWeights) &&
           FlagsRequireCapability(
               payload,
               kTransitionFlags,
               PlayerAnimationCapability::TransitionProgress) &&
           FlagsRequireCapability(
               payload,
               kRuntimeFlags,
               PlayerAnimationCapability::RuntimeFlags) &&
           IsCanonicalHash(payload.graphHash, graphValid) &&
           IsCanonicalHash(payload.stateHash, stateValid) &&
           IsCanonicalHash(payload.primaryClipHash, primaryClipValid) &&
           IsCanonicalHash(payload.secondaryClipHash, secondaryClipValid) &&
           IsNormalizedOrZero(
               payload.primaryNormalizedPhase,
               primaryPhaseValid) &&
           IsNormalizedOrZero(
               payload.secondaryNormalizedPhase,
               secondaryPhaseValid) &&
           IsPlaybackRateOrZero(
               payload.primaryPlaybackRate,
               primaryRateValid) &&
           IsPlaybackRateOrZero(
               payload.secondaryPlaybackRate,
               secondaryRateValid) &&
           IsNormalizedOrZero(
               payload.primaryBlendWeight,
               primaryWeightValid) &&
           IsNormalizedOrZero(
               payload.secondaryBlendWeight,
               secondaryWeightValid) &&
           IsNormalizedOrZero(
               payload.transitionProgress,
               transitionProgressValid) &&
           (primaryClipValid ||
            (!primaryPhaseValid && !primaryRateValid && !primaryWeightValid)) &&
           (secondaryClipValid ||
            (!secondaryPhaseValid && !secondaryRateValid &&
             !secondaryWeightValid)) &&
           (!secondaryClipValid || primaryClipValid) &&
           (!HasFlag(payload, PlayerAnimationStateFlag::Transitioning) ||
            transitionProgressValid) &&
           ((payload.source == PlayerAnimationSampleSource::None) ==
            (payload.capabilities == 0U && payload.flags == 0U));
}

[[nodiscard]] bool IsValid(
    const MotionReplicationConfigPayload& payload) noexcept {
    constexpr auto kKnownFlags = static_cast<std::uint16_t>(
        MotionReplicationConfigFlag::AllowTaskNavmeshFallback) |
        static_cast<std::uint16_t>(
            MotionReplicationConfigFlag::EnableAnimSceneStoryVmProbe);
    const auto mode = static_cast<std::uint8_t>(payload.mode);
    return payload.schemaVersion == kMotionReplicationConfigSchemaVersion &&
           mode <= static_cast<std::uint8_t>(
               MotionReplicationWireMode::AnimGraphReplica) &&
           (payload.flags & ~kKnownFlags) == 0U &&
           payload.revision != 0U;
}

}  // namespace

std::vector<std::uint8_t> EncodePlayerAnimationState(
    const PlayerAnimationStatePayload& payload) {
    if (!IsValid(payload)) {
        throw std::invalid_argument(
            "PlayerAnimationState contains invalid fields");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kPlayerAnimationStatePayloadSize);
    AppendLittleEndian(bytes, payload.entityId.Value());
    AppendLittleEndian(bytes, payload.slot);
    AppendLittleEndian(bytes, payload.schemaVersion);
    AppendLittleEndian(bytes, payload.locomotionEpoch);
    AppendLittleEndian(bytes, payload.sampleSequence);
    AppendLittleEndian(bytes, payload.capabilities);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, payload.graphHash);
    AppendLittleEndian(bytes, payload.stateHash);
    AppendLittleEndian(bytes, payload.primaryClipHash);
    AppendLittleEndian(bytes, payload.secondaryClipHash);
    AppendFloat(bytes, payload.primaryNormalizedPhase);
    AppendFloat(bytes, payload.secondaryNormalizedPhase);
    AppendFloat(bytes, payload.primaryPlaybackRate);
    AppendFloat(bytes, payload.secondaryPlaybackRate);
    AppendFloat(bytes, payload.primaryBlendWeight);
    AppendFloat(bytes, payload.secondaryBlendWeight);
    AppendFloat(bytes, payload.transitionProgress);
    AppendLittleEndian(bytes, payload.source);
    AppendLittleEndian(bytes, std::uint8_t{0});
    AppendLittleEndian(bytes, std::uint16_t{0});
    return bytes;
}

std::optional<PlayerAnimationStatePayload> DecodePlayerAnimationState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kPlayerAnimationStatePayloadSize) {
        return std::nullopt;
    }

    std::size_t offset{};
    PlayerAnimationStatePayload payload;
    payload.entityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.slot = static_cast<PlayerSlot>(
        ReadLittleEndian<std::uint8_t>(bytes, offset));
    payload.schemaVersion =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.locomotionEpoch =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.sampleSequence =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.capabilities =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.flags = ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.graphHash = ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.stateHash = ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.primaryClipHash =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.secondaryClipHash =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.primaryNormalizedPhase = ReadFloat(bytes, offset);
    payload.secondaryNormalizedPhase = ReadFloat(bytes, offset);
    payload.primaryPlaybackRate = ReadFloat(bytes, offset);
    payload.secondaryPlaybackRate = ReadFloat(bytes, offset);
    payload.primaryBlendWeight = ReadFloat(bytes, offset);
    payload.secondaryBlendWeight = ReadFloat(bytes, offset);
    payload.transitionProgress = ReadFloat(bytes, offset);
    payload.source = static_cast<PlayerAnimationSampleSource>(
        ReadLittleEndian<std::uint8_t>(bytes, offset));
    const auto reservedByte =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reservedWord =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    if (reservedByte != 0U || reservedWord != 0U || !IsValid(payload)) {
        return std::nullopt;
    }
    return payload;
}

std::vector<std::uint8_t> EncodeMotionReplicationConfig(
    const MotionReplicationConfigPayload& payload) {
    if (!IsValid(payload)) {
        throw std::invalid_argument(
            "MotionReplicationConfig contains invalid fields");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kMotionReplicationConfigPayloadSize);
    AppendLittleEndian(bytes, payload.schemaVersion);
    AppendLittleEndian(bytes, payload.mode);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, payload.revision);
    return bytes;
}

std::optional<MotionReplicationConfigPayload> DecodeMotionReplicationConfig(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kMotionReplicationConfigPayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    MotionReplicationConfigPayload payload;
    payload.schemaVersion =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.mode = static_cast<MotionReplicationWireMode>(
        ReadLittleEndian<std::uint8_t>(bytes, offset));
    payload.flags = ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.revision = ReadLittleEndian<std::uint32_t>(bytes, offset);
    return IsValid(payload)
        ? std::optional<MotionReplicationConfigPayload>{payload}
        : std::nullopt;
}

}  // namespace coopstory::bridge
