#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace coopstory::bridge {

// Stable RAGE/joaat script hash. Mission names are internal script names, not
// localized UI text or a chapter number.
[[nodiscard]] constexpr std::uint32_t CampaignMissionId(
    const std::string_view value) noexcept {
    std::uint32_t hash{};
    for (const auto character : value) {
        const auto lower = character >= 'A' && character <= 'Z'
            ? static_cast<std::uint8_t>(character + ('a' - 'A'))
            : static_cast<std::uint8_t>(character);
        hash += lower;
        hash += hash << 10U;
        hash ^= hash >> 6U;
    }
    hash += hash << 3U;
    hash ^= hash >> 11U;
    hash += hash << 15U;
    return hash;
}

enum class CampaignCompletionBinding : std::uint8_t {
    None = 0,
    // MissionData completion and the exact host rating, followed by the
    // vanilla mission-log refresh that exposes every chapter, activity, shop,
    // recipe and encounter gate derived from that MissionData record. The
    // generic progression pipeline may then apply only this mission's
    // explicit, idempotent direct reward records. Cash is deliberately
    // excluded: a wallet delta is not a mission reward receipt.
    MissionDataRatingAndDerivedUnlocks = 1,
};

// Mission scripts award concrete records through distinct native systems. The
// catalog keeps those records attached to their exact mission instead of
// treating an arbitrary host inventory change as a campaign reward.
enum class CampaignMissionRewardBinding : std::uint8_t {
    WeaponOwnership = 1,
    UnlockVisible = 2,
    // A concrete Story inventory record such as a document/map or a
    // pamphlet. Pamphlets are recipe ownership records in RDR2, so this is
    // one half of a verified recipe award.
    InventoryItem = 3,
    // A recipe's UNLOCK entitlement is independent from the physical
    // pamphlet/document item. A mission that permanently awards a recipe must
    // carry both records when its script proves that it grants both.
    RecipeUnlock = 4,
    // Some Story rewards unlock an entitlement rather than merely making a
    // menu/activity visible. This deliberately sets and verifies both bits;
    // use UnlockVisible above for scripts that only expose a progression gate.
    UnlockEntitlement = 5,
    // A weapon can become purchasable without being granted. The facade
    // resolves the weapon's own unlock record through the public unlock
    // resolver, then marks that entitlement visible and unlocked.
    WeaponShopEligibility = 6,
};

struct CampaignMissionReward final {
    CampaignMissionRewardBinding binding{};
    std::uint32_t recordHash{};
    std::uint32_t amount{};
};

struct CampaignMissionDefinition final {
    std::uint32_t missionId{};
    // MissionData's canonical four-character ID. This is the stable ID sent
    // over the progression protocol and supplied to the MissionData natives.
    std::string_view scriptName{};
    // The actual RAGE script registered in init_all_sp. It is deliberately
    // separate: many Story missions use different runtime and MissionData
    // names (for example hunting1 / HNT1).
    std::string_view runtimeScriptName{};
    std::string_view displayName{};
    CampaignCompletionBinding completionBinding{
        CampaignCompletionBinding::None};
    std::span<const CampaignMissionReward> rewards{};
};

// Dialogue is always played by each machine's own Story script.  The network
// only conveys a cue after a locally-created scripted-conversation root has
// been observed.  A profile exists for every catalogued Story mission so the
// protocol can reject cross-mission cues, even where no source-reviewed root
// has been admitted yet.  Roots are deliberately a small reviewed allow-list:
// ScriptHook exposes no safe "enumerate active conversations" native.
[[nodiscard]] constexpr std::uint32_t CampaignMissionDialogueProfileId(
    const std::uint32_t missionId) noexcept {
    return missionId;
}

struct CampaignMissionDialogueRoot final {
    std::uint32_t missionId{};
    std::string_view root{};
    std::uint8_t guestAudioRoles{};
};

enum class CampaignMissionDialogueRole : std::uint8_t {
    Arthur = 1U << 0U,
    Hosea = 1U << 1U,
    Dutch = 1U << 2U,
};

[[nodiscard]] constexpr std::uint8_t operator|(
    const CampaignMissionDialogueRole left,
    const CampaignMissionDialogueRole right) noexcept {
    return static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right);
}

inline constexpr std::uint8_t kCampaignDialogueArthurHosea =
    static_cast<std::uint8_t>(CampaignMissionDialogueRole::Arthur) |
    static_cast<std::uint8_t>(CampaignMissionDialogueRole::Hosea);
inline constexpr std::uint8_t kCampaignDialogueArthurDutchHosea =
    static_cast<std::uint8_t>(CampaignMissionDialogueRole::Arthur) |
    static_cast<std::uint8_t>(CampaignMissionDialogueRole::Dutch) |
    static_cast<std::uint8_t>(CampaignMissionDialogueRole::Hosea);

inline constexpr std::uint32_t kFud1MissionId = CampaignMissionId("FUD1");
inline constexpr std::uint32_t kHunt1MissionId = CampaignMissionId("HNT1");

// Source evidence: hunting1 makes SP_CHAL_HUNT_ROOT visible when the
// legendary-animal map route becomes available. This application is
// idempotent locally. A fishing-rod check in feud1 is a precondition, not an
// award, so it intentionally has no reward record here.
inline constexpr CampaignMissionReward kHunt1MissionRewards[]{
    // hunting1's mission-ending function grants this exact map through the
    // standard inventory grant helper before showing the reward treatment.
    {CampaignMissionRewardBinding::InventoryItem,
     CampaignMissionId("DOCUMENT_MAP_LEGENDARY_ANIMALS"), 1U},
    {CampaignMissionRewardBinding::UnlockVisible,
     CampaignMissionId("SP_CHAL_HUNT_ROOT"), 0U},
};
// winter4 (WNT4 / Old Friends) awards the Carbine Repeater and Lasso. The
// former is on the script's player weapon grant path; the latter is the
// mission's permanent Story reward. Utility weapons are handled specially by
// the facade because ScriptHook's IS_WEAPON_VALID does not reliably classify
// the lasso as a conventional weapon in the prologue.
inline constexpr CampaignMissionReward kWnt4MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponOwnership,
     CampaignMissionId("WEAPON_REPEATER_CARBINE"), 99U},
    {CampaignMissionRewardBinding::WeaponOwnership,
     CampaignMissionId("WEAPON_LASSO"), 1U},
};
// feud1 (FUD1 / The New South) gives Arthur WEAPON_FISHINGROD when it is
// absent before the fishing objective. Unlike RABI1, feud1 has no matching
// fishing-rod removal path, so it is a permanent prerequisite/reward.
inline constexpr CampaignMissionReward kFud1MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponOwnership,
     CampaignMissionId("WEAPON_FISHINGROD"), 1U},
};
// sadie3 (SAD3 / Uncle's Bad Day) gives Arthur a Carcano with 50 rounds when
// absent for the sniper sequence and contains no Carcano-removal path.
inline constexpr CampaignMissionReward kSad3MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponOwnership,
     CampaignMissionId("WEAPON_SNIPERRIFLE_CARCANO"), 50U},
};
// marston8 (MAR8 / American Venom) has a persistent fallback grant for the
// Binoculars; the mission only invokes it if the player does not own them.
inline constexpr CampaignMissionReward kMar8MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponOwnership,
     CampaignMissionId("WEAPON_KIT_BINOCULARS"), 0U},
};
// abigail2_1 (AB21 / The Tool Box) grants Sadie's telegram at mission start
// only when the document is absent; it is a persistent Story document.
inline constexpr CampaignMissionReward kAb21MissionRewards[]{
    {CampaignMissionRewardBinding::InventoryItem,
     CampaignMissionId("DOCUMENT_LETTER_SADIE_TELEGRAM"), 1U},
};
// Mission reward pages are cross-checked against the canonical runtime IDs
// below. These are durable weapon awards, not optional battlefield pickups:
// SEN1 / The First Shall Be the Last awards the Tomahawk; DST1 / Paying a
// Social Call awards the Double-Barreled Shotgun and Throwing Knife. Their
// Cash is not inferred from wallet movement; no authoritative mission-owned
// cash receipt is available through the current public-native surface.
inline constexpr CampaignMissionReward kSen1MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponOwnership,
     CampaignMissionId("WEAPON_THROWN_TOMAHAWK"), 1U},
};
inline constexpr CampaignMissionReward kDst1MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponOwnership,
     CampaignMissionId("WEAPON_SHOTGUN_DOUBLEBARREL"), 12U},
    {CampaignMissionRewardBinding::WeaponOwnership,
     CampaignMissionId("WEAPON_THROWN_THROWING_KNIVES"), 1U},
};
inline constexpr CampaignMissionReward kInd3MissionRewards[]{
    // IND3 / A Fine Night of Debauchery makes these weapons purchasable; it
    // does not grant a free Semi-Auto Shotgun. The Reutlinger watch is an
    // explicit inventory reward (not a document) and industry3's mission
    // path grants this exact provision record when it is absent.
    {CampaignMissionRewardBinding::WeaponShopEligibility,
     CampaignMissionId("WEAPON_SHOTGUN_SEMIAUTO"), 0U},
    {CampaignMissionRewardBinding::WeaponShopEligibility,
     CampaignMissionId("WEAPON_REVOLVER_DOUBLEACTION_GAMBLER"), 0U},
    {CampaignMissionRewardBinding::InventoryItem,
     CampaignMissionId("PROVISION_POCKET_WATCH_REUTLINGE"), 1U},
};
// MUD4 / The Sheep and the Goats makes the standard Rolling Block Rifle part
// of Story progression.
inline constexpr CampaignMissionReward kMud4MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponOwnership,
     CampaignMissionId("WEAPON_SNIPERRIFLE_ROLLINGBLOCK"), 100U},
};
// TRE1 / Magicians for Sport uses RDR2's distinct rare/exotic Rolling Block
// record. Do not collapse it to the ordinary MUD4 weapon: that would lose the
// mission's unique variant for a guest who follows the same Story path.
inline constexpr CampaignMissionReward kTre1MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponOwnership,
     CampaignMissionId("WEAPON_SNIPERRIFLE_ROLLINGBLOCK_EXOTIC"), 100U},
};
// MUD1 / Americans at Rest does not award a Tomahawk; it makes the weapon
// purchasable at gunsmiths. Model that as an entitlement, not ownership.
inline constexpr CampaignMissionReward kMud1MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponShopEligibility,
     CampaignMissionId("WEAPON_THROWN_TOMAHAWK"), 0U},
};
// GRY1 / American Distillation makes the Evans Repeater purchasable at
// gunsmiths; it is an entitlement, never a free weapon grant.
inline constexpr CampaignMissionReward kGry1MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponShopEligibility,
     CampaignMissionId("WEAPON_REPEATER_EVANS"), 0U},
};
// UTP2 / An American Pastoral Scene grants the Lancaster Repeater directly.
// MUD6 / Pouring Forth Oil makes the Pump-Action Shotgun purchasable; GNG3 /
// Visiting Hours does the same for the Repeating Shotgun. These are distinct
// entitlement cases and must not be represented as free weapon grants.
inline constexpr CampaignMissionReward kUtp2MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponOwnership,
     CampaignMissionId("WEAPON_REPEATER_LANCASTER"), 60U},
};
inline constexpr CampaignMissionReward kMud6MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponShopEligibility,
     CampaignMissionId("WEAPON_SHOTGUN_PUMP"), 0U},
};
inline constexpr CampaignMissionReward kGng3MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponShopEligibility,
     CampaignMissionId("WEAPON_SHOTGUN_REPEATING"), 0U},
};
// DST5 / Goodbye, Dear Friend places a permanent Carcano pickup for Arthur
// and makes the Litchfield Repeater purchasable. Mary's keepsakes are omitted
// until their exact persistent inventory IDs (rather than mission prop IDs)
// are verified.
inline constexpr CampaignMissionReward kDst5MissionRewards[]{
    {CampaignMissionRewardBinding::WeaponOwnership,
     CampaignMissionId("WEAPON_SNIPERRIFLE_CARCANO"), 60U},
    {CampaignMissionRewardBinding::WeaponShopEligibility,
     CampaignMissionId("WEAPON_REPEATER_LITCHFIELD"), 0U},
};

// Source-reviewed conversation roots.  These were traced to direct scripted
// conversation helper calls in the corresponding Story scripts.  More roots
// may be admitted only after the exact game build has been reviewed and a
// two-PC capture confirms that they have the same local cast and line order.
inline constexpr std::array<CampaignMissionDialogueRoot, 8U>
    kCampaignMissionDialogueRoots{{
        {kHunt1MissionId, "RH1_TRACK_CHAT", kCampaignDialogueArthurHosea},
        {kHunt1MissionId, "RH1_TRK_FND1", kCampaignDialogueArthurHosea},
        {kHunt1MissionId, "RH1_TRK_FND2", kCampaignDialogueArthurHosea},
        {kHunt1MissionId, "RH1_TRK_FND3", kCampaignDialogueArthurHosea},
        {kFud1MissionId, "FUD1_FISHTALK1", kCampaignDialogueArthurDutchHosea},
        {kFud1MissionId, "FUD1_FISHTALK2", kCampaignDialogueArthurDutchHosea},
        {kFud1MissionId, "FUD1_FISHTALK3", kCampaignDialogueArthurDutchHosea},
        {kFud1MissionId, "FUD1_FISHTALK4", kCampaignDialogueArthurDutchHosea},
    }};

// Canonical Story registry transcribed from init_all_sp's func_282 entries.
// Unknown/non-registry scripts remain companion-only.
#define COOPSTORY_STORY_MISSION(id, runtime, title) \
    {CampaignMissionId(id), id, runtime, title, CampaignCompletionBinding::MissionDataRatingAndDerivedUnlocks}
inline constexpr CampaignMissionDefinition kCampaignMissionCatalog[]{
    COOPSTORY_STORY_MISSION("WNT1", "winter1", "Outlaws from the West"),
    COOPSTORY_STORY_MISSION("WNT2", "winter2", "Enter, Pursued by a Memory"),
    {CampaignMissionId("WNT4"), "WNT4", "winter4", "Old Friends", CampaignCompletionBinding::MissionDataNormalComplete, kWnt4MissionRewards},
    {CampaignMissionId("MUD1"), "MUD1", "mudtown1", "Americans at Rest", CampaignCompletionBinding::MissionDataNormalComplete, kMud1MissionRewards},
    COOPSTORY_STORY_MISSION("MUD2", "mudtown2", "Who Is Not without Sin"),
    COOPSTORY_STORY_MISSION("MUD3", "mudtown3", "Polite Society, Valentine Style"),
    {CampaignMissionId("MUD4"), "MUD4", "mudtown4", "The Sheep and the Goats", CampaignCompletionBinding::MissionDataNormalComplete, kMud4MissionRewards},
    COOPSTORY_STORY_MISSION("MUD5", "mudtown5", "Sodom? Back to Gomorrah"),
    COOPSTORY_STORY_MISSION("MRY1", "mary1", "We Loved Once and True I"),
    COOPSTORY_STORY_MISSION("MRY3", "mary3", "We Loved Once and True III"),
    COOPSTORY_STORY_MISSION("SAL1", "saloon1", "A Quiet Time"),
    COOPSTORY_STORY_MISSION("UTP1", "utopia1", "Blessed Are the Meek?"),
    {CampaignMissionId("UTP2"), "UTP2", "utopia2", "American Pastoral Scene", CampaignCompletionBinding::MissionDataNormalComplete, kUtp2MissionRewards},
    {CampaignMissionId("SEN1"), "SEN1", "sean1", "The First Shall Be Last", CampaignCompletionBinding::MissionDataNormalComplete, kSen1MissionRewards},
    {CampaignMissionId("MUD6"), "MUD6", "mudtown3b", "Pouring Forth Oil", CampaignCompletionBinding::MissionDataNormalComplete, kMud6MissionRewards},
    COOPSTORY_STORY_MISSION("BOU1", "bounty1", "Good, Honest, Snake Oil"),
    COOPSTORY_STORY_MISSION("RABI1", "rcm_abigail11", "A Fisher of Men"),
    COOPSTORY_STORY_MISSION("REV1", "reverend1", "Who Is Not without Sin"),
    {kHunt1MissionId, "HNT1", "hunting1", "Exit Pursued by a Bruised Ego", CampaignCompletionBinding::MissionDataNormalComplete, kHunt1MissionRewards},
    {kFud1MissionId, "FUD1", "feud1", "The New South", CampaignCompletionBinding::MissionDataNormalComplete, kFud1MissionRewards},
    {CampaignMissionId("GRY1"), "GRY1", "grays1", "American Distillation", CampaignCompletionBinding::MissionDataNormalComplete, kGry1MissionRewards},
    COOPSTORY_STORY_MISSION("GRY2", "grays2", "Horse Flesh for Dinner"),
    COOPSTORY_STORY_MISSION("GRY3", "grays3", "The Course of True Love"),
    COOPSTORY_STORY_MISSION("BRT1", "braithwaites1", "Advertising, the New American Art"),
    COOPSTORY_STORY_MISSION("BRT2", "braithwaites2", "The Fine Joys of Tobacco"),
    COOPSTORY_STORY_MISSION("BRT3", "braithwaites3", "Blood Feuds, Ancient and Modern"),
    {CampaignMissionId("TRE1"), "TRE1", "trelawny1", "Magicians for Sport", CampaignCompletionBinding::MissionDataNormalComplete, kTre1MissionRewards},
    COOPSTORY_STORY_MISSION("MOB1", "mob1", "The Joys of Civilization"),
    COOPSTORY_STORY_MISSION("MOB2", "mob2", "Angelo Bronte, a Man of Honor"),
    COOPSTORY_STORY_MISSION("MOB3", "mob3", "Urban Pleasures"),
    COOPSTORY_STORY_MISSION("MOB4", "mob4", "Country Pursuits"),
    COOPSTORY_STORY_MISSION("MOB5", "mob5", "Revenge Is a Dish Best Eaten"),
    {CampaignMissionId("DST1"), "DST1", "odriscolls1", "Paying a Social Call", CampaignCompletionBinding::MissionDataNormalComplete, kDst1MissionRewards},
    COOPSTORY_STORY_MISSION("DST3", "odriscolls3", "Blessed Are the Peacemakers"),
    COOPSTORY_STORY_MISSION("ODR4", "odriscolls4", "Horsemen, Apocalypses"),
    {CampaignMissionId("DST5"), "DST5", "odriscolls5", "Goodbye, Dear Friend", CampaignCompletionBinding::MissionDataNormalComplete, kDst5MissionRewards},
    COOPSTORY_STORY_MISSION("IND1", "industry1", "The Gilded Cage"),
    {CampaignMissionId("IND3"), "IND3", "industry3", "A Fine Night of Debauchery", CampaignCompletionBinding::MissionDataNormalComplete, kInd3MissionRewards},
    COOPSTORY_STORY_MISSION("NBD1", "saint_denis1", "A Fine Night of Debauchery"),
    COOPSTORY_STORY_MISSION("SUS1", "susan1", "No, No and Thrice, No"),
    COOPSTORY_STORY_MISSION("GUA1", "guama1", "Welcome to the New World"),
    COOPSTORY_STORY_MISSION("GUA2", "guama2", "A Kind and Benevolent Despot"),
    COOPSTORY_STORY_MISSION("GUA3", "guama3", "Savagery Unleashed"),
    COOPSTORY_STORY_MISSION("FUS1", "fussar1", "A Kind and Benevolent Despot"),
    COOPSTORY_STORY_MISSION("FUS2", "fussar2", "Savagery Unleashed"),
    COOPSTORY_STORY_MISSION("SMG2", "smuggler2", "The Fine Art of Conversation"),
    COOPSTORY_STORY_MISSION("GNG1", "gang1", "Fleeting Joy"),
    COOPSTORY_STORY_MISSION("GNG2", "gang2", "Icarus and Friends"),
    {CampaignMissionId("GNG3"), "GNG3", "gang3", "Visiting Hours", CampaignCompletionBinding::MissionDataNormalComplete, kGng3MissionRewards},
    COOPSTORY_STORY_MISSION("CRN1", "cornwall1", "An Honest Mistake"),
    COOPSTORY_STORY_MISSION("TRN1", "train_robbery1", "Just a Social Call"),
    COOPSTORY_STORY_MISSION("TRN2", "train_robbery2", "The Delights of Van Horn"),
    COOPSTORY_STORY_MISSION("TRN3", "train_robbery3", "The Bridge to Nowhere"),
    COOPSTORY_STORY_MISSION("TRN4", "train_robbery4", "Our Best Selves"),
    COOPSTORY_STORY_MISSION("NTV1", "native1", "American Fathers"),
    COOPSTORY_STORY_MISSION("NTV2", "native2", "Archaeology for Beginners"),
    COOPSTORY_STORY_MISSION("NTV3", "native3", "My Last Boy"),
    COOPSTORY_STORY_MISSION("NTS1", "native_son1", "A Rage Unleashed"),
    COOPSTORY_STORY_MISSION("NTS2", "native_son2", "The Wisdom of the Elders"),
    COOPSTORY_STORY_MISSION("NTS3", "native_son3", "The Course of True Love"),
    COOPSTORY_STORY_MISSION("FIN1", "finale1", "Our Best Selves"),
    COOPSTORY_STORY_MISSION("FIN2", "finale2", "Red Dead Redemption"),
    COOPSTORY_STORY_MISSION("FIN3", "finale3", "Red Dead Redemption"),
    COOPSTORY_STORY_MISSION("MAR1", "marston1", "The Wheel"),
    COOPSTORY_STORY_MISSION("MAR2", "marston2", "Simple Pleasures"),
    COOPSTORY_STORY_MISSION("MAR4", "marston4", "Jim Milton Rides, Again?"),
    COOPSTORY_STORY_MISSION("MAR5", "marston5_1", "Fatherhood, for Beginners"),
    COOPSTORY_STORY_MISSION("MR52", "marston5_2", "Fatherhood, for Beginners"),
    COOPSTORY_STORY_MISSION("MR53", "marston5_3", "Fatherhood, for Beginners"),
    COOPSTORY_STORY_MISSION("LAR1", "laramie1", "Motherhood"),
    COOPSTORY_STORY_MISSION("MAR6", "marston6", "Home Improvement for Beginners"),
    COOPSTORY_STORY_MISSION("MAR7", "marston7", "An Honest Day's Labors"),
    {CampaignMissionId("MAR8"), "MAR8", "marston8", "American Venom", CampaignCompletionBinding::MissionDataNormalComplete, kMar8MissionRewards},
    COOPSTORY_STORY_MISSION("BE22", "beechers2_2", "A New Jerusalem"),
    {CampaignMissionId("AB21"), "AB21", "abigail2_1", "The Tool Box", CampaignCompletionBinding::MissionDataNormalComplete, kAb21MissionRewards},
    COOPSTORY_STORY_MISSION("SAD2", "sadie2", "A Quick Favor for an Old Friend"),
    {CampaignMissionId("SAD3"), "SAD3", "sadie3", "Uncle's Bad Day", CampaignCompletionBinding::MissionDataNormalComplete, kSad3MissionRewards},
    COOPSTORY_STORY_MISSION("SAD4", "sadie4", "A Really Big Bastard"),
    COOPSTORY_STORY_MISSION("SAD5", "sadie5", "A New Future Imagined"),
    COOPSTORY_STORY_MISSION("TL21", "dreamanim", "A New Jerusalem"),
};
#undef COOPSTORY_STORY_MISSION

[[nodiscard]] constexpr std::optional<CampaignMissionDefinition>
FindCampaignMission(const std::uint32_t missionId) noexcept {
    for (const auto& definition : kCampaignMissionCatalog) {
        if (definition.missionId == missionId) return definition;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool HasVerifiedCampaignCompletionMapping(
    const std::uint32_t missionId) noexcept {
    const auto definition = FindCampaignMission(missionId);
    return definition.has_value() &&
        definition->completionBinding ==
            CampaignCompletionBinding::MissionDataRatingAndDerivedUnlocks;
}

// Unlocks which vanilla derives from a Story MissionData completion are part
// of every admitted mapping. Direct item/weapon entitlements remain in the
// per-mission reward span because they are not derivable from MissionData.
[[nodiscard]] constexpr bool PropagatesCampaignMissionDerivedUnlocks(
    const std::uint32_t missionId) noexcept {
    return HasVerifiedCampaignCompletionMapping(missionId);
}

[[nodiscard]] constexpr std::span<const CampaignMissionReward>
CampaignMissionRewards(const std::uint32_t missionId) noexcept {
    const auto definition = FindCampaignMission(missionId);
    return definition.has_value() ? definition->rewards
                                  : std::span<const CampaignMissionReward>{};
}

// Every catalogued mission has a dialogue profile.  A profile without roots
// intentionally falls back to that game's normal local mission dialogue; it
// never causes a peer to manufacture, restart, pause, or skip a conversation.
[[nodiscard]] constexpr bool HasCampaignMissionDialogueProfile(
    const std::uint32_t missionId,
    const std::uint32_t profileId) noexcept {
    return profileId == CampaignMissionDialogueProfileId(missionId) &&
        FindCampaignMission(missionId).has_value();
}

[[nodiscard]] constexpr std::span<const CampaignMissionDialogueRoot>
CampaignMissionDialogueRoots(const std::uint32_t missionId) noexcept {
    for (std::size_t first{}; first < kCampaignMissionDialogueRoots.size();
         ++first) {
        if (kCampaignMissionDialogueRoots[first].missionId != missionId) {
            continue;
        }
        auto end = first + 1U;
        while (end < kCampaignMissionDialogueRoots.size() &&
               kCampaignMissionDialogueRoots[end].missionId == missionId) {
            ++end;
        }
        return {kCampaignMissionDialogueRoots.data() + first, end - first};
    }
    return {};
}

[[nodiscard]] constexpr bool IsCampaignMissionDialogueRoot(
    const std::uint32_t missionId,
    const std::uint32_t rootId) noexcept {
    for (const auto& root : CampaignMissionDialogueRoots(missionId)) {
        if (CampaignMissionId(root.root) == rootId) return true;
    }
    return false;
}

[[nodiscard]] constexpr const CampaignMissionDialogueRoot*
FindCampaignMissionDialogueRoot(
    const std::uint32_t missionId,
    const std::uint32_t rootId) noexcept {
    for (const auto& root : CampaignMissionDialogueRoots(missionId)) {
        if (CampaignMissionId(root.root) == rootId) return &root;
    }
    return nullptr;
}

}  // namespace coopstory::bridge
