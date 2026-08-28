#include "coopstory/bridge/FrameCodec.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace coopstory::bridge {
namespace {

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
void AppendLittleEndian(std::vector<std::uint8_t>& destination, T value) {
    using Raw = typename RawIntegral<T>::Type;
    using Unsigned = std::make_unsigned_t<Raw>;
    auto bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
        destination.push_back(static_cast<std::uint8_t>(bits & 0xFFU));
        bits >>= 8U;
    }
}

void AppendFloat(std::vector<std::uint8_t>& destination, const float value) {
    AppendLittleEndian(destination, std::bit_cast<std::uint32_t>(value));
}

template <typename T>
    requires(std::is_integral_v<T>)
[[nodiscard]] T ReadLittleEndian(
    const std::span<const std::uint8_t> source,
    std::size_t& offset) {
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned value{};
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<Unsigned>(source[offset++]) << (index * 8U);
    }
    return static_cast<T>(value);
}

[[nodiscard]] float ReadFloat(
    const std::span<const std::uint8_t> source,
    std::size_t& offset) {
    return std::bit_cast<float>(
        ReadLittleEndian<std::uint32_t>(source, offset));
}

[[nodiscard]] bool IsKnownLifecycle(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(PlayerLifecycle::Spectator);
}

[[nodiscard]] bool IsKnownSlot(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(PlayerSlot::Guest);
}

[[nodiscard]] bool IsKnownTraversalKind(
    const std::uint8_t value) noexcept {
    return value <=
           static_cast<std::uint8_t>(PlayerTraversalKind::Climb);
}

[[nodiscard]] bool IsKnownLocomotionMode(
    const std::uint8_t value) noexcept {
    return value <=
           static_cast<std::uint8_t>(PlayerLocomotionMode::Mounted);
}

[[nodiscard]] bool IsKnownMissionPhase(
    const std::uint8_t value) noexcept {
    return value <=
           static_cast<std::uint8_t>(MissionPhase::SoloOverride);
}

[[nodiscard]] bool IsKnownMissionCinematicPhase(
    const std::uint8_t value) noexcept {
    return value >= static_cast<std::uint8_t>(
                        MissionCinematicPhase::Playing) &&
           value <= static_cast<std::uint8_t>(
                        MissionCinematicPhase::Aborted);
}

[[nodiscard]] bool IsKnownMissionCinematicActionKind(
    const std::uint8_t value) noexcept {
    return value >= static_cast<std::uint8_t>(
                        MissionCinematicActionKind::PresentationReady) &&
           value <= static_cast<std::uint8_t>(
                        MissionCinematicActionKind::SkipRequest);
}

[[nodiscard]] bool IsKnownPlayerActionKind(
    const std::uint8_t value) noexcept {
    return value >=
               static_cast<std::uint8_t>(PlayerActionKind::Aim) &&
           value <=
               static_cast<std::uint8_t>(PlayerActionKind::Crafting);
}

[[nodiscard]] bool IsKnownPlayerActionPhase(
    const std::uint8_t value) noexcept {
    return value >=
               static_cast<std::uint8_t>(PlayerActionPhase::Begin) &&
           value <=
               static_cast<std::uint8_t>(PlayerActionPhase::Snapshot);
}

[[nodiscard]] bool IsKnownInteractionKind(
    const std::uint8_t value) noexcept {
    return value >=
               static_cast<std::uint8_t>(
                   InteractionKind::ReleaseRestraint) &&
           value <=
               static_cast<std::uint8_t>(
                   InteractionKind::EmergencyRecover);
}

[[nodiscard]] bool IsKnownInteractionIntentPhase(
    const std::uint8_t value) noexcept {
    return value >=
               static_cast<std::uint8_t>(
                   InteractionIntentPhase::Begin) &&
           value <=
               static_cast<std::uint8_t>(
                   InteractionIntentPhase::Cancel);
}

[[nodiscard]] bool IsKnownInteractionResultStatus(
    const std::uint8_t value) noexcept {
    return value >=
               static_cast<std::uint8_t>(
                   InteractionResultStatus::Accepted) &&
           value <=
               static_cast<std::uint8_t>(
                   InteractionResultStatus::Cancelled);
}

[[nodiscard]] bool IsKnownInteractionRejectReason(
    const std::uint8_t value) noexcept {
    return value <=
           static_cast<std::uint8_t>(
               InteractionRejectReason::SessionReset);
}

[[nodiscard]] bool IsKnownRestraintState(
    const std::uint8_t value) noexcept {
    return value <=
           static_cast<std::uint8_t>(
               PlayerRestraintState::Hogtied);
}

[[nodiscard]] bool IsKnownWorldEntityKind(
    const std::uint8_t value) noexcept {
    return value ==
               static_cast<std::uint8_t>(WorldEntityKind::Ped) ||
           value ==
               static_cast<std::uint8_t>(WorldEntityKind::Object);
}

[[nodiscard]] bool IsKnownWorldCombatTargetSlot(
    const std::uint8_t value) noexcept {
    return value <=
           static_cast<std::uint8_t>(
               WorldCombatTargetSlot::Guest);
}

[[nodiscard]] bool IsKnownWorldTaskKind(
    const std::uint8_t value) noexcept {
    return value <=
           static_cast<std::uint8_t>(WorldTaskKind::Cinematic);
}

[[nodiscard]] bool IsKnownCommandOpcode(const std::uint16_t value) noexcept {
    return value >= static_cast<std::uint16_t>(CommandOpcode::SpawnReplica) &&
           value <=
               static_cast<std::uint16_t>(
                   CommandOpcode::DiagnosticMarker);
}

[[nodiscard]] bool IsFinite(const Vec3 value) noexcept {
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z);
}

[[nodiscard]] bool IsZero(const Vec3 value) noexcept {
    return value.x == 0.0F &&
           value.y == 0.0F &&
           value.z == 0.0F;
}

[[nodiscard]] bool IsValidHealth(const float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] bool IsUnicodeWhitespace(
    const std::uint32_t codePoint) noexcept {
    return (codePoint >= 0x0009U && codePoint <= 0x000DU) ||
           codePoint == 0x0020U ||
           codePoint == 0x0085U ||
           codePoint == 0x00A0U ||
           codePoint == 0x1680U ||
           (codePoint >= 0x2000U && codePoint <= 0x200AU) ||
           codePoint == 0x2028U ||
           codePoint == 0x2029U ||
           codePoint == 0x202FU ||
           codePoint == 0x205FU ||
           codePoint == 0x3000U;
}

[[nodiscard]] bool IsDisallowedNicknameCodePoint(
    const std::uint32_t codePoint) noexcept {
    if (codePoint <= 0x001FU ||
        (codePoint >= 0x007FU && codePoint <= 0x009FU) ||
        codePoint == static_cast<std::uint32_t>('~') ||
        codePoint == 0x2028U ||
        codePoint == 0x2029U) {
        return true;
    }

    // Unicode format characters are not visible and can reorder or hide HUD
    // text. Keep the wire rule fail-closed without depending on the process
    // locale or a platform Unicode database.
    return codePoint == 0x00ADU ||
           (codePoint >= 0x0600U && codePoint <= 0x0605U) ||
           codePoint == 0x061CU ||
           codePoint == 0x06DDU ||
           codePoint == 0x070FU ||
           (codePoint >= 0x0890U && codePoint <= 0x0891U) ||
           codePoint == 0x08E2U ||
           codePoint == 0x180EU ||
           (codePoint >= 0x200BU && codePoint <= 0x200FU) ||
           (codePoint >= 0x202AU && codePoint <= 0x202EU) ||
           (codePoint >= 0x2060U && codePoint <= 0x2064U) ||
           (codePoint >= 0x2066U && codePoint <= 0x206FU) ||
           codePoint == 0xFEFFU ||
           (codePoint >= 0xFFF9U && codePoint <= 0xFFFBU) ||
           codePoint == 0x110BDU ||
           codePoint == 0x110CDU ||
           codePoint == 0xE0001U ||
           (codePoint >= 0xE0020U && codePoint <= 0xE007FU);
}

[[nodiscard]] bool DecodeUtf8CodePoint(
    const std::string_view text,
    std::size_t& offset,
    std::uint32_t& codePoint) noexcept {
    if (offset >= text.size()) {
        return false;
    }

    const auto first =
        static_cast<std::uint8_t>(text[offset++]);
    std::size_t continuationCount{};
    std::uint32_t minimum{};
    if (first <= 0x7FU) {
        codePoint = first;
        return true;
    }
    if (first >= 0xC2U && first <= 0xDFU) {
        codePoint = first & 0x1FU;
        continuationCount = 1U;
        minimum = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        codePoint = first & 0x0FU;
        continuationCount = 2U;
        minimum = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        codePoint = first & 0x07U;
        continuationCount = 3U;
        minimum = 0x10000U;
    } else {
        return false;
    }

    if (continuationCount > text.size() - offset) {
        return false;
    }
    for (std::size_t index = 0U;
         index < continuationCount;
         ++index) {
        const auto continuation =
            static_cast<std::uint8_t>(text[offset++]);
        if ((continuation & 0xC0U) != 0x80U) {
            return false;
        }
        codePoint =
            (codePoint << 6U) |
            (continuation & 0x3FU);
    }

    return codePoint >= minimum &&
           codePoint <= 0x10FFFFU &&
           !(codePoint >= 0xD800U && codePoint <= 0xDFFFU);
}

[[nodiscard]] bool IsValidPlayerNickname(
    const std::string_view nickname) noexcept {
    if (nickname.empty() ||
        nickname.size() > kMaximumPlayerNicknameUtf8Bytes) {
        return false;
    }

    std::size_t offset{};
    std::size_t count{};
    std::uint32_t firstCodePoint{};
    std::uint32_t lastCodePoint{};
    while (offset < nickname.size()) {
        std::uint32_t codePoint{};
        if (!DecodeUtf8CodePoint(
                nickname,
                offset,
                codePoint) ||
            IsDisallowedNicknameCodePoint(codePoint)) {
            return false;
        }
        if (count == 0U) {
            firstCodePoint = codePoint;
        }
        lastCodePoint = codePoint;
        ++count;
        if (count > kMaximumPlayerNicknameCodePoints) {
            return false;
        }
    }

    return count > 0U &&
           !IsUnicodeWhitespace(firstCodePoint) &&
           !IsUnicodeWhitespace(lastCodePoint);
}

[[nodiscard]] bool IsValidWorldState(
    const WorldStatePayload& payload) noexcept {
    constexpr auto kKnownFlags =
        static_cast<std::uint8_t>(
            WorldStateFlag::WeatherValid);
    if (payload.hour > 23U ||
        payload.minute > 59U ||
        payload.second > 59U ||
        payload.day < 1U ||
        payload.day > 31U ||
        payload.month > 11U ||
        payload.year < 1800U ||
        payload.year > 2200U ||
        (payload.flags & ~kKnownFlags) != 0U ||
        !std::isfinite(payload.weatherBlend) ||
        payload.weatherBlend < 0.0F ||
        payload.weatherBlend > 1.0F) {
        return false;
    }
    if ((payload.flags & kKnownFlags) == 0U) {
        return payload.weatherFrom == 0U &&
               payload.weatherTo == 0U &&
               payload.weatherBlend == 0.0F;
    }
    return payload.weatherFrom != 0U &&
           payload.weatherTo != 0U;
}

[[nodiscard]] bool IsValidMissionState(
    const MissionStatePayload& payload) noexcept {
    constexpr auto kAnchorValid =
        static_cast<std::uint8_t>(
            MissionStateFlag::AnchorValid);
    constexpr auto kMissionActive =
        static_cast<std::uint8_t>(
            MissionStateFlag::MissionActive);
    constexpr auto kCheckpointRecovery =
        static_cast<std::uint8_t>(
            MissionStateFlag::CheckpointRecovery);
    constexpr auto kScriptedControlLock =
        static_cast<std::uint8_t>(MissionStateFlag::ScriptedControlLock);
    constexpr auto kScreenTransition =
        static_cast<std::uint8_t>(MissionStateFlag::ScreenTransition);
    constexpr auto kScenarioActivity =
        static_cast<std::uint8_t>(MissionStateFlag::ScenarioActivity);
    constexpr auto kScriptedVehicleTransition =
        static_cast<std::uint8_t>(
            MissionStateFlag::ScriptedVehicleTransition);
    constexpr auto kMinigameActivity =
        static_cast<std::uint8_t>(MissionStateFlag::MinigameActivity);
    constexpr auto kKnownFlags =
        kAnchorValid |
        kMissionActive |
        kCheckpointRecovery |
        kScriptedControlLock |
        kScreenTransition |
        kScenarioActivity |
        kScriptedVehicleTransition |
        kMinigameActivity;
    const auto phase =
        static_cast<std::uint8_t>(payload.phase);
    const bool anchorValid =
        (payload.flags & kAnchorValid) != 0U;
    const bool missionActive =
        (payload.flags & kMissionActive) != 0U;
    const bool checkpointRecovery =
        (payload.flags & kCheckpointRecovery) != 0U;
    if (!payload.hostEntityId.IsValid() ||
        payload.missionEpoch == 0U ||
        payload.revision == 0U ||
        !IsKnownMissionPhase(phase) ||
        (payload.flags & ~kKnownFlags) != 0U ||
        !IsFinite(payload.hostAnchor) ||
        !std::isfinite(payload.hostHeading) ||
        (!anchorValid &&
         (!IsZero(payload.hostAnchor) ||
          payload.hostHeading != 0.0F)) ||
        (payload.phase == MissionPhase::Idle &&
         missionActive) ||
        (payload.phase == MissionPhase::Active &&
         !missionActive) ||
        (checkpointRecovery !=
         (payload.phase == MissionPhase::Recovery))) {
        return false;
    }
    return true;
}

[[nodiscard]] bool IsValidMissionCameraState(
    const MissionCameraStatePayload& payload) noexcept {
    constexpr auto kActive =
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::Active);
    constexpr auto kFadedOut =
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::ScreenFadedOut);
    constexpr auto kFadingOut =
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::ScreenFadingOut);
    constexpr auto kFadingIn =
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::ScreenFadingIn);
    constexpr auto kRenderingScript =
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::SourceRenderingScriptCamera);
    constexpr auto kCinematicGameplay =
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::SourceCinematicGameplayCamera);
    constexpr auto kGameplayFallback =
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::SourceGameplayCameraFallback);
    constexpr auto kFadeFlags = kFadedOut | kFadingOut | kFadingIn;
    constexpr auto kSourceFlags =
        kRenderingScript | kCinematicGameplay | kGameplayFallback;
    constexpr auto kKnownFlags = kActive | kFadeFlags | kSourceFlags;
    const bool active = (payload.flags & kActive) != 0U;
    const auto fadeFlags = payload.flags & kFadeFlags;
    const auto sourceFlags = payload.flags & kSourceFlags;
    if (!payload.hostEntityId.IsValid() ||
        payload.missionEpoch == 0U ||
        payload.cinematicGeneration == 0U ||
        payload.revision == 0U ||
        (payload.flags & ~kKnownFlags) != 0U ||
        (fadeFlags != 0U &&
         (fadeFlags & (fadeFlags - 1U)) != 0U) ||
        (active &&
         (sourceFlags == 0U ||
          (sourceFlags & (sourceFlags - 1U)) != 0U)) ||
        (!active && sourceFlags != 0U) ||
        !IsFinite(payload.position) ||
        !IsFinite(payload.rotation) ||
        !std::isfinite(payload.fieldOfView)) {
        return false;
    }
    if (active) {
        return payload.fieldOfView >= 1.0F &&
               payload.fieldOfView <= 179.0F;
    }
    return IsZero(payload.position) &&
           IsZero(payload.rotation) &&
           payload.fieldOfView == 0.0F;
}

[[nodiscard]] bool IsValidMissionCinematicState(
    const MissionCinematicStatePayload& payload) noexcept {
    constexpr auto kCameraExpected = static_cast<std::uint16_t>(
        MissionCinematicStateFlag::CameraExpected);
    constexpr auto kAnchorValid = static_cast<std::uint16_t>(
        MissionCinematicStateFlag::AnchorValid);
    constexpr auto kSkipPending = static_cast<std::uint16_t>(
        MissionCinematicStateFlag::SkipPending);
    constexpr auto kResumeTimedOut = static_cast<std::uint16_t>(
        MissionCinematicStateFlag::ResumeTimedOut);
    constexpr auto kKnownFlags =
        kCameraExpected | kAnchorValid | kSkipPending | kResumeTimedOut;
    const bool anchorValid = (payload.flags & kAnchorValid) != 0U;
    const bool skipPending = (payload.flags & kSkipPending) != 0U;
    const bool resumeTimedOut = (payload.flags & kResumeTimedOut) != 0U;
    const auto phase = static_cast<std::uint8_t>(payload.phase);
    if (!payload.hostEntityId.IsValid() ||
        payload.missionEpoch == 0U ||
        payload.cinematicGeneration == 0U ||
        payload.revision == 0U ||
        payload.checkpointGeneration == 0U ||
        !IsKnownMissionCinematicPhase(phase) ||
        (payload.flags & ~kKnownFlags) != 0U ||
        !IsFinite(payload.resumeAnchor) ||
        !std::isfinite(payload.resumeHeading) ||
        (!anchorValid &&
         (!IsZero(payload.resumeAnchor) || payload.resumeHeading != 0.0F)) ||
        ((payload.phase == MissionCinematicPhase::PrepareResume ||
          payload.phase == MissionCinematicPhase::Completed) &&
         !anchorValid) ||
        (resumeTimedOut &&
         payload.phase != MissionCinematicPhase::Completed &&
         payload.phase != MissionCinematicPhase::Aborted) ||
        (skipPending &&
         payload.phase != MissionCinematicPhase::Playing &&
         payload.phase != MissionCinematicPhase::Loading)) {
        return false;
    }
    return true;
}

[[nodiscard]] bool IsValidMissionCinematicAction(
    const MissionCinematicActionPayload& payload) noexcept {
    constexpr auto kFallbackUsed = static_cast<std::uint16_t>(
        MissionCinematicActionFlag::FallbackUsed);
    const bool fallbackUsed = (payload.flags & kFallbackUsed) != 0U;
    return payload.hostEntityId.IsValid() &&
           payload.missionEpoch != 0U &&
           payload.cinematicGeneration != 0U &&
           payload.actionId != 0U &&
           IsKnownMissionCinematicActionKind(
               static_cast<std::uint8_t>(payload.kind)) &&
           IsKnownSlot(payload.senderSlot) &&
           (payload.flags & ~kFallbackUsed) == 0U &&
           (!fallbackUsed ||
            payload.kind == MissionCinematicActionKind::ResumeReady);
}

[[nodiscard]] bool IsValidPlayerAppearanceState(
    const PlayerAppearanceStatePayload& payload) noexcept {
    constexpr auto kComplete = static_cast<std::uint16_t>(
        PlayerAppearanceStateFlag::CompleteComponentSet);
    constexpr auto kStoryMetaPed = static_cast<std::uint16_t>(
        PlayerAppearanceStateFlag::StoryMetaPed);
    constexpr auto kKnownFlags = kComplete | kStoryMetaPed;
    if (!payload.entityId.IsValid() ||
        !IsKnownSlot(static_cast<std::uint8_t>(payload.slot)) ||
        payload.schemaVersion != 1U ||
        payload.revision == 0U ||
        payload.modelHash == 0U ||
        payload.fingerprint == 0U ||
        (payload.flags & ~kKnownFlags) != 0U ||
        payload.componentHashes.empty() ||
        payload.componentHashes.size() >
            kMaximumPlayerAppearanceComponents) {
        return false;
    }
    for (std::size_t index = 0;
         index < payload.componentHashes.size();
         ++index) {
        if (payload.componentHashes[index] == 0U) {
            return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (payload.componentHashes[previous] ==
                payload.componentHashes[index]) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool IsValidAnimSceneReplicaState(
    const AnimSceneReplicaStatePayload& payload) noexcept {
    constexpr auto kActive = static_cast<std::uint32_t>(
        AnimSceneReplicaStateFlag::Active);
    constexpr auto kRunning = static_cast<std::uint32_t>(
        AnimSceneReplicaStateFlag::Running);
    constexpr auto kLoaded = static_cast<std::uint32_t>(
        AnimSceneReplicaStateFlag::Loaded);
    constexpr auto kCamera = static_cast<std::uint32_t>(
        AnimSceneReplicaStateFlag::CameraActive);
    constexpr auto kOrigin = static_cast<std::uint32_t>(
        AnimSceneReplicaStateFlag::OriginValid);
    constexpr auto kKnownFlags =
        kActive | kRunning | kLoaded | kCamera | kOrigin;
    const bool originValid = (payload.flags & kOrigin) != 0U;
    return payload.hostEntityId.IsValid() &&
           payload.missionEpoch != 0U &&
           payload.cinematicGeneration != 0U &&
           payload.revision != 0U &&
           payload.dictionaryHash != 0U &&
           (payload.flags & kActive) != 0U &&
           (payload.flags & ~kKnownFlags) == 0U &&
           std::isfinite(payload.phase) &&
           payload.phase >= 0.0F && payload.phase <= 1.05F &&
           std::isfinite(payload.durationSeconds) &&
           payload.durationSeconds > 0.0F &&
           payload.durationSeconds <= 7'200.0F &&
           std::isfinite(payload.rate) &&
           payload.rate >= 0.0F && payload.rate <= 4.0F &&
           IsFinite(payload.originPosition) &&
           IsFinite(payload.originRotation) &&
           (originValid ||
            (IsZero(payload.originPosition) &&
             IsZero(payload.originRotation))) &&
           payload.activeCameraCount <= 32U;
}

[[nodiscard]] bool IsKnownAnimSceneRoleKind(
    const std::uint8_t value) noexcept {
    return value >= static_cast<std::uint8_t>(AnimSceneRoleKind::Ped) &&
           value <= static_cast<std::uint8_t>(AnimSceneRoleKind::Pickup);
}

[[nodiscard]] bool IsKnownAnimSceneControlKind(
    const std::uint8_t value) noexcept {
    return value >=
               static_cast<std::uint8_t>(AnimSceneControlKind::GuestReady) &&
           value <=
               static_cast<std::uint8_t>(AnimSceneControlKind::HostAbort);
}

[[nodiscard]] bool IsKnownAnimSceneControlReason(
    const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(
                        AnimSceneControlReason::StaleGeneration);
}

[[nodiscard]] bool IsPrintableAscii(
    const std::string_view value,
    const bool allowEmpty,
    const std::size_t maximumBytes) noexcept {
    if ((!allowEmpty && value.empty()) || value.size() > maximumBytes) {
        return false;
    }
    for (const unsigned char character : value) {
        if (character < 0x20U || character > 0x7EU) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsValidAnimSceneDefinitionShape(
    const AnimSceneDefinitionPayload& payload) noexcept {
    if (!payload.hostEntityId.IsValid() ||
        payload.missionEpoch == 0U ||
        payload.cinematicGeneration == 0U ||
        payload.definitionRevision == 0U ||
        payload.dictionaryHash == 0U ||
        (payload.createOptionFlags & ~0x03U) != 0U ||
        !std::isfinite(payload.durationSeconds) ||
        payload.durationSeconds <= 0.0F ||
        payload.durationSeconds > 7'200.0F ||
        !IsPrintableAscii(
            payload.resourceName,
            false,
            kMaximumAnimSceneResourceBytes) ||
        !IsPrintableAscii(
            payload.playbackList,
            true,
            kMaximumAnimScenePlaybackListBytes) ||
        payload.roles.size() > kMaximumAnimSceneDefinitionRoles) {
        return false;
    }

    std::size_t encodedSize =
        kAnimSceneDefinitionHeaderSize +
        payload.resourceName.size() +
        payload.playbackList.size();
    std::string_view previousRoleName{};
    bool hasPreviousRole{};
    constexpr auto kRequired = static_cast<std::uint16_t>(
        AnimSceneRoleFlag::Required);
    constexpr auto kPlayer = static_cast<std::uint16_t>(
        AnimSceneRoleFlag::Player);
    constexpr auto kKnownFlags = kRequired | kPlayer;
    for (std::size_t index = 0U; index < payload.roles.size(); ++index) {
        const auto& role = payload.roles[index];
        if (!IsPrintableAscii(
                role.roleName,
                false,
                kMaximumAnimSceneRoleNameBytes) ||
            (hasPreviousRole && !(previousRoleName < role.roleName)) ||
            !IsKnownAnimSceneRoleKind(
                static_cast<std::uint8_t>(role.kind)) ||
            (role.flags & ~kKnownFlags) != 0U) {
            return false;
        }
        previousRoleName = role.roleName;
        hasPreviousRole = true;

        const bool mapped = role.entityId.IsValid();
        if ((role.entityId.Value() != 0U && !mapped) ||
            (mapped && role.modelHash == 0U) ||
            (!mapped &&
             (role.modelHash != 0U || role.bindingFlags != 0U)) ||
            ((role.flags & (kRequired | kPlayer)) != 0U && !mapped) ||
            ((role.flags & kPlayer) != 0U &&
             role.kind != AnimSceneRoleKind::Ped)) {
            return false;
        }
        if (mapped) {
            for (std::size_t previous = 0U; previous < index; ++previous) {
                if (payload.roles[previous].entityId == role.entityId) {
                    return false;
                }
            }
        }
        encodedSize +=
            kAnimSceneRoleBindingHeaderSize + role.roleName.size();
        if (encodedSize > kMaximumAnimSceneDefinitionPayloadSize) {
            return false;
        }
    }
    return encodedSize <= kMaximumAnimSceneDefinitionPayloadSize;
}

[[nodiscard]] std::vector<std::uint8_t> SerializeAnimSceneDefinition(
    const AnimSceneDefinitionPayload& payload,
    const bool clearFingerprint) {
    std::size_t size =
        kAnimSceneDefinitionHeaderSize +
        payload.resourceName.size() +
        payload.playbackList.size();
    for (const auto& role : payload.roles) {
        size += kAnimSceneRoleBindingHeaderSize + role.roleName.size();
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(size);
    AppendLittleEndian(bytes, payload.hostEntityId.Value());
    AppendLittleEndian(bytes, payload.missionEpoch);
    AppendLittleEndian(bytes, payload.cinematicGeneration);
    AppendLittleEndian(bytes, payload.definitionRevision);
    AppendLittleEndian(bytes, payload.dictionaryHash);
    AppendLittleEndian(
        bytes,
        clearFingerprint ? std::uint64_t{} : payload.fingerprintLow);
    AppendLittleEndian(
        bytes,
        clearFingerprint ? std::uint64_t{} : payload.fingerprintHigh);
    AppendFloat(bytes, payload.durationSeconds);
    AppendLittleEndian(
        bytes,
        static_cast<std::uint16_t>(payload.resourceName.size()));
    AppendLittleEndian(
        bytes,
        static_cast<std::uint16_t>(payload.playbackList.size()));
    AppendLittleEndian(
        bytes,
        static_cast<std::uint16_t>(payload.roles.size()));
    AppendLittleEndian(bytes, std::uint16_t{});
    AppendLittleEndian(bytes, payload.sceneFlags);
    AppendLittleEndian(bytes, payload.createOptionFlags);
    AppendLittleEndian(bytes, std::uint8_t{});
    AppendLittleEndian(bytes, std::uint8_t{});
    AppendLittleEndian(bytes, std::uint8_t{});
    bytes.insert(
        bytes.end(),
        payload.resourceName.begin(),
        payload.resourceName.end());
    bytes.insert(
        bytes.end(),
        payload.playbackList.begin(),
        payload.playbackList.end());
    for (const auto& role : payload.roles) {
        AppendLittleEndian(bytes, role.entityId.Value());
        AppendLittleEndian(bytes, role.modelHash);
        AppendLittleEndian(bytes, role.bindingFlags);
        AppendLittleEndian(bytes, role.flags);
        AppendLittleEndian(bytes, role.kind);
        AppendLittleEndian(
            bytes,
            static_cast<std::uint8_t>(role.roleName.size()));
        bytes.insert(
            bytes.end(),
            role.roleName.begin(),
            role.roleName.end());
    }
    return bytes;
}

[[nodiscard]] std::optional<AnimSceneDefinitionFingerprint>
TryComputeAnimSceneDefinitionFingerprint(
    const AnimSceneDefinitionPayload& payload) noexcept {
    try {
        auto canonical = SerializeAnimSceneDefinition(payload, true);
        BCRYPT_ALG_HANDLE algorithm{};
        const auto openStatus = ::BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0U);
        if (!BCRYPT_SUCCESS(openStatus)) {
            return std::nullopt;
        }
        std::array<std::uint8_t, 32U> digest{};
        const auto hashStatus = ::BCryptHash(
            algorithm,
            nullptr,
            0U,
            canonical.data(),
            static_cast<ULONG>(canonical.size()),
            digest.data(),
            static_cast<ULONG>(digest.size()));
        ::BCryptCloseAlgorithmProvider(algorithm, 0U);
        if (!BCRYPT_SUCCESS(hashStatus)) {
            return std::nullopt;
        }
        const std::span<const std::uint8_t> digestBytes{digest};
        std::size_t offset{};
        return AnimSceneDefinitionFingerprint{
            ReadLittleEndian<std::uint64_t>(digestBytes, offset),
            ReadLittleEndian<std::uint64_t>(digestBytes, offset)};
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] bool IsValidAnimSceneDefinition(
    const AnimSceneDefinitionPayload& payload) noexcept {
    if (!IsValidAnimSceneDefinitionShape(payload) ||
        (payload.fingerprintLow | payload.fingerprintHigh) == 0U) {
        return false;
    }
    const auto expected =
        TryComputeAnimSceneDefinitionFingerprint(payload);
    return expected.has_value() &&
           expected->low == payload.fingerprintLow &&
           expected->high == payload.fingerprintHigh;
}

[[nodiscard]] bool IsValidAnimSceneControl(
    const AnimSceneControlPayload& payload) noexcept {
    constexpr auto kResourceLoaded = static_cast<std::uint32_t>(
        AnimSceneControlFlag::ResourceLoaded);
    constexpr auto kRequiredRolesBound = static_cast<std::uint32_t>(
        AnimSceneControlFlag::RequiredRolesBound);
    constexpr auto kCacheHit = static_cast<std::uint32_t>(
        AnimSceneControlFlag::CacheHit);
    constexpr auto kLateJoin = static_cast<std::uint32_t>(
        AnimSceneControlFlag::LateJoin);
    constexpr auto kFallbackUsed = static_cast<std::uint32_t>(
        AnimSceneControlFlag::FallbackUsed);
    constexpr auto kKnownFlags =
        kResourceLoaded | kRequiredRolesBound | kCacheHit |
        kLateJoin | kFallbackUsed;
    if (!payload.hostEntityId.IsValid() ||
        payload.missionEpoch == 0U ||
        payload.cinematicGeneration == 0U ||
        payload.definitionRevision == 0U ||
        payload.actionId == 0U ||
        (payload.fingerprintLow | payload.fingerprintHigh) == 0U ||
        !IsKnownAnimSceneControlKind(
            static_cast<std::uint8_t>(payload.kind)) ||
        !IsKnownAnimSceneControlReason(
            static_cast<std::uint8_t>(payload.reason)) ||
        payload.senderSlot > 1U ||
        (payload.flags & ~kKnownFlags) != 0U ||
        !std::isfinite(payload.startPhase) ||
        !std::isfinite(payload.rate)) {
        return false;
    }

    switch (payload.kind) {
        case AnimSceneControlKind::GuestReady:
            return payload.senderSlot == 1U &&
                   payload.reason == AnimSceneControlReason::None &&
                   payload.playAtHostTick == 0U &&
                   payload.startPhase == 0.0F &&
                   payload.rate == 0.0F &&
                   (payload.flags &
                    (kResourceLoaded | kRequiredRolesBound)) ==
                       (kResourceLoaded | kRequiredRolesBound) &&
                   (payload.flags &
                    ~(kResourceLoaded | kRequiredRolesBound | kCacheHit)) ==
                       0U;
        case AnimSceneControlKind::GuestRejected:
            return payload.senderSlot == 1U &&
                   payload.reason != AnimSceneControlReason::None &&
                   payload.playAtHostTick == 0U &&
                   payload.startPhase == 0.0F &&
                   payload.rate == 0.0F &&
                   payload.flags == 0U;
        case AnimSceneControlKind::HostPlayCommit:
            return payload.senderSlot == 0U &&
                   payload.reason == AnimSceneControlReason::None &&
                   payload.playAtHostTick != 0U &&
                   payload.startPhase >= 0.0F &&
                   payload.startPhase <= 1.05F &&
                   payload.rate > 0.0F && payload.rate <= 4.0F &&
                   (payload.flags & ~kLateJoin) == 0U;
        case AnimSceneControlKind::HostAbort:
            return payload.senderSlot == 0U &&
                   payload.reason != AnimSceneControlReason::None &&
                   payload.playAtHostTick == 0U &&
                   payload.startPhase == 0.0F &&
                   payload.rate == 0.0F &&
                   (payload.flags & ~kFallbackUsed) == 0U;
        default:
            return false;
    }
}

[[nodiscard]] bool IsValidWorldEntityState(
    const WorldEntityStatePayload& payload) noexcept {
    constexpr auto kHuman =
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Human);
    constexpr auto kHorse =
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Horse);
    constexpr auto kInCombat =
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::InCombat);
    constexpr auto kFiring =
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Firing);
    constexpr auto kAiming =
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Aiming);
    constexpr auto kMounted =
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Mounted);
    constexpr auto kScriptOwned =
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::ScriptOwned);
    constexpr auto kKnownFlags =
        kHuman |
        kHorse |
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Dead) |
        kInCombat |
        kFiring |
        kAiming |
        kMounted |
        kScriptOwned;
    const auto kind =
        static_cast<std::uint8_t>(payload.kind);
    const auto combatTarget =
        static_cast<std::uint8_t>(
            payload.combatTargetSlot);
    const auto human = (payload.flags & kHuman) != 0U;
    const auto horse = (payload.flags & kHorse) != 0U;
    const auto inCombat =
        (payload.flags & kInCombat) != 0U;
    const auto usesWeapon =
        (payload.flags & (kFiring | kAiming)) != 0U;
    const auto mounted =
        (payload.flags & kMounted) != 0U;
    const auto object =
        payload.kind == WorldEntityKind::Object;
    const auto objectSemantics =
        !object ||
        (!human && !horse && !inCombat && !usesWeapon && !mounted &&
         payload.combatTargetSlot == WorldCombatTargetSlot::None &&
         !payload.parentEntityId.IsValid() &&
         payload.weaponHash == 0U &&
         (payload.taskKind == WorldTaskKind::Idle ||
          payload.taskKind == WorldTaskKind::Cinematic));

    return payload.entityId.IsValid() &&
           payload.modelHash != 0U &&
           IsKnownWorldEntityKind(kind) &&
           objectSemantics &&
           (payload.flags & ~kKnownFlags) == 0U &&
           IsKnownWorldCombatTargetSlot(combatTarget) &&
           IsKnownWorldTaskKind(
               static_cast<std::uint8_t>(
                   payload.taskKind)) &&
           (inCombat ||
            payload.combatTargetSlot ==
                WorldCombatTargetSlot::None) &&
           !(human && horse) &&
           (mounted == payload.parentEntityId.IsValid()) &&
           (!mounted ||
            (human &&
             payload.parentEntityId != payload.entityId &&
             payload.taskKind == WorldTaskKind::Mounted)) &&
           (payload.taskKind != WorldTaskKind::Dead ||
            (payload.flags &
             static_cast<std::uint8_t>(
                 WorldEntityStateFlag::Dead)) != 0U) &&
           (human || payload.weaponHash == 0U) &&
           (!usesWeapon ||
            (human && payload.weaponHash != 0U)) &&
           IsFinite(payload.position) &&
           IsFinite(payload.velocity) &&
           std::isfinite(payload.heading) &&
           payload.heading >= 0.0F &&
           payload.heading < 360.0F &&
           IsValidHealth(payload.healthFraction) &&
           IsFinite(payload.taskTarget);
}

[[nodiscard]] bool IsValidPlayerMountState(
    const PlayerMountStatePayload& payload) noexcept {
    constexpr auto kPresent =
        static_cast<std::uint8_t>(
            PlayerMountStateFlag::Present);
    constexpr auto kKnownFlags =
        kPresent |
        static_cast<std::uint8_t>(
            PlayerMountStateFlag::Mounted) |
        static_cast<std::uint8_t>(
            PlayerMountStateFlag::Dead) |
        static_cast<std::uint8_t>(
            PlayerMountStateFlag::BorrowedPeerMount) |
        static_cast<std::uint8_t>(
            PlayerMountStateFlag::Vehicle) |
        static_cast<std::uint8_t>(
            PlayerMountStateFlag::VehicleDriver) |
        static_cast<std::uint8_t>(
            PlayerMountStateFlag::VehiclePassenger);
    const bool present =
        (payload.flags & kPresent) != 0U;
    const bool mounted =
        (payload.flags &
         static_cast<std::uint8_t>(
             PlayerMountStateFlag::Mounted)) != 0U;
    const bool borrowed =
        (payload.flags &
         static_cast<std::uint8_t>(
            PlayerMountStateFlag::BorrowedPeerMount)) != 0U;
    const bool vehicle =
        (payload.flags & static_cast<std::uint8_t>(
                             PlayerMountStateFlag::Vehicle)) != 0U;
    const bool driver =
        (payload.flags & static_cast<std::uint8_t>(
                             PlayerMountStateFlag::VehicleDriver)) != 0U;
    const bool passenger =
        (payload.flags & static_cast<std::uint8_t>(
                             PlayerMountStateFlag::VehiclePassenger)) != 0U;
    return payload.playerEntityId.IsValid() &&
           payload.mountEntityId.IsValid() &&
           payload.playerEntityId != payload.mountEntityId &&
           IsKnownSlot(
               static_cast<std::uint8_t>(payload.slot)) &&
           (payload.flags & ~kKnownFlags) == 0U &&
           (!borrowed || (present && mounted)) &&
           (!vehicle || (present && mounted && (driver != passenger))) &&
           (vehicle || (!driver && !passenger)) &&
           (present ||
            (payload.flags == 0U &&
             payload.modelHash == 0U &&
             IsZero(payload.position) &&
             IsZero(payload.velocity) &&
             payload.heading == 0.0F &&
             payload.healthFraction == 0.0F)) &&
           (!present || payload.modelHash != 0U) &&
           IsFinite(payload.position) &&
           IsFinite(payload.velocity) &&
           std::isfinite(payload.heading) &&
           payload.heading >= 0.0F &&
           payload.heading < 360.0F &&
           IsValidHealth(payload.healthFraction) &&
           payload.generation != 0U;
}

[[nodiscard]] bool IsValidPlayerAction(
    const PlayerActionPayload& payload) noexcept {
    constexpr auto kIntent =
        static_cast<std::uint32_t>(PlayerActionFlag::Intent);
    constexpr auto kAuthoritative =
        static_cast<std::uint32_t>(PlayerActionFlag::Authoritative);
    constexpr auto kTargetEntityValid =
        static_cast<std::uint32_t>(
            PlayerActionFlag::TargetEntityValid);
    constexpr auto kTargetPointValid =
        static_cast<std::uint32_t>(
            PlayerActionFlag::TargetPointValid);
    constexpr auto kActorAnchorValid =
        static_cast<std::uint32_t>(
            PlayerActionFlag::ActorAnchorValid);
    constexpr auto kPersistent =
        static_cast<std::uint32_t>(PlayerActionFlag::Persistent);
    constexpr auto kPhysicalTargetEffect =
        static_cast<std::uint32_t>(
            PlayerActionFlag::PhysicalTargetEffect);
    constexpr auto kResyncSnapshot =
        static_cast<std::uint32_t>(
            PlayerActionFlag::ResyncSnapshot);
    constexpr auto kVariantValid =
        static_cast<std::uint32_t>(PlayerActionFlag::VariantValid);
    constexpr auto kAnimationSampleValid =
        static_cast<std::uint32_t>(
            PlayerActionFlag::AnimationSampleValid);
    constexpr auto kNormalizedPhaseValid =
        static_cast<std::uint32_t>(
            PlayerActionFlag::NormalizedPhaseValid);
    constexpr auto kKnownFlags =
        kIntent |
        kAuthoritative |
        kTargetEntityValid |
        kTargetPointValid |
        kActorAnchorValid |
        kPersistent |
        kPhysicalTargetEffect |
        kResyncSnapshot |
        kVariantValid |
        kAnimationSampleValid |
        kNormalizedPhaseValid;
    constexpr std::uint32_t kMaximumActionTimeMilliseconds = 3'600'000U;

    const bool intent = (payload.flags & kIntent) != 0U;
    const bool authoritative =
        (payload.flags & kAuthoritative) != 0U;
    const bool targetEntityValid =
        (payload.flags & kTargetEntityValid) != 0U;
    const bool targetPointValid =
        (payload.flags & kTargetPointValid) != 0U;
    const bool actorAnchorValid =
        (payload.flags & kActorAnchorValid) != 0U;
    const bool variantValid =
        (payload.flags & kVariantValid) != 0U;
    const bool animationSampleValid =
        (payload.flags & kAnimationSampleValid) != 0U;
    const bool normalizedPhaseValid =
        (payload.flags & kNormalizedPhaseValid) != 0U;

    return payload.actorEntityId.IsValid() &&
           payload.sequence != 0U &&
           payload.actionId != 0U &&
           payload.revision != 0U &&
           IsKnownSlot(static_cast<std::uint8_t>(payload.actorSlot)) &&
           IsKnownSlot(static_cast<std::uint8_t>(payload.authoritySlot)) &&
           IsKnownPlayerActionKind(
               static_cast<std::uint8_t>(payload.kind)) &&
           IsKnownPlayerActionPhase(
               static_cast<std::uint8_t>(payload.phase)) &&
           (payload.flags & ~kKnownFlags) == 0U &&
           intent != authoritative &&
           (!intent || payload.actorSlot == payload.authoritySlot) &&
           (!authoritative ||
            payload.authoritySlot == PlayerSlot::Host) &&
           (targetEntityValid
                ? payload.targetEntityId.IsValid() &&
                      payload.targetEntityId != payload.actorEntityId
                : payload.targetEntityId.Value() == 0U) &&
           (targetPointValid || IsZero(payload.targetPoint)) &&
           (actorAnchorValid || IsZero(payload.actorAnchor)) &&
           (variantValid
                ? payload.variantHash != 0U
                : payload.variantHash == 0U) &&
           (animationSampleValid
                ? payload.animationSampleSequence != 0U
                : payload.animationSampleSequence == 0U) &&
           (normalizedPhaseValid || payload.normalizedPhase == 0.0F) &&
           IsFinite(payload.actorAnchor) &&
           IsFinite(payload.targetPoint) &&
           std::isfinite(payload.facingHeading) &&
           payload.facingHeading >= 0.0F &&
           payload.facingHeading < 360.0F &&
           std::isfinite(payload.normalizedPhase) &&
           (!normalizedPhaseValid ||
            (payload.normalizedPhase >= 0.0F &&
             payload.normalizedPhase <= 1.0F)) &&
           payload.durationMilliseconds <=
               kMaximumActionTimeMilliseconds &&
           payload.phaseElapsedMilliseconds <=
               kMaximumActionTimeMilliseconds &&
           (payload.durationMilliseconds == 0U ||
            payload.phaseElapsedMilliseconds <=
                payload.durationMilliseconds) &&
           ((payload.flags & kPhysicalTargetEffect) == 0U ||
            targetEntityValid) &&
           ((payload.flags & kResyncSnapshot) == 0U ||
            (authoritative && (payload.flags & kPersistent) != 0U));
}

[[nodiscard]] bool IsValidInteractionIntent(
    const InteractionIntentPayload& payload) noexcept {
    constexpr auto kTargetPlayer =
        static_cast<std::uint8_t>(
            InteractionIntentFlag::TargetPlayer);
    constexpr auto kTargetMount =
        static_cast<std::uint8_t>(
            InteractionIntentFlag::TargetMount);
    constexpr auto kHoldRequired =
        static_cast<std::uint8_t>(
            InteractionIntentFlag::HoldRequired);
    constexpr auto kKnownFlags =
        kTargetPlayer | kTargetMount | kHoldRequired;
    constexpr std::uint32_t kMaximumDurationMilliseconds = 60'000U;
    const auto phase = static_cast<std::uint8_t>(payload.phase);
    const auto kind = static_cast<std::uint8_t>(payload.kind);
    const bool targetsPlayer = (payload.flags & kTargetPlayer) != 0U;
    const bool targetsMount = (payload.flags & kTargetMount) != 0U;
    const bool holdRequired = (payload.flags & kHoldRequired) != 0U;
    const bool secondaryRequired =
        payload.kind == InteractionKind::MountDriver ||
        payload.kind == InteractionKind::MountPassenger;
    const bool cancelled =
        payload.phase == InteractionIntentPhase::Cancel;

    return payload.actorEntityId.IsValid() &&
           payload.targetEntityId.IsValid() &&
           (payload.actorEntityId != payload.targetEntityId ||
            payload.kind == InteractionKind::EmergencyRecover) &&
           payload.interactionId != 0U &&
           payload.revision != 0U &&
           IsKnownSlot(static_cast<std::uint8_t>(payload.actorSlot)) &&
           IsKnownInteractionKind(kind) &&
           IsKnownInteractionIntentPhase(phase) &&
           (payload.flags & ~kKnownFlags) == 0U &&
           targetsPlayer != targetsMount &&
           payload.requestedDurationMilliseconds <=
               kMaximumDurationMilliseconds &&
           (secondaryRequired == payload.secondaryEntityId.IsValid()) &&
           (!secondaryRequired ||
            (payload.secondaryEntityId != payload.actorEntityId &&
             payload.secondaryEntityId != payload.targetEntityId)) &&
           (cancelled
                ? payload.requestedDurationMilliseconds == 0U &&
                      !holdRequired
                : payload.kind == InteractionKind::Revive
                      ? targetsPlayer && holdRequired &&
                            payload.requestedDurationMilliseconds == 4'000U
                      : holdRequired ==
                            (payload.requestedDurationMilliseconds > 0U));
}

[[nodiscard]] bool IsValidInteractionResult(
    const InteractionResultPayload& payload) noexcept {
    constexpr auto kAuthoritative =
        static_cast<std::uint16_t>(
            InteractionResultFlag::Authoritative);
    constexpr auto kKnownFlags =
        kAuthoritative |
        static_cast<std::uint16_t>(
            InteractionResultFlag::StateChanged) |
        static_cast<std::uint16_t>(
            InteractionResultFlag::HoldRequired);
    constexpr std::uint32_t kMaximumDurationMilliseconds = 60'000U;
    const bool rejected =
        payload.status == InteractionResultStatus::Rejected;
    const bool secondaryRequired =
        payload.kind == InteractionKind::MountDriver ||
        payload.kind == InteractionKind::MountPassenger;
    return payload.actorEntityId.IsValid() &&
           payload.targetEntityId.IsValid() &&
           (payload.actorEntityId != payload.targetEntityId ||
            payload.kind == InteractionKind::EmergencyRecover) &&
           payload.interactionId != 0U &&
           payload.revision != 0U &&
           IsKnownInteractionKind(
               static_cast<std::uint8_t>(payload.kind)) &&
           IsKnownInteractionResultStatus(
               static_cast<std::uint8_t>(payload.status)) &&
           IsKnownInteractionRejectReason(
               static_cast<std::uint8_t>(payload.rejectReason)) &&
           (payload.flags & ~kKnownFlags) == 0U &&
           (payload.flags & kAuthoritative) != 0U &&
           (rejected ==
            (payload.rejectReason != InteractionRejectReason::None)) &&
           (secondaryRequired == payload.secondaryEntityId.IsValid()) &&
           payload.progressMilliseconds <=
               kMaximumDurationMilliseconds &&
           payload.requiredDurationMilliseconds <=
               kMaximumDurationMilliseconds &&
           (payload.requiredDurationMilliseconds == 0U ||
            payload.progressMilliseconds <=
                payload.requiredDurationMilliseconds);
}

[[nodiscard]] bool IsValidRestraintState(
    const RestraintStatePayload& payload) noexcept {
    constexpr auto kAuthoritative =
        static_cast<std::uint8_t>(
            RestraintStateFlag::Authoritative);
    constexpr auto kEngineOwned =
        static_cast<std::uint8_t>(
            RestraintStateFlag::EngineOwned);
    constexpr auto kKnownFlags =
        kAuthoritative | kEngineOwned |
        static_cast<std::uint8_t>(
            RestraintStateFlag::Snapshot);
    const bool free =
        payload.state == PlayerRestraintState::Free;
    return payload.subjectEntityId.IsValid() &&
           payload.revision != 0U &&
           IsKnownRestraintState(
               static_cast<std::uint8_t>(payload.state)) &&
           (payload.flags & ~kKnownFlags) == 0U &&
           (payload.flags & kAuthoritative) != 0U &&
           (free
                ? !payload.ownerEntityId.IsValid() &&
                      (payload.flags & kEngineOwned) == 0U
                : payload.ownerEntityId.IsValid() &&
                      payload.ownerEntityId != payload.subjectEntityId);
}

[[nodiscard]] bool IsValidEquipmentState(
    const EquipmentStatePayload& payload) noexcept {
    constexpr auto kKnownFlags =
        static_cast<std::uint32_t>(
            EquipmentStateFlag::Equipped) |
        static_cast<std::uint32_t>(
            EquipmentStateFlag::Reloading);
    return payload.entityId.IsValid() &&
           (payload.flags & ~kKnownFlags) == 0U;
}

}  // namespace

AnimSceneDefinitionFingerprint ComputeAnimSceneDefinitionFingerprint(
    const AnimSceneDefinitionPayload& payload) {
    if (!IsValidAnimSceneDefinitionShape(payload)) {
        throw std::invalid_argument(
            "invalid canonical AnimSceneDefinition payload");
    }
    const auto fingerprint =
        TryComputeAnimSceneDefinitionFingerprint(payload);
    if (!fingerprint.has_value()) {
        throw std::runtime_error(
            "failed to compute AnimSceneDefinition SHA-256 fingerprint");
    }
    return *fingerprint;
}

bool IsKnownMessageType(const std::uint16_t value) noexcept {
    return value >= static_cast<std::uint16_t>(MessageType::Hello) &&
           value <=
               static_cast<std::uint16_t>(
               MessageType::MissionProgression);
}

std::vector<std::uint8_t> EncodeMissionProgression(
    const MissionProgressionPayload& payload) {
    const auto phase = static_cast<std::uint8_t>(payload.phase);
    constexpr auto allowed = static_cast<std::uint8_t>(
        MissionProgressionFlag::GuestCanStart) |
        static_cast<std::uint8_t>(MissionProgressionFlag::VerifiedCompletionMapping);
    const bool completion =
        payload.phase == MissionProgressionPhase::Completion;
    const bool appliesMapping = (payload.flags & static_cast<std::uint8_t>(
        MissionProgressionFlag::VerifiedCompletionMapping)) != 0U;
    if (payload.missionId == 0U || payload.missionEpoch == 0U ||
        payload.eventId == 0U || phase < static_cast<std::uint8_t>(MissionProgressionPhase::Offer) ||
        phase > static_cast<std::uint8_t>(MissionProgressionPhase::Completion) ||
        (payload.flags & ~allowed) != 0U ||
        (!completion && payload.completionRating != 0U) ||
        (!completion && payload.completionCashAward != 0) ||
        payload.completionCashAward < 0 ||
        (appliesMapping && (!completion || payload.completionRating < 2U ||
                            payload.completionRating > 5U))) {
        throw std::invalid_argument("invalid mission progression payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kMissionProgressionPayloadSize);
    AppendLittleEndian(bytes, payload.missionId);
    AppendLittleEndian(bytes, payload.missionEpoch);
    AppendLittleEndian(bytes, payload.eventId);
    AppendLittleEndian(bytes, payload.phase);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, payload.completionRating);
    AppendLittleEndian(bytes, std::uint8_t{0U});
    AppendLittleEndian(bytes, payload.completionCashAward);
    return bytes;
}

std::optional<MissionProgressionPayload> DecodeMissionProgression(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kMissionProgressionPayloadSize) return std::nullopt;
    std::size_t offset{};
    MissionProgressionPayload payload{ReadLittleEndian<std::uint32_t>(bytes, offset),
        ReadLittleEndian<std::uint32_t>(bytes, offset),
        ReadLittleEndian<std::uint64_t>(bytes, offset),
        static_cast<MissionProgressionPhase>(ReadLittleEndian<std::uint8_t>(bytes, offset)),
        ReadLittleEndian<std::uint8_t>(bytes, offset),
        ReadLittleEndian<std::uint8_t>(bytes, offset)};
    const auto reserved8 = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto completionCashAward = ReadLittleEndian<std::int32_t>(bytes, offset);
    try { (void)EncodeMissionProgression(payload); } catch (...) { return std::nullopt; }
    payload.completionCashAward = completionCashAward;
    return reserved8 == 0U ? std::optional<MissionProgressionPayload>{payload} : std::nullopt;
}

std::vector<std::uint8_t> EncodeCampaignCapability(
    const CampaignCapabilityPayload& payload) {
    const auto kind = static_cast<std::uint8_t>(payload.kind);
    if (kind < static_cast<std::uint8_t>(CampaignCapabilityKind::WeaponShopEligibility) ||
        kind > static_cast<std::uint8_t>(CampaignCapabilityKind::ActivityGate) ||
        payload.recordHash == 0U || payload.hostEventId == 0U ||
        payload.grantedAtUnixMilliseconds <= 0) {
        throw std::invalid_argument("invalid campaign capability payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kCampaignCapabilityPayloadSize);
    AppendLittleEndian(bytes, payload.kind);
    AppendLittleEndian(bytes, std::uint8_t{0U});
    AppendLittleEndian(bytes, std::uint16_t{0U});
    AppendLittleEndian(bytes, payload.recordHash);
    AppendLittleEndian(bytes, payload.hostEventId);
    AppendLittleEndian(bytes, payload.grantedAtUnixMilliseconds);
    return bytes;
}

std::optional<CampaignCapabilityPayload> DecodeCampaignCapability(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kCampaignCapabilityPayloadSize) return std::nullopt;
    std::size_t offset{};
    const auto kind = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reservedByte = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reserved = ReadLittleEndian<std::uint16_t>(bytes, offset);
    CampaignCapabilityPayload payload{static_cast<CampaignCapabilityKind>(kind),
        ReadLittleEndian<std::uint32_t>(bytes, offset),
        ReadLittleEndian<std::uint64_t>(bytes, offset),
        ReadLittleEndian<std::int64_t>(bytes, offset)};
    if (kind < static_cast<std::uint8_t>(CampaignCapabilityKind::WeaponShopEligibility) ||
        kind > static_cast<std::uint8_t>(CampaignCapabilityKind::ActivityGate) ||
        reservedByte != 0U || reserved != 0U || payload.recordHash == 0U ||
        payload.hostEventId == 0U || payload.grantedAtUnixMilliseconds <= 0) return std::nullopt;
    return payload;
}

std::vector<std::uint8_t> EncodeCampaignCapabilityAck(const CampaignCapabilityAckPayload& payload) {
    const auto kind = static_cast<std::uint8_t>(payload.kind);
    if (kind < static_cast<std::uint8_t>(CampaignCapabilityKind::WeaponShopEligibility) || kind > static_cast<std::uint8_t>(CampaignCapabilityKind::ActivityGate) || payload.recordHash == 0U || payload.hostEventId == 0U) throw std::invalid_argument("invalid campaign capability acknowledgement");
    std::vector<std::uint8_t> bytes; bytes.reserve(kCampaignCapabilityAckPayloadSize);
    AppendLittleEndian(bytes, payload.kind); AppendLittleEndian(bytes, std::uint8_t{0U}); AppendLittleEndian(bytes, std::uint16_t{0U}); AppendLittleEndian(bytes, payload.recordHash); AppendLittleEndian(bytes, payload.hostEventId); return bytes;
}

std::optional<CampaignCapabilityAckPayload> DecodeCampaignCapabilityAck(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kCampaignCapabilityAckPayloadSize) return std::nullopt;
    std::size_t offset{}; const auto kind = ReadLittleEndian<std::uint8_t>(bytes, offset); const auto b = ReadLittleEndian<std::uint8_t>(bytes, offset); const auto r = ReadLittleEndian<std::uint16_t>(bytes, offset);
    CampaignCapabilityAckPayload payload{static_cast<CampaignCapabilityKind>(kind), ReadLittleEndian<std::uint32_t>(bytes, offset), ReadLittleEndian<std::uint64_t>(bytes, offset)};
    if (kind < static_cast<std::uint8_t>(CampaignCapabilityKind::WeaponShopEligibility) || kind > static_cast<std::uint8_t>(CampaignCapabilityKind::ActivityGate) || b != 0U || r != 0U || payload.recordHash == 0U || payload.hostEventId == 0U) return std::nullopt; return payload;
}

std::vector<std::uint8_t> EncodePickupCollected(const PickupCollectedPayload& payload) {
    if (!payload.actorEntityId.IsValid() || payload.collectionId == 0U || payload.pickupHash == 0U) throw std::invalid_argument("invalid pickup collection");
    std::vector<std::uint8_t> bytes; bytes.reserve(kPickupCollectedPayloadSize);
    AppendLittleEndian(bytes, payload.actorEntityId.Value()); AppendLittleEndian(bytes, payload.collectionId); AppendLittleEndian(bytes, payload.pickupHash); AppendLittleEndian(bytes, std::uint32_t{0U}); return bytes;
}
std::optional<PickupCollectedPayload> DecodePickupCollected(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kPickupCollectedPayloadSize) return std::nullopt;
    std::size_t offset{}; PickupCollectedPayload payload{NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)}, ReadLittleEndian<std::uint64_t>(bytes, offset), ReadLittleEndian<std::uint32_t>(bytes, offset)}; const auto reserved = ReadLittleEndian<std::uint32_t>(bytes, offset);
    return payload.actorEntityId.IsValid() && payload.collectionId != 0U && payload.pickupHash != 0U && reserved == 0U ? std::optional<PickupCollectedPayload>{payload} : std::nullopt;
}

std::vector<std::uint8_t> FrameCodec::Encode(const Frame& frame) {
    if (frame.payload.size() > kMaximumFramePayload) {
        throw std::length_error("frame payload exceeds the 1 MiB protocol limit");
    }
    if (!IsKnownMessageType(static_cast<std::uint16_t>(frame.header.type))) {
        throw std::invalid_argument("frame has an unknown message type");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kFrameHeaderSize + frame.payload.size());
    AppendLittleEndian(bytes, kFrameMagic);
    AppendLittleEndian(bytes, kProtocolVersion);
    AppendLittleEndian(bytes, frame.header.type);
    AppendLittleEndian(bytes, frame.header.sequence);
    AppendLittleEndian(bytes, frame.header.tick);
    AppendLittleEndian(
        bytes,
        static_cast<std::uint32_t>(frame.payload.size()));
    bytes.insert(bytes.end(), frame.payload.begin(), frame.payload.end());
    return bytes;
}

DecodeResult FrameCodec::DecodeOne(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kFrameHeaderSize) {
        return {DecodeStatus::NeedMoreData, 0U, std::nullopt, {}};
    }

    std::size_t offset{};
    const auto magic = ReadLittleEndian<std::uint32_t>(bytes, offset);
    const auto version = ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto typeValue = ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto sequence = ReadLittleEndian<std::uint32_t>(bytes, offset);
    const auto tick = ReadLittleEndian<std::uint64_t>(bytes, offset);
    const auto payloadLength =
        ReadLittleEndian<std::uint32_t>(bytes, offset);

    if (magic != kFrameMagic) {
        return {
            DecodeStatus::Invalid,
            0U,
            std::nullopt,
            "invalid frame magic (expected ASCII R2CP)"};
    }
    if (version != kProtocolVersion) {
        return {
            DecodeStatus::Invalid,
            0U,
            std::nullopt,
            "unsupported frame protocol version"};
    }
    if (!IsKnownMessageType(typeValue)) {
        return {DecodeStatus::Invalid, 0U, std::nullopt, "unknown message type"};
    }
    if (payloadLength > kMaximumFramePayload) {
        return {
            DecodeStatus::Invalid,
            0U,
            std::nullopt,
            "frame payload exceeds the 1 MiB protocol limit"};
    }

    const auto totalLength =
        kFrameHeaderSize + static_cast<std::size_t>(payloadLength);
    if (bytes.size() < totalLength) {
        return {DecodeStatus::NeedMoreData, 0U, std::nullopt, {}};
    }

    Frame frame;
    frame.header = FrameHeader{
        magic,
        version,
        static_cast<MessageType>(typeValue),
        sequence,
        tick,
        payloadLength};
    frame.payload.assign(
        bytes.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderSize),
        bytes.begin() + static_cast<std::ptrdiff_t>(totalLength));
    return {DecodeStatus::Complete, totalLength, std::move(frame), {}};
}

void FrameStreamDecoder::Append(const std::span<const std::uint8_t> bytes) {
    if (HasError() || bytes.empty()) {
        return;
    }
    // Two complete maximum frames cover coalesced stream writes while still
    // imposing a hard memory bound. NamedPipeClient drains after each 64 KiB
    // read, so its practical high-water mark is one frame plus one read.
    constexpr auto kMaximumBufferedBytes =
        2U *
        (kFrameHeaderSize +
         static_cast<std::size_t>(kMaximumFramePayload));
    if (bytes.size() > kMaximumBufferedBytes ||
        buffer_.size() > kMaximumBufferedBytes - bytes.size()) {
        error_ = "named-pipe receive buffer exceeded the frame limit";
        buffer_.clear();
        return;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

std::optional<Frame> FrameStreamDecoder::Pop() {
    if (HasError()) {
        return std::nullopt;
    }
    auto result =
        FrameCodec::DecodeOne(std::span<const std::uint8_t>{buffer_});
    if (result.status == DecodeStatus::Invalid) {
        error_ = std::move(result.error);
        buffer_.clear();
        return std::nullopt;
    }
    if (result.status == DecodeStatus::NeedMoreData) {
        return std::nullopt;
    }

    auto frame = std::move(result.frame);
    buffer_.erase(
        buffer_.begin(),
        buffer_.begin() + static_cast<std::ptrdiff_t>(result.consumed));
    return frame;
}

void FrameStreamDecoder::Reset() {
    buffer_.clear();
    error_.clear();
}

SequenceDisposition SequenceWindow::Observe(
    const std::uint32_t sequence) noexcept {
    if (!hasLast_) {
        hasLast_ = true;
        last_ = sequence;
        return SequenceDisposition::First;
    }
    if (sequence == last_) {
        return SequenceDisposition::Duplicate;
    }

    const auto delta = static_cast<std::int32_t>(sequence - last_);
    if (delta > 0) {
        last_ = sequence;
        return SequenceDisposition::Newer;
    }
    return SequenceDisposition::Stale;
}

void SequenceWindow::Reset() noexcept {
    hasLast_ = false;
    last_ = 0U;
}

std::uint32_t FrameSequencer::Next() noexcept {
    const auto result = next_;
    ++next_;
    if (next_ == 0U) {
        next_ = 1U;
    }
    return result;
}

std::vector<std::uint8_t> EncodePlayerState(
    const PlayerStatePayload& payload) {
    if (!payload.entityId.IsValid()) {
        throw std::invalid_argument("PlayerState requires a valid NetEntityId");
    }
    const auto hasAimTarget =
        (payload.flags &
         static_cast<std::uint32_t>(
             PlayerStateFlag::AimTargetValid)) != 0U;
    if (!IsFinite(payload.position) || !IsFinite(payload.velocity) ||
        !std::isfinite(payload.heading) ||
        !IsValidHealth(payload.healthFraction) ||
        !IsFinite(payload.aimTarget) ||
        (!hasAimTarget && !IsZero(payload.aimTarget)) ||
        !std::isfinite(payload.movementHeading) ||
        !std::isfinite(payload.localForwardSpeed) ||
        !std::isfinite(payload.localRightSpeed) ||
        std::abs(payload.localForwardSpeed) > 50.0F ||
        std::abs(payload.localRightSpeed) > 50.0F ||
        !std::isfinite(payload.desiredMoveBlend) ||
        payload.desiredMoveBlend < 0.0F ||
        payload.desiredMoveBlend > 3.0F ||
        !IsKnownTraversalKind(
            static_cast<std::uint8_t>(payload.traversalKind)) ||
        !IsKnownLocomotionMode(
            static_cast<std::uint8_t>(payload.locomotionMode)) ||
        !IsFinite(payload.traversalAnchor) ||
        !std::isfinite(payload.traversalHeading) ||
        (payload.traversalKind != PlayerTraversalKind::None &&
         payload.traversalActionId == 0U)) {
        throw std::invalid_argument(
            "PlayerState contains invalid numeric values");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kPlayerStatePayloadSize);
    AppendLittleEndian(bytes, payload.entityId.Value());
    AppendLittleEndian(bytes, payload.slot);
    AppendLittleEndian(bytes, payload.lifecycle);
    AppendLittleEndian(bytes, std::uint16_t{0});
    AppendFloat(bytes, payload.position.x);
    AppendFloat(bytes, payload.position.y);
    AppendFloat(bytes, payload.position.z);
    AppendFloat(bytes, payload.velocity.x);
    AppendFloat(bytes, payload.velocity.y);
    AppendFloat(bytes, payload.velocity.z);
    AppendFloat(bytes, payload.heading);
    AppendFloat(bytes, payload.healthFraction);
    AppendLittleEndian(bytes, payload.flags);
    AppendFloat(bytes, payload.aimTarget.x);
    AppendFloat(bytes, payload.aimTarget.y);
    AppendFloat(bytes, payload.aimTarget.z);
    AppendLittleEndian(bytes, payload.fireSequence);
    AppendFloat(bytes, payload.movementHeading);
    AppendFloat(bytes, payload.localForwardSpeed);
    AppendFloat(bytes, payload.localRightSpeed);
    AppendFloat(bytes, payload.desiredMoveBlend);
    AppendLittleEndian(bytes, payload.locomotionEpoch);
    AppendLittleEndian(bytes, payload.traversalActionId);
    AppendLittleEndian(bytes, payload.traversalKind);
    AppendLittleEndian(bytes, payload.locomotionMode);
    AppendLittleEndian(bytes, std::uint16_t{0});
    AppendFloat(bytes, payload.traversalAnchor.x);
    AppendFloat(bytes, payload.traversalAnchor.y);
    AppendFloat(bytes, payload.traversalAnchor.z);
    AppendFloat(bytes, payload.traversalHeading);
    return bytes;
}

std::optional<PlayerStatePayload> DecodePlayerState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kPlayerStatePayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    PlayerStatePayload payload;
    payload.entityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    const auto slot = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto lifecycle = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reserved = ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.position.x = ReadFloat(bytes, offset);
    payload.position.y = ReadFloat(bytes, offset);
    payload.position.z = ReadFloat(bytes, offset);
    payload.velocity.x = ReadFloat(bytes, offset);
    payload.velocity.y = ReadFloat(bytes, offset);
    payload.velocity.z = ReadFloat(bytes, offset);
    payload.heading = ReadFloat(bytes, offset);
    payload.healthFraction = ReadFloat(bytes, offset);
    payload.flags = ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.aimTarget.x = ReadFloat(bytes, offset);
    payload.aimTarget.y = ReadFloat(bytes, offset);
    payload.aimTarget.z = ReadFloat(bytes, offset);
    payload.fireSequence =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.movementHeading = ReadFloat(bytes, offset);
    payload.localForwardSpeed = ReadFloat(bytes, offset);
    payload.localRightSpeed = ReadFloat(bytes, offset);
    payload.desiredMoveBlend = ReadFloat(bytes, offset);
    payload.locomotionEpoch =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.traversalActionId =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto traversalKind =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto locomotionMode =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto semanticReserved =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.traversalAnchor.x = ReadFloat(bytes, offset);
    payload.traversalAnchor.y = ReadFloat(bytes, offset);
    payload.traversalAnchor.z = ReadFloat(bytes, offset);
    payload.traversalHeading = ReadFloat(bytes, offset);
    const auto hasAimTarget =
        (payload.flags &
         static_cast<std::uint32_t>(
             PlayerStateFlag::AimTargetValid)) != 0U;

    if (!payload.entityId.IsValid() || !IsKnownSlot(slot) ||
        !IsKnownLifecycle(lifecycle) || reserved != 0U ||
        !IsFinite(payload.position) || !IsFinite(payload.velocity) ||
        !std::isfinite(payload.heading) ||
        !IsValidHealth(payload.healthFraction) ||
        !IsFinite(payload.aimTarget) ||
        (!hasAimTarget && !IsZero(payload.aimTarget)) ||
        !std::isfinite(payload.movementHeading) ||
        !std::isfinite(payload.localForwardSpeed) ||
        !std::isfinite(payload.localRightSpeed) ||
        std::abs(payload.localForwardSpeed) > 50.0F ||
        std::abs(payload.localRightSpeed) > 50.0F ||
        !std::isfinite(payload.desiredMoveBlend) ||
        payload.desiredMoveBlend < 0.0F ||
        payload.desiredMoveBlend > 3.0F ||
        !IsKnownTraversalKind(traversalKind) ||
        !IsKnownLocomotionMode(locomotionMode) ||
        semanticReserved != 0U ||
        !IsFinite(payload.traversalAnchor) ||
        !std::isfinite(payload.traversalHeading) ||
        (traversalKind !=
             static_cast<std::uint8_t>(PlayerTraversalKind::None) &&
         payload.traversalActionId == 0U)) {
        return std::nullopt;
    }
    payload.slot = static_cast<PlayerSlot>(slot);
    payload.lifecycle = static_cast<PlayerLifecycle>(lifecycle);
    payload.traversalKind =
        static_cast<PlayerTraversalKind>(traversalKind);
    payload.locomotionMode =
        static_cast<PlayerLocomotionMode>(locomotionMode);
    return payload;
}

std::vector<std::uint8_t> EncodePlayerTraversal(
    const PlayerTraversalPayload& payload) {
    constexpr auto kKnownFlags =
        static_cast<std::uint32_t>(
            PlayerTraversalFlag::InputEdgeDetected) |
        static_cast<std::uint32_t>(
            PlayerTraversalFlag::ObstacleValid) |
        static_cast<std::uint32_t>(
            PlayerTraversalFlag::ExpectedLandingValid);
    const bool obstacleValid =
        (payload.flags & static_cast<std::uint32_t>(
             PlayerTraversalFlag::ObstacleValid)) != 0U;
    const bool landingValid =
        (payload.flags & static_cast<std::uint32_t>(
             PlayerTraversalFlag::ExpectedLandingValid)) != 0U;
    if (!payload.entityId.IsValid() ||
        !IsKnownSlot(static_cast<std::uint8_t>(payload.slot)) ||
        payload.kind == PlayerTraversalKind::None ||
        !IsKnownTraversalKind(static_cast<std::uint8_t>(payload.kind)) ||
        payload.actionId == 0U || payload.revision == 0U ||
        payload.locomotionEpoch == 0U ||
        (payload.flags & ~kKnownFlags) != 0U ||
        !std::isfinite(payload.takeoffHeading) ||
        !IsFinite(payload.takeoffPosition) ||
        !IsFinite(payload.approachVelocity) ||
        !IsFinite(payload.obstaclePoint) ||
        !IsFinite(payload.obstacleNormal) ||
        !std::isfinite(payload.obstacleTopZ) ||
        !IsFinite(payload.expectedLanding) ||
        (!obstacleValid &&
         (!IsZero(payload.obstaclePoint) ||
          !IsZero(payload.obstacleNormal) ||
          payload.obstacleTopZ != 0.0F)) ||
        (!landingValid && !IsZero(payload.expectedLanding))) {
        throw std::invalid_argument(
            "PlayerTraversal contains invalid fields");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kPlayerTraversalPayloadSize);
    AppendLittleEndian(bytes, payload.entityId.Value());
    AppendLittleEndian(bytes, payload.slot);
    AppendLittleEndian(bytes, payload.kind);
    AppendLittleEndian(bytes, payload.actionId);
    AppendLittleEndian(bytes, payload.revision);
    AppendLittleEndian(bytes, payload.locomotionEpoch);
    AppendLittleEndian(bytes, payload.flags);
    AppendFloat(bytes, payload.takeoffHeading);
    AppendFloat(bytes, payload.takeoffPosition.x);
    AppendFloat(bytes, payload.takeoffPosition.y);
    AppendFloat(bytes, payload.takeoffPosition.z);
    AppendFloat(bytes, payload.approachVelocity.x);
    AppendFloat(bytes, payload.approachVelocity.y);
    AppendFloat(bytes, payload.approachVelocity.z);
    AppendFloat(bytes, payload.obstaclePoint.x);
    AppendFloat(bytes, payload.obstaclePoint.y);
    AppendFloat(bytes, payload.obstaclePoint.z);
    AppendFloat(bytes, payload.obstacleNormal.x);
    AppendFloat(bytes, payload.obstacleNormal.y);
    AppendFloat(bytes, payload.obstacleNormal.z);
    AppendFloat(bytes, payload.obstacleTopZ);
    AppendFloat(bytes, payload.expectedLanding.x);
    AppendFloat(bytes, payload.expectedLanding.y);
    AppendFloat(bytes, payload.expectedLanding.z);
    return bytes;
}

std::optional<PlayerTraversalPayload> DecodePlayerTraversal(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kPlayerTraversalPayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    PlayerTraversalPayload payload;
    payload.entityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    const auto slot = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto kind = ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.actionId = ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.revision = ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.locomotionEpoch =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.flags = ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.takeoffHeading = ReadFloat(bytes, offset);
    payload.takeoffPosition = {
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset)};
    payload.approachVelocity = {
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset)};
    payload.obstaclePoint = {
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset)};
    payload.obstacleNormal = {
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset)};
    payload.obstacleTopZ = ReadFloat(bytes, offset);
    payload.expectedLanding = {
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset)};
    payload.slot = static_cast<PlayerSlot>(slot);
    payload.kind = static_cast<PlayerTraversalKind>(kind);
    try {
        // Reuse the encoder as the single strict validation source.
        (void)EncodePlayerTraversal(payload);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    return payload;
}

std::vector<std::uint8_t> EncodePlayerAction(
    const PlayerActionPayload& payload) {
    if (!IsValidPlayerAction(payload)) {
        throw std::invalid_argument(
            "PlayerAction contains invalid fields");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kPlayerActionPayloadSize);
    AppendLittleEndian(bytes, payload.actorEntityId.Value());
    AppendLittleEndian(bytes, payload.targetEntityId.Value());
    AppendLittleEndian(bytes, payload.sequence);
    AppendLittleEndian(bytes, payload.actionId);
    AppendLittleEndian(bytes, payload.revision);
    AppendLittleEndian(bytes, payload.actorSlot);
    AppendLittleEndian(bytes, payload.authoritySlot);
    AppendLittleEndian(bytes, payload.kind);
    AppendLittleEndian(bytes, payload.phase);
    AppendLittleEndian(bytes, std::uint16_t{0U});
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, payload.durationMilliseconds);
    AppendLittleEndian(bytes, payload.phaseElapsedMilliseconds);
    AppendLittleEndian(bytes, payload.weaponHash);
    AppendLittleEndian(bytes, payload.variantHash);
    AppendLittleEndian(bytes, payload.animationSampleSequence);
    AppendFloat(bytes, payload.actorAnchor.x);
    AppendFloat(bytes, payload.actorAnchor.y);
    AppendFloat(bytes, payload.actorAnchor.z);
    AppendFloat(bytes, payload.targetPoint.x);
    AppendFloat(bytes, payload.targetPoint.y);
    AppendFloat(bytes, payload.targetPoint.z);
    AppendFloat(bytes, payload.facingHeading);
    AppendFloat(bytes, payload.normalizedPhase);
    return bytes;
}

std::optional<PlayerActionPayload> DecodePlayerAction(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kPlayerActionPayloadSize) {
        return std::nullopt;
    }

    std::size_t offset{};
    PlayerActionPayload payload;
    payload.actorEntityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.targetEntityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.sequence =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.actionId =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.revision =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto actorSlot =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto authoritySlot =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto kind =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto phase =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.flags =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.durationMilliseconds =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.phaseElapsedMilliseconds =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.weaponHash =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.variantHash =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.animationSampleSequence =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.actorAnchor = {
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset)};
    payload.targetPoint = {
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset),
        ReadFloat(bytes, offset)};
    payload.facingHeading = ReadFloat(bytes, offset);
    payload.normalizedPhase = ReadFloat(bytes, offset);
    if (reserved != 0U ||
        !IsKnownSlot(actorSlot) ||
        !IsKnownSlot(authoritySlot) ||
        !IsKnownPlayerActionKind(kind) ||
        !IsKnownPlayerActionPhase(phase)) {
        return std::nullopt;
    }

    payload.actorSlot = static_cast<PlayerSlot>(actorSlot);
    payload.authoritySlot = static_cast<PlayerSlot>(authoritySlot);
    payload.kind = static_cast<PlayerActionKind>(kind);
    payload.phase = static_cast<PlayerActionPhase>(phase);
    return IsValidPlayerAction(payload)
        ? std::optional<PlayerActionPayload>{payload}
        : std::nullopt;
}

std::vector<std::uint8_t> EncodeInteractionIntent(
    const InteractionIntentPayload& payload) {
    if (!IsValidInteractionIntent(payload)) {
        throw std::invalid_argument(
            "InteractionIntent contains invalid fields");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kInteractionIntentPayloadSize);
    AppendLittleEndian(bytes, payload.actorEntityId.Value());
    AppendLittleEndian(bytes, payload.targetEntityId.Value());
    AppendLittleEndian(bytes, payload.secondaryEntityId.Value());
    AppendLittleEndian(bytes, payload.interactionId);
    AppendLittleEndian(bytes, payload.revision);
    AppendLittleEndian(bytes, payload.actorSlot);
    AppendLittleEndian(bytes, payload.kind);
    AppendLittleEndian(bytes, payload.phase);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, std::uint16_t{0U});
    AppendLittleEndian(bytes, payload.requestedDurationMilliseconds);
    return bytes;
}

std::optional<InteractionIntentPayload> DecodeInteractionIntent(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kInteractionIntentPayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    InteractionIntentPayload payload;
    payload.actorEntityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.targetEntityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.secondaryEntityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.interactionId =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.revision =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto slot = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto kind = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto phase = ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.flags = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.requestedDurationMilliseconds =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    if (reserved != 0U || !IsKnownSlot(slot) ||
        !IsKnownInteractionKind(kind) ||
        !IsKnownInteractionIntentPhase(phase)) {
        return std::nullopt;
    }
    payload.actorSlot = static_cast<PlayerSlot>(slot);
    payload.kind = static_cast<InteractionKind>(kind);
    payload.phase = static_cast<InteractionIntentPhase>(phase);
    return IsValidInteractionIntent(payload)
        ? std::optional<InteractionIntentPayload>{payload}
        : std::nullopt;
}

std::vector<std::uint8_t> EncodeInteractionResult(
    const InteractionResultPayload& payload) {
    if (!IsValidInteractionResult(payload)) {
        throw std::invalid_argument(
            "InteractionResult contains invalid fields");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kInteractionResultPayloadSize);
    AppendLittleEndian(bytes, payload.actorEntityId.Value());
    AppendLittleEndian(bytes, payload.targetEntityId.Value());
    AppendLittleEndian(bytes, payload.secondaryEntityId.Value());
    AppendLittleEndian(bytes, payload.interactionId);
    AppendLittleEndian(bytes, payload.revision);
    AppendLittleEndian(bytes, payload.kind);
    AppendLittleEndian(bytes, payload.status);
    AppendLittleEndian(bytes, payload.rejectReason);
    AppendLittleEndian(bytes, std::uint8_t{0U});
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, payload.progressMilliseconds);
    AppendLittleEndian(bytes, payload.requiredDurationMilliseconds);
    return bytes;
}

std::optional<InteractionResultPayload> DecodeInteractionResult(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kInteractionResultPayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    InteractionResultPayload payload;
    payload.actorEntityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.targetEntityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.secondaryEntityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.interactionId =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.revision =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto kind = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto status = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reason = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reserved = ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.flags = ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.progressMilliseconds =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.requiredDurationMilliseconds =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    if (reserved != 0U || !IsKnownInteractionKind(kind) ||
        !IsKnownInteractionResultStatus(status) ||
        !IsKnownInteractionRejectReason(reason)) {
        return std::nullopt;
    }
    payload.kind = static_cast<InteractionKind>(kind);
    payload.status = static_cast<InteractionResultStatus>(status);
    payload.rejectReason = static_cast<InteractionRejectReason>(reason);
    return IsValidInteractionResult(payload)
        ? std::optional<InteractionResultPayload>{payload}
        : std::nullopt;
}

std::vector<std::uint8_t> EncodeRestraintState(
    const RestraintStatePayload& payload) {
    if (!IsValidRestraintState(payload)) {
        throw std::invalid_argument(
            "RestraintState contains invalid fields");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kRestraintStatePayloadSize);
    AppendLittleEndian(bytes, payload.subjectEntityId.Value());
    AppendLittleEndian(bytes, payload.ownerEntityId.Value());
    AppendLittleEndian(bytes, payload.sourceInteractionId);
    AppendLittleEndian(bytes, payload.revision);
    AppendLittleEndian(bytes, payload.state);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, std::uint16_t{0U});
    return bytes;
}

std::optional<RestraintStatePayload> DecodeRestraintState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kRestraintStatePayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    RestraintStatePayload payload;
    payload.subjectEntityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.ownerEntityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.sourceInteractionId =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.revision =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    const auto state = ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.flags = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    if (reserved != 0U || !IsKnownRestraintState(state)) {
        return std::nullopt;
    }
    payload.state = static_cast<PlayerRestraintState>(state);
    return IsValidRestraintState(payload)
        ? std::optional<RestraintStatePayload>{payload}
        : std::nullopt;
}

std::vector<std::uint8_t> EncodePlayerIdentity(
    const PlayerIdentityPayload& payload) {
    if (!payload.entityId.IsValid() ||
        !IsKnownSlot(static_cast<std::uint8_t>(payload.slot)) ||
        !IsValidPlayerNickname(payload.nickname)) {
        throw std::invalid_argument("invalid PlayerIdentity payload");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kPlayerIdentityHeaderSize + payload.nickname.size());
    AppendLittleEndian(bytes, payload.entityId.Value());
    AppendLittleEndian(bytes, payload.slot);
    AppendLittleEndian(
        bytes,
        static_cast<std::uint8_t>(payload.nickname.size()));
    bytes.insert(
        bytes.end(),
        payload.nickname.begin(),
        payload.nickname.end());
    return bytes;
}

std::optional<PlayerIdentityPayload> DecodePlayerIdentity(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kPlayerIdentityHeaderSize ||
        bytes.size() >
            kPlayerIdentityHeaderSize +
                kMaximumPlayerNicknameUtf8Bytes) {
        return std::nullopt;
    }

    std::size_t offset{};
    PlayerIdentityPayload payload;
    payload.entityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    const auto slot = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto nicknameLength =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    if (!payload.entityId.IsValid() ||
        !IsKnownSlot(slot) ||
        nicknameLength == 0U ||
        bytes.size() !=
            kPlayerIdentityHeaderSize +
                static_cast<std::size_t>(nicknameLength)) {
        return std::nullopt;
    }

    payload.slot = static_cast<PlayerSlot>(slot);
    payload.nickname.assign(
        reinterpret_cast<const char*>(bytes.data() + offset),
        nicknameLength);
    if (!IsValidPlayerNickname(payload.nickname)) {
        return std::nullopt;
    }
    return payload;
}

std::vector<std::uint8_t> EncodePlayerAppearanceState(
    const PlayerAppearanceStatePayload& payload) {
    if (!IsValidPlayerAppearanceState(payload)) {
        throw std::invalid_argument(
            "invalid PlayerAppearanceState payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(
        kPlayerAppearanceStateHeaderSize +
        payload.componentHashes.size() * sizeof(std::uint32_t));
    AppendLittleEndian(bytes, payload.entityId.Value());
    AppendLittleEndian(bytes, payload.slot);
    AppendLittleEndian(bytes, payload.schemaVersion);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, payload.revision);
    AppendLittleEndian(bytes, payload.modelHash);
    AppendLittleEndian(
        bytes,
        static_cast<std::uint16_t>(payload.componentHashes.size()));
    AppendLittleEndian(bytes, std::uint16_t{0U});
    AppendLittleEndian(bytes, payload.fingerprint);
    for (const auto componentHash : payload.componentHashes) {
        AppendLittleEndian(bytes, componentHash);
    }
    return bytes;
}

std::optional<PlayerAppearanceStatePayload> DecodePlayerAppearanceState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kPlayerAppearanceStateHeaderSize ||
        bytes.size() >
            kPlayerAppearanceStateHeaderSize +
                kMaximumPlayerAppearanceComponents *
                    sizeof(std::uint32_t)) {
        return std::nullopt;
    }
    std::size_t offset{};
    PlayerAppearanceStatePayload payload;
    payload.entityId = NetEntityId{
        ReadLittleEndian<std::uint64_t>(bytes, offset)};
    const auto slot = ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.schemaVersion =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.flags = ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.revision = ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.modelHash = ReadLittleEndian<std::uint32_t>(bytes, offset);
    const auto componentCount =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.fingerprint =
        ReadLittleEndian<std::uint64_t>(bytes, offset);
    if (!IsKnownSlot(slot) || reserved != 0U ||
        componentCount == 0U ||
        componentCount > kMaximumPlayerAppearanceComponents ||
        bytes.size() !=
            kPlayerAppearanceStateHeaderSize +
                static_cast<std::size_t>(componentCount) *
                    sizeof(std::uint32_t)) {
        return std::nullopt;
    }
    payload.slot = static_cast<PlayerSlot>(slot);
    payload.componentHashes.reserve(componentCount);
    for (std::uint16_t index = 0U; index < componentCount; ++index) {
        payload.componentHashes.push_back(
            ReadLittleEndian<std::uint32_t>(bytes, offset));
    }
    return IsValidPlayerAppearanceState(payload)
               ? std::optional{std::move(payload)}
               : std::nullopt;
}

std::vector<std::uint8_t> EncodeWorldEntityState(
    const WorldEntityStatePayload& payload) {
    if (!IsValidWorldEntityState(payload)) {
        throw std::invalid_argument(
            "invalid WorldEntityState payload");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kWorldEntityStatePayloadSize);
    AppendLittleEndian(bytes, payload.entityId.Value());
    AppendLittleEndian(bytes, payload.modelHash);
    AppendLittleEndian(bytes, payload.kind);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, payload.combatTargetSlot);
    AppendLittleEndian(bytes, payload.taskKind);
    AppendFloat(bytes, payload.position.x);
    AppendFloat(bytes, payload.position.y);
    AppendFloat(bytes, payload.position.z);
    AppendFloat(bytes, payload.velocity.x);
    AppendFloat(bytes, payload.velocity.y);
    AppendFloat(bytes, payload.velocity.z);
    AppendFloat(bytes, payload.heading);
    AppendFloat(bytes, payload.healthFraction);
    AppendLittleEndian(bytes, payload.weaponHash);
    AppendLittleEndian(bytes, payload.parentEntityId.Value());
    AppendFloat(bytes, payload.taskTarget.x);
    AppendFloat(bytes, payload.taskTarget.y);
    AppendFloat(bytes, payload.taskTarget.z);
    AppendLittleEndian(bytes, std::uint32_t{0U});
    return bytes;
}

std::optional<WorldEntityStatePayload> DecodeWorldEntityState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kWorldEntityStatePayloadSize) {
        return std::nullopt;
    }

    std::size_t offset{};
    WorldEntityStatePayload payload;
    payload.entityId =
        NetEntityId{
            ReadLittleEndian<std::uint64_t>(
                bytes,
                offset)};
    payload.modelHash =
        ReadLittleEndian<std::uint32_t>(
            bytes,
            offset);
    payload.kind =
        static_cast<WorldEntityKind>(
            ReadLittleEndian<std::uint8_t>(
                bytes,
                offset));
    payload.flags =
        ReadLittleEndian<std::uint8_t>(
            bytes,
            offset);
    payload.combatTargetSlot =
        static_cast<WorldCombatTargetSlot>(
            ReadLittleEndian<std::uint8_t>(
                bytes,
                offset));
    payload.taskKind =
        static_cast<WorldTaskKind>(
            ReadLittleEndian<std::uint8_t>(
                bytes,
                offset));
    payload.position.x = ReadFloat(bytes, offset);
    payload.position.y = ReadFloat(bytes, offset);
    payload.position.z = ReadFloat(bytes, offset);
    payload.velocity.x = ReadFloat(bytes, offset);
    payload.velocity.y = ReadFloat(bytes, offset);
    payload.velocity.z = ReadFloat(bytes, offset);
    payload.heading = ReadFloat(bytes, offset);
    payload.healthFraction = ReadFloat(bytes, offset);
    payload.weaponHash =
        ReadLittleEndian<std::uint32_t>(
            bytes,
            offset);
    payload.parentEntityId =
        NetEntityId{
            ReadLittleEndian<std::uint64_t>(
                bytes,
                offset)};
    payload.taskTarget.x = ReadFloat(bytes, offset);
    payload.taskTarget.y = ReadFloat(bytes, offset);
    payload.taskTarget.z = ReadFloat(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint32_t>(
            bytes,
            offset);
    return reserved == 0U &&
                   IsValidWorldEntityState(payload)
               ? std::optional{payload}
               : std::nullopt;
}

std::vector<std::uint8_t> EncodePlayerMountState(
    const PlayerMountStatePayload& payload) {
    if (!IsValidPlayerMountState(payload)) {
        throw std::invalid_argument(
            "invalid PlayerMountState payload");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kPlayerMountStatePayloadSize);
    AppendLittleEndian(bytes, payload.playerEntityId.Value());
    AppendLittleEndian(bytes, payload.mountEntityId.Value());
    AppendLittleEndian(bytes, payload.slot);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, std::uint16_t{0U});
    AppendLittleEndian(bytes, payload.modelHash);
    AppendFloat(bytes, payload.position.x);
    AppendFloat(bytes, payload.position.y);
    AppendFloat(bytes, payload.position.z);
    AppendFloat(bytes, payload.velocity.x);
    AppendFloat(bytes, payload.velocity.y);
    AppendFloat(bytes, payload.velocity.z);
    AppendFloat(bytes, payload.heading);
    AppendFloat(bytes, payload.healthFraction);
    AppendLittleEndian(bytes, payload.generation);
    return bytes;
}

std::optional<PlayerMountStatePayload> DecodePlayerMountState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kPlayerMountStatePayloadSize) {
        return std::nullopt;
    }

    std::size_t offset{};
    PlayerMountStatePayload payload;
    payload.playerEntityId = NetEntityId{
        ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.mountEntityId = NetEntityId{
        ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.slot = static_cast<PlayerSlot>(
        ReadLittleEndian<std::uint8_t>(bytes, offset));
    payload.flags =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.modelHash =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.position.x = ReadFloat(bytes, offset);
    payload.position.y = ReadFloat(bytes, offset);
    payload.position.z = ReadFloat(bytes, offset);
    payload.velocity.x = ReadFloat(bytes, offset);
    payload.velocity.y = ReadFloat(bytes, offset);
    payload.velocity.z = ReadFloat(bytes, offset);
    payload.heading = ReadFloat(bytes, offset);
    payload.healthFraction = ReadFloat(bytes, offset);
    payload.generation =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    return reserved == 0U &&
                   IsValidPlayerMountState(payload)
               ? std::optional{payload}
               : std::nullopt;
}

std::vector<std::uint8_t> EncodeEntityDespawn(
    const EntityDespawnPayload& payload) {
    if (!payload.entityId.IsValid()) {
        throw std::invalid_argument(
            "invalid EntityDespawn payload");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kEntityDespawnPayloadSize);
    AppendLittleEndian(bytes, payload.entityId.Value());
    return bytes;
}

std::optional<EntityDespawnPayload> DecodeEntityDespawn(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kEntityDespawnPayloadSize) {
        return std::nullopt;
    }

    std::size_t offset{};
    EntityDespawnPayload payload{
        NetEntityId{
            ReadLittleEndian<std::uint64_t>(
                bytes,
                offset)}};
    return payload.entityId.IsValid()
               ? std::optional{payload}
               : std::nullopt;
}

std::vector<std::uint8_t> EncodeDamageIntent(
    const DamageIntentPayload& payload) {
    if (!payload.attackerId.IsValid() ||
        !payload.targetId.IsValid() ||
        payload.weaponHash == 0U ||
        !std::isfinite(payload.damage) ||
        payload.damage <= 0.0F ||
        payload.damage > 100.0F ||
        payload.shotSequence == 0U) {
        throw std::invalid_argument(
            "invalid DamageIntent payload");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kDamageIntentPayloadSize);
    AppendLittleEndian(bytes, payload.attackerId.Value());
    AppendLittleEndian(bytes, payload.targetId.Value());
    AppendLittleEndian(bytes, payload.weaponHash);
    AppendFloat(bytes, payload.damage);
    AppendLittleEndian(bytes, payload.shotSequence);
    AppendLittleEndian(bytes, std::uint32_t{0U});
    return bytes;
}

std::optional<DamageIntentPayload> DecodeDamageIntent(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kDamageIntentPayloadSize) {
        return std::nullopt;
    }

    std::size_t offset{};
    DamageIntentPayload payload;
    payload.attackerId =
        NetEntityId{
            ReadLittleEndian<std::uint64_t>(
                bytes,
                offset)};
    payload.targetId =
        NetEntityId{
            ReadLittleEndian<std::uint64_t>(
                bytes,
                offset)};
    payload.weaponHash =
        ReadLittleEndian<std::uint32_t>(
            bytes,
            offset);
    payload.damage = ReadFloat(bytes, offset);
    payload.shotSequence =
        ReadLittleEndian<std::uint32_t>(
            bytes,
            offset);
    const auto reserved =
        ReadLittleEndian<std::uint32_t>(
            bytes,
            offset);
    if (reserved != 0U ||
        !payload.attackerId.IsValid() ||
        !payload.targetId.IsValid() ||
        payload.weaponHash == 0U ||
        !std::isfinite(payload.damage) ||
        payload.damage <= 0.0F ||
        payload.damage > 100.0F ||
        payload.shotSequence == 0U) {
        return std::nullopt;
    }
    return payload;
}

std::vector<std::uint8_t> EncodeWorldState(
    const WorldStatePayload& payload) {
    if (!IsValidWorldState(payload)) {
        throw std::invalid_argument("invalid WorldState payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kWorldStatePayloadSize);
    AppendLittleEndian(bytes, payload.hour);
    AppendLittleEndian(bytes, payload.minute);
    AppendLittleEndian(bytes, payload.second);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, payload.weatherFrom);
    AppendLittleEndian(bytes, payload.weatherTo);
    AppendFloat(bytes, payload.weatherBlend);
    AppendLittleEndian(bytes, payload.day);
    AppendLittleEndian(bytes, payload.month);
    AppendLittleEndian(bytes, payload.year);
    AppendLittleEndian(bytes, std::uint32_t{0U});
    return bytes;
}

std::optional<WorldStatePayload> DecodeWorldState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kWorldStatePayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    WorldStatePayload payload;
    payload.hour =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.minute =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.second =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.flags =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.weatherFrom =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.weatherTo =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.weatherBlend = ReadFloat(bytes, offset);
    payload.day =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.month =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.year =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    return reserved == 0U &&
                   IsValidWorldState(payload)
               ? std::optional{payload}
               : std::nullopt;
}

std::vector<std::uint8_t> EncodeMissionState(
    const MissionStatePayload& payload) {
    if (!IsValidMissionState(payload)) {
        throw std::invalid_argument(
            "invalid MissionState payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kMissionStatePayloadSize);
    AppendLittleEndian(bytes, payload.hostEntityId.Value());
    AppendLittleEndian(bytes, payload.missionEpoch);
    AppendLittleEndian(bytes, payload.revision);
    AppendLittleEndian(bytes, payload.checkpointGeneration);
    AppendLittleEndian(bytes, payload.phase);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, std::uint16_t{0U});
    AppendFloat(bytes, payload.hostAnchor.x);
    AppendFloat(bytes, payload.hostAnchor.y);
    AppendFloat(bytes, payload.hostAnchor.z);
    AppendFloat(bytes, payload.hostHeading);
    AppendLittleEndian(bytes, std::uint64_t{0U});
    return bytes;
}

std::optional<MissionStatePayload> DecodeMissionState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kMissionStatePayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    MissionStatePayload payload;
    payload.hostEntityId =
        NetEntityId{
            ReadLittleEndian<std::uint64_t>(
                bytes,
                offset)};
    payload.missionEpoch =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.revision =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.checkpointGeneration =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.phase = static_cast<MissionPhase>(
        ReadLittleEndian<std::uint8_t>(bytes, offset));
    payload.flags =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.hostAnchor.x = ReadFloat(bytes, offset);
    payload.hostAnchor.y = ReadFloat(bytes, offset);
    payload.hostAnchor.z = ReadFloat(bytes, offset);
    payload.hostHeading = ReadFloat(bytes, offset);
    const auto trailingReserved =
        ReadLittleEndian<std::uint64_t>(bytes, offset);
    return reserved == 0U &&
                   trailingReserved == 0U &&
                   IsValidMissionState(payload)
               ? std::optional{payload}
               : std::nullopt;
}

std::vector<std::uint8_t> EncodeMissionCinematicState(
    const MissionCinematicStatePayload& payload) {
    if (!IsValidMissionCinematicState(payload)) {
        throw std::invalid_argument(
            "invalid MissionCinematicState payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kMissionCinematicStatePayloadSize);
    AppendLittleEndian(bytes, payload.hostEntityId.Value());
    AppendLittleEndian(bytes, payload.missionEpoch);
    AppendLittleEndian(bytes, payload.cinematicGeneration);
    AppendLittleEndian(bytes, payload.revision);
    AppendLittleEndian(bytes, payload.checkpointGeneration);
    AppendLittleEndian(bytes, payload.phase);
    AppendLittleEndian(bytes, std::uint8_t{0U});
    AppendLittleEndian(bytes, payload.flags);
    AppendFloat(bytes, payload.resumeAnchor.x);
    AppendFloat(bytes, payload.resumeAnchor.y);
    AppendFloat(bytes, payload.resumeAnchor.z);
    AppendFloat(bytes, payload.resumeHeading);
    AppendLittleEndian(bytes, std::uint32_t{0U});
    return bytes;
}

std::optional<MissionCinematicStatePayload> DecodeMissionCinematicState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kMissionCinematicStatePayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    MissionCinematicStatePayload payload;
    payload.hostEntityId = NetEntityId{
        ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.missionEpoch =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.cinematicGeneration =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.revision =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.checkpointGeneration =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.phase = static_cast<MissionCinematicPhase>(
        ReadLittleEndian<std::uint8_t>(bytes, offset));
    const auto reserved =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.flags =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.resumeAnchor.x = ReadFloat(bytes, offset);
    payload.resumeAnchor.y = ReadFloat(bytes, offset);
    payload.resumeAnchor.z = ReadFloat(bytes, offset);
    payload.resumeHeading = ReadFloat(bytes, offset);
    const auto trailingReserved =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    return reserved == 0U &&
                   trailingReserved == 0U &&
                   IsValidMissionCinematicState(payload)
               ? std::optional{payload}
               : std::nullopt;
}

std::vector<std::uint8_t> EncodeMissionCinematicAction(
    const MissionCinematicActionPayload& payload) {
    if (!IsValidMissionCinematicAction(payload)) {
        throw std::invalid_argument(
            "invalid MissionCinematicAction payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kMissionCinematicActionPayloadSize);
    AppendLittleEndian(bytes, payload.hostEntityId.Value());
    AppendLittleEndian(bytes, payload.missionEpoch);
    AppendLittleEndian(bytes, payload.cinematicGeneration);
    AppendLittleEndian(bytes, payload.actionId);
    AppendLittleEndian(bytes, payload.kind);
    AppendLittleEndian(bytes, payload.senderSlot);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, std::uint64_t{0U});
    return bytes;
}

std::optional<MissionCinematicActionPayload> DecodeMissionCinematicAction(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kMissionCinematicActionPayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    MissionCinematicActionPayload payload;
    payload.hostEntityId = NetEntityId{
        ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.missionEpoch =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.cinematicGeneration =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.actionId =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.kind = static_cast<MissionCinematicActionKind>(
        ReadLittleEndian<std::uint8_t>(bytes, offset));
    payload.senderSlot =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.flags =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint64_t>(bytes, offset);
    return reserved == 0U && IsValidMissionCinematicAction(payload)
               ? std::optional{payload}
               : std::nullopt;
}

std::vector<std::uint8_t> EncodeAnimSceneReplicaState(
    const AnimSceneReplicaStatePayload& payload) {
    if (!IsValidAnimSceneReplicaState(payload)) {
        throw std::invalid_argument(
            "invalid AnimSceneReplicaState payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kAnimSceneReplicaStatePayloadSize);
    AppendLittleEndian(bytes, payload.hostEntityId.Value());
    AppendLittleEndian(bytes, payload.missionEpoch);
    AppendLittleEndian(bytes, payload.cinematicGeneration);
    AppendLittleEndian(bytes, payload.definitionRevision);
    AppendLittleEndian(bytes, payload.revision);
    AppendLittleEndian(bytes, payload.dictionaryHash);
    AppendLittleEndian(bytes, payload.flags);
    AppendFloat(bytes, payload.phase);
    AppendFloat(bytes, payload.durationSeconds);
    AppendFloat(bytes, payload.rate);
    AppendFloat(bytes, payload.originPosition.x);
    AppendFloat(bytes, payload.originPosition.y);
    AppendFloat(bytes, payload.originPosition.z);
    AppendFloat(bytes, payload.originRotation.x);
    AppendFloat(bytes, payload.originRotation.y);
    AppendFloat(bytes, payload.originRotation.z);
    AppendLittleEndian(bytes, payload.activeCameraCount);
    AppendLittleEndian(bytes, std::uint16_t{0U});
    return bytes;
}

std::optional<AnimSceneReplicaStatePayload> DecodeAnimSceneReplicaState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kAnimSceneReplicaStatePayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    AnimSceneReplicaStatePayload payload;
    payload.hostEntityId = NetEntityId{
        ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.missionEpoch =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.cinematicGeneration =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.definitionRevision =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.revision =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.dictionaryHash =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.flags =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.phase = ReadFloat(bytes, offset);
    payload.durationSeconds = ReadFloat(bytes, offset);
    payload.rate = ReadFloat(bytes, offset);
    payload.originPosition.x = ReadFloat(bytes, offset);
    payload.originPosition.y = ReadFloat(bytes, offset);
    payload.originPosition.z = ReadFloat(bytes, offset);
    payload.originRotation.x = ReadFloat(bytes, offset);
    payload.originRotation.y = ReadFloat(bytes, offset);
    payload.originRotation.z = ReadFloat(bytes, offset);
    payload.activeCameraCount =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    return reserved == 0U && IsValidAnimSceneReplicaState(payload)
               ? std::optional{payload}
               : std::nullopt;
}

std::vector<std::uint8_t> EncodeAnimSceneDefinition(
    const AnimSceneDefinitionPayload& payload) {
    if (!IsValidAnimSceneDefinition(payload)) {
        throw std::invalid_argument(
            "invalid or non-canonical AnimSceneDefinition payload");
    }
    return SerializeAnimSceneDefinition(payload, false);
}

std::optional<AnimSceneDefinitionPayload> DecodeAnimSceneDefinition(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kAnimSceneDefinitionHeaderSize ||
        bytes.size() > kMaximumAnimSceneDefinitionPayloadSize) {
        return std::nullopt;
    }

    std::size_t offset{};
    AnimSceneDefinitionPayload payload;
    payload.hostEntityId = NetEntityId{
        ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.missionEpoch =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.cinematicGeneration =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.definitionRevision =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.dictionaryHash =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.fingerprintLow =
        ReadLittleEndian<std::uint64_t>(bytes, offset);
    payload.fingerprintHigh =
        ReadLittleEndian<std::uint64_t>(bytes, offset);
    payload.durationSeconds = ReadFloat(bytes, offset);
    const auto resourceLength =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto playbackLength =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto roleCount =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.sceneFlags =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.createOptionFlags =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto createReserved0 =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto createReserved1 =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto createReserved2 =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    if (reserved != 0U || resourceLength == 0U ||
        createReserved0 != 0U || createReserved1 != 0U ||
        createReserved2 != 0U ||
        resourceLength > kMaximumAnimSceneResourceBytes ||
        playbackLength > kMaximumAnimScenePlaybackListBytes ||
        roleCount > kMaximumAnimSceneDefinitionRoles ||
        bytes.size() - offset <
            static_cast<std::size_t>(resourceLength) + playbackLength) {
        return std::nullopt;
    }

    payload.resourceName.assign(
        reinterpret_cast<const char*>(bytes.data() + offset),
        resourceLength);
    offset += resourceLength;
    payload.playbackList.assign(
        reinterpret_cast<const char*>(bytes.data() + offset),
        playbackLength);
    offset += playbackLength;
    payload.roles.reserve(roleCount);
    for (std::uint16_t index = 0U; index < roleCount; ++index) {
        if (bytes.size() - offset < kAnimSceneRoleBindingHeaderSize) {
            return std::nullopt;
        }
        AnimSceneRoleBindingPayload role;
        role.entityId = NetEntityId{
            ReadLittleEndian<std::uint64_t>(bytes, offset)};
        role.modelHash =
            ReadLittleEndian<std::uint32_t>(bytes, offset);
        role.bindingFlags =
            ReadLittleEndian<std::uint32_t>(bytes, offset);
        role.flags =
            ReadLittleEndian<std::uint16_t>(bytes, offset);
        role.kind = static_cast<AnimSceneRoleKind>(
            ReadLittleEndian<std::uint8_t>(bytes, offset));
        const auto roleNameLength =
            ReadLittleEndian<std::uint8_t>(bytes, offset);
        if (roleNameLength == 0U ||
            roleNameLength > kMaximumAnimSceneRoleNameBytes ||
            bytes.size() - offset < roleNameLength) {
            return std::nullopt;
        }
        role.roleName.assign(
            reinterpret_cast<const char*>(bytes.data() + offset),
            roleNameLength);
        offset += roleNameLength;
        payload.roles.push_back(std::move(role));
    }
    if (offset != bytes.size() ||
        !IsValidAnimSceneDefinition(payload)) {
        return std::nullopt;
    }
    return payload;
}

std::vector<std::uint8_t> EncodeAnimSceneControl(
    const AnimSceneControlPayload& payload) {
    if (!IsValidAnimSceneControl(payload)) {
        throw std::invalid_argument(
            "invalid AnimSceneControl payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kAnimSceneControlPayloadSize);
    AppendLittleEndian(bytes, payload.hostEntityId.Value());
    AppendLittleEndian(bytes, payload.missionEpoch);
    AppendLittleEndian(bytes, payload.cinematicGeneration);
    AppendLittleEndian(bytes, payload.definitionRevision);
    AppendLittleEndian(bytes, payload.actionId);
    AppendLittleEndian(bytes, payload.fingerprintLow);
    AppendLittleEndian(bytes, payload.fingerprintHigh);
    AppendLittleEndian(bytes, payload.playAtHostTick);
    AppendFloat(bytes, payload.startPhase);
    AppendFloat(bytes, payload.rate);
    AppendLittleEndian(bytes, payload.kind);
    AppendLittleEndian(bytes, payload.senderSlot);
    AppendLittleEndian(bytes, payload.reason);
    AppendLittleEndian(bytes, std::uint8_t{});
    AppendLittleEndian(bytes, payload.flags);
    return bytes;
}

std::optional<AnimSceneControlPayload> DecodeAnimSceneControl(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kAnimSceneControlPayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    AnimSceneControlPayload payload;
    payload.hostEntityId = NetEntityId{
        ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.missionEpoch =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.cinematicGeneration =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.definitionRevision =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.actionId =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.fingerprintLow =
        ReadLittleEndian<std::uint64_t>(bytes, offset);
    payload.fingerprintHigh =
        ReadLittleEndian<std::uint64_t>(bytes, offset);
    payload.playAtHostTick =
        ReadLittleEndian<std::uint64_t>(bytes, offset);
    payload.startPhase = ReadFloat(bytes, offset);
    payload.rate = ReadFloat(bytes, offset);
    payload.kind = static_cast<AnimSceneControlKind>(
        ReadLittleEndian<std::uint8_t>(bytes, offset));
    payload.senderSlot =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.reason = static_cast<AnimSceneControlReason>(
        ReadLittleEndian<std::uint8_t>(bytes, offset));
    const auto reserved =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    payload.flags =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    return reserved == 0U && IsValidAnimSceneControl(payload)
               ? std::optional{payload}
               : std::nullopt;
}

std::vector<std::uint8_t> EncodeMissionCameraState(
    const MissionCameraStatePayload& payload) {
    if (!IsValidMissionCameraState(payload)) {
        throw std::invalid_argument(
            "invalid MissionCameraState payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kMissionCameraStatePayloadSize);
    AppendLittleEndian(bytes, payload.hostEntityId.Value());
    AppendLittleEndian(bytes, payload.missionEpoch);
    AppendLittleEndian(bytes, payload.cinematicGeneration);
    AppendLittleEndian(bytes, payload.revision);
    AppendLittleEndian(bytes, payload.flags);
    AppendFloat(bytes, payload.position.x);
    AppendFloat(bytes, payload.position.y);
    AppendFloat(bytes, payload.position.z);
    AppendFloat(bytes, payload.rotation.x);
    AppendFloat(bytes, payload.rotation.y);
    AppendFloat(bytes, payload.rotation.z);
    AppendFloat(bytes, payload.fieldOfView);
    AppendLittleEndian(bytes, std::uint32_t{0U});
    return bytes;
}

std::optional<MissionCameraStatePayload> DecodeMissionCameraState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kMissionCameraStatePayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    MissionCameraStatePayload payload;
    payload.hostEntityId =
        NetEntityId{
            ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.missionEpoch =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.cinematicGeneration =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.revision =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.flags =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.position.x = ReadFloat(bytes, offset);
    payload.position.y = ReadFloat(bytes, offset);
    payload.position.z = ReadFloat(bytes, offset);
    payload.rotation.x = ReadFloat(bytes, offset);
    payload.rotation.y = ReadFloat(bytes, offset);
    payload.rotation.z = ReadFloat(bytes, offset);
    payload.fieldOfView = ReadFloat(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    return reserved == 0U && IsValidMissionCameraState(payload)
               ? std::optional{payload}
               : std::nullopt;
}

std::vector<std::uint8_t> EncodeEquipmentState(
    const EquipmentStatePayload& payload) {
    if (!IsValidEquipmentState(payload)) {
        throw std::invalid_argument(
            "invalid EquipmentState payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kEquipmentStatePayloadSize);
    AppendLittleEndian(bytes, payload.entityId.Value());
    AppendLittleEndian(bytes, payload.weaponHash);
    AppendLittleEndian(bytes, payload.ammo);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, std::uint32_t{0U});
    return bytes;
}

std::optional<EquipmentStatePayload> DecodeEquipmentState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kEquipmentStatePayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    EquipmentStatePayload payload;
    payload.entityId =
        NetEntityId{
            ReadLittleEndian<std::uint64_t>(
                bytes,
                offset)};
    payload.weaponHash =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.ammo =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    payload.flags =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    return reserved == 0U &&
                   IsValidEquipmentState(payload)
               ? std::optional{payload}
               : std::nullopt;
}

std::vector<std::uint8_t> EncodeCommand(const CommandPayload& payload) {
    if (!IsKnownCommandOpcode(
            static_cast<std::uint16_t>(payload.opcode))) {
        throw std::invalid_argument("Command has an unknown opcode");
    }
    if (!IsFinite(payload.position) || !std::isfinite(payload.heading) ||
        !std::isfinite(payload.value)) {
        throw std::invalid_argument("Command contains invalid numeric values");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kCommandPayloadSize);
    AppendLittleEndian(bytes, payload.opcode);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, payload.target.Value());
    AppendFloat(bytes, payload.position.x);
    AppendFloat(bytes, payload.position.y);
    AppendFloat(bytes, payload.position.z);
    AppendFloat(bytes, payload.heading);
    AppendFloat(bytes, payload.value);
    return bytes;
}

std::optional<CommandPayload> DecodeCommand(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kCommandPayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    const auto opcode = ReadLittleEndian<std::uint16_t>(bytes, offset);
    if (!IsKnownCommandOpcode(opcode)) {
        return std::nullopt;
    }
    CommandPayload payload;
    payload.opcode = static_cast<CommandOpcode>(opcode);
    payload.flags = ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.target =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    payload.position.x = ReadFloat(bytes, offset);
    payload.position.y = ReadFloat(bytes, offset);
    payload.position.z = ReadFloat(bytes, offset);
    payload.heading = ReadFloat(bytes, offset);
    payload.value = ReadFloat(bytes, offset);
    if (!IsFinite(payload.position) || !std::isfinite(payload.heading) ||
        !std::isfinite(payload.value)) {
        return std::nullopt;
    }
    return payload;
}

std::vector<std::uint8_t> EncodePauseVote(
    const PauseVotePayload& payload) {
    constexpr auto kKnownFlags =
        static_cast<std::uint8_t>(PauseVoteFlag::HostVoted) |
        static_cast<std::uint8_t>(PauseVoteFlag::GuestVoted) |
        static_cast<std::uint8_t>(PauseVoteFlag::Paused);
    const auto kind = static_cast<std::uint8_t>(payload.kind);
    const auto slot = static_cast<std::uint8_t>(payload.voterSlot);
    if ((kind != static_cast<std::uint8_t>(
                     PauseVoteKind::RequestToggle) &&
         kind != static_cast<std::uint8_t>(
                     PauseVoteKind::AuthoritativeState)) ||
        !IsKnownSlot(slot) ||
        (payload.flags & ~kKnownFlags) != 0U ||
        (payload.kind == PauseVoteKind::RequestToggle &&
         payload.flags != 0U)) {
        throw std::invalid_argument("invalid pause-vote payload");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kPauseVotePayloadSize);
    AppendLittleEndian(bytes, payload.kind);
    AppendLittleEndian(bytes, payload.voterSlot);
    AppendLittleEndian(bytes, payload.flags);
    AppendLittleEndian(bytes, std::uint8_t{0U});
    AppendLittleEndian(bytes, payload.generation);
    AppendLittleEndian(bytes, std::uint32_t{0U});
    return bytes;
}

std::optional<PauseVotePayload> DecodePauseVote(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kPauseVotePayloadSize) {
        return std::nullopt;
    }

    std::size_t offset{};
    const auto kind =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto slot =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto flags =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reservedByte =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto generation =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    const auto reserved =
        ReadLittleEndian<std::uint32_t>(bytes, offset);
    constexpr auto kKnownFlags =
        static_cast<std::uint8_t>(PauseVoteFlag::HostVoted) |
        static_cast<std::uint8_t>(PauseVoteFlag::GuestVoted) |
        static_cast<std::uint8_t>(PauseVoteFlag::Paused);
    if ((kind != static_cast<std::uint8_t>(
                     PauseVoteKind::RequestToggle) &&
         kind != static_cast<std::uint8_t>(
                     PauseVoteKind::AuthoritativeState)) ||
        !IsKnownSlot(slot) ||
        (flags & ~kKnownFlags) != 0U ||
        (kind == static_cast<std::uint8_t>(
                     PauseVoteKind::RequestToggle) &&
         flags != 0U) ||
        reservedByte != 0U ||
        reserved != 0U) {
        return std::nullopt;
    }

    return PauseVotePayload{
        static_cast<PauseVoteKind>(kind),
        static_cast<PlayerSlot>(slot),
        flags,
        generation};
}

std::vector<std::uint8_t> EncodeDownedState(
    const DownedStatePayload& payload) {
    if (!payload.entityId.IsValid() ||
        !IsKnownLifecycle(
            static_cast<std::uint8_t>(payload.lifecycle)) ||
        !IsValidHealth(payload.healthFraction)) {
        throw std::invalid_argument("invalid DownedState payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kDownedStatePayloadSize);
    AppendLittleEndian(bytes, payload.entityId.Value());
    AppendLittleEndian(bytes, payload.lifecycle);
    AppendLittleEndian(bytes, std::uint8_t{0});
    AppendLittleEndian(bytes, std::uint16_t{0});
    AppendFloat(bytes, payload.healthFraction);
    return bytes;
}

std::optional<DownedStatePayload> DecodeDownedState(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kDownedStatePayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    DownedStatePayload payload;
    payload.entityId =
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)};
    const auto lifecycle = ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reservedByte =
        ReadLittleEndian<std::uint8_t>(bytes, offset);
    const auto reservedWord =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    payload.healthFraction = ReadFloat(bytes, offset);
    if (!payload.entityId.IsValid() || !IsKnownLifecycle(lifecycle) ||
        reservedByte != 0U || reservedWord != 0U ||
        !IsValidHealth(payload.healthFraction)) {
        return std::nullopt;
    }
    payload.lifecycle = static_cast<PlayerLifecycle>(lifecycle);
    return payload;
}

std::vector<std::uint8_t> EncodeReviveRequest(
    const ReviveRequestPayload& payload) {
    if (!payload.reviverId.IsValid() || !payload.targetId.IsValid() ||
        payload.reviverId == payload.targetId) {
        throw std::invalid_argument("invalid ReviveRequest payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kReviveRequestPayloadSize);
    AppendLittleEndian(bytes, payload.reviverId.Value());
    AppendLittleEndian(bytes, payload.targetId.Value());
    return bytes;
}

std::optional<ReviveRequestPayload> DecodeReviveRequest(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kReviveRequestPayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    ReviveRequestPayload payload{
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)},
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)}};
    if (!payload.reviverId.IsValid() || !payload.targetId.IsValid() ||
        payload.reviverId == payload.targetId) {
        return std::nullopt;
    }
    return payload;
}

std::vector<std::uint8_t> EncodeReviveComplete(
    const ReviveCompletePayload& payload) {
    if (!payload.reviverId.IsValid() || !payload.targetId.IsValid() ||
        payload.reviverId == payload.targetId ||
        !IsValidHealth(payload.healthFraction)) {
        throw std::invalid_argument("invalid ReviveComplete payload");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kReviveCompletePayloadSize);
    AppendLittleEndian(bytes, payload.reviverId.Value());
    AppendLittleEndian(bytes, payload.targetId.Value());
    AppendFloat(bytes, payload.healthFraction);
    return bytes;
}

std::optional<ReviveCompletePayload> DecodeReviveComplete(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kReviveCompletePayloadSize) {
        return std::nullopt;
    }
    std::size_t offset{};
    ReviveCompletePayload payload{
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)},
        NetEntityId{ReadLittleEndian<std::uint64_t>(bytes, offset)},
        ReadFloat(bytes, offset)};
    if (!payload.reviverId.IsValid() || !payload.targetId.IsValid() ||
        payload.reviverId == payload.targetId ||
        !IsValidHealth(payload.healthFraction)) {
        return std::nullopt;
    }
    return payload;
}

std::vector<std::uint8_t> EncodeSessionMenuRequest(
    const SessionMenuAction action,
    const std::string_view inviteCode) {
    if (action != SessionMenuAction::Host &&
        action != SessionMenuAction::JoinFromClipboard &&
        action != SessionMenuAction::ToggleSoloTest &&
        action != SessionMenuAction::ToggleGhostRecord &&
        action != SessionMenuAction::ToggleGhostReplay &&
        action != SessionMenuAction::StopSession) {
        throw std::invalid_argument("unknown session menu action");
    }
    if (inviteCode.size() > kMaximumSessionInviteCodeBytes) {
        throw std::length_error("session invite code is too large");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(1U + inviteCode.size());
    bytes.push_back(static_cast<std::uint8_t>(action));
    bytes.insert(bytes.end(), inviteCode.begin(), inviteCode.end());
    return bytes;
}

std::optional<SessionMenuStatusPayload> DecodeSessionMenuStatus(
    const std::span<const std::uint8_t> bytes) {
    constexpr std::size_t kHeaderSize = 5U;
    if (bytes.size() < kHeaderSize ||
        bytes.front() >
            static_cast<std::uint8_t>(
                SessionMenuStatusKind::Error)) {
        return std::nullopt;
    }

    std::size_t offset = 1U;
    const auto messageLength =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    const auto inviteLength =
        ReadLittleEndian<std::uint16_t>(bytes, offset);
    if (messageLength > kMaximumSessionStatusMessageBytes ||
        inviteLength > kMaximumSessionInviteCodeBytes ||
        bytes.size() !=
            kHeaderSize +
                static_cast<std::size_t>(messageLength) +
                static_cast<std::size_t>(inviteLength)) {
        return std::nullopt;
    }

    SessionMenuStatusPayload result;
    result.kind =
        static_cast<SessionMenuStatusKind>(bytes.front());
    result.message.assign(
        reinterpret_cast<const char*>(bytes.data() + kHeaderSize),
        messageLength);
    result.inviteCode.assign(
        reinterpret_cast<const char*>(
            bytes.data() + kHeaderSize + messageLength),
        inviteLength);
    if (result.message.find('\0') != std::string::npos ||
        result.inviteCode.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    return result;
}

}  // namespace coopstory::bridge
