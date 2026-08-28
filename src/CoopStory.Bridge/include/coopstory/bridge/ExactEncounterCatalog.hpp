#pragma once

#include "coopstory/bridge/AmbientEncounterCoordinator.hpp"
#include "coopstory/bridge/CampaignMissionCatalog.hpp"

#include <cstdint>

namespace coopstory::bridge {

// Exact-event contracts are deliberately separate from ambient filename
// discovery. They define the small, explicit presentation/reward surface that
// a bridge-owned adaptation is permitted to expose.
enum class EncounterPeerRole : std::uint8_t {
    Participant = 1,
    Companion = 2,
};

struct ExactEncounterDefinition final {
    std::uint32_t scriptId{};
    const char* scriptName{};
    AmbientEncounterProfile profile{AmbientEncounterProfile::HostageRescue};
    bool companionMayLootGeneric{};
    // A bridge scene may never infer or clone a special pickup from a local
    // Rockstar event. Set this only after an independently reviewed mapping.
    bool sceneMayContainUniqueLoot{};
    bool synchronizesHonor{};
};

// The Valentine O'Driscoll extortion beat. Decompiled script evidence pins the
// exact script identity; its generic hostile scene can be represented with the
// bridge-owned hostage-rescue profile. A companion may fight and loot generic
// supplies. The bridge scene never contains unique event rewards or Honor
// changes, even for a participant.
inline constexpr ExactEncounterDefinition kExtortionEncounter{
    CampaignMissionId("beat_odriscoll_town_encounter"),
    "beat_odriscoll_town_encounter",
    AmbientEncounterProfile::HostageRescue,
    true,
    false,
    false};

// This is an authenticated wire sentinel, not a Rockstar script identifier
// and never a reward key. It lets a guest explicitly answer a host's exact
// Extortion preflight when that guest's private save does not offer the beat.
inline constexpr std::uint32_t kExtortionCompanionPreflightEvidence =
    0x4350'4D50U;

struct ExactEncounterPreflight final {
    std::uint32_t observedScriptId{};
    bool locallyEligible{};
};

struct ExactEncounterPeerPolicy final {
    EncounterPeerRole role{EncounterPeerRole::Companion};
    bool mayFight{true};
    bool mayLootGeneric{};
    bool mayReceiveUniqueLoot{};
    bool mayReceiveHonor{};
};

[[nodiscard]] constexpr bool IsExactEncounterEligible(
    const ExactEncounterDefinition& definition,
    const ExactEncounterPreflight& preflight) noexcept {
    return definition.scriptId != 0U &&
        preflight.observedScriptId == definition.scriptId &&
        preflight.locallyEligible;
}

[[nodiscard]] constexpr ExactEncounterPeerPolicy ResolveExactEncounterPeerPolicy(
    const ExactEncounterDefinition& definition,
    const ExactEncounterPreflight& preflight) noexcept {
    const bool participant = IsExactEncounterEligible(definition, preflight);
    return ExactEncounterPeerPolicy{
        participant ? EncounterPeerRole::Participant : EncounterPeerRole::Companion,
        true,
        participant || definition.companionMayLootGeneric,
        participant && definition.sceneMayContainUniqueLoot,
        participant && definition.synchronizesHonor};
}

}  // namespace coopstory::bridge
