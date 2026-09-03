#pragma once

#include "coopstory/bridge/AmbientEncounterCoordinator.hpp"
#include "coopstory/bridge/CampaignMissionCatalog.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace coopstory::bridge {

// Approved free-roam encounter replacements owned by this mod.
// The list keeps unknown Rockstar events local instead of trying to replicate unsafe scripts.
// These entries are a reviewed discovery catalog for build 1.0.1491.50.
// An entry admits only a bridge-owned replacement scene; it never authorizes execution, reward handling, Honor, law state, or save progress from the matching Rockstar script.
// Keep an unfamiliar script local until it is classified here and exercised with the two-player test checklist.
struct BridgeOwnedEncounterDefinition final {
    std::uint32_t scriptId{};
    const char* scriptName{};
    AmbientEncounterProfile profile{AmbientEncounterProfile::RoadsideAmbush};
};

// The catalog is intentionally broader than the one exact-ID adaptation in ExactEncounterCatalog.hpp.
// Every record below maps into one of five bounded bridge profiles and therefore has the same host-owned outcome and cleanup rules.
// The source script remains private and is only used as local evidence that it is time to offer a replacement scene.
inline constexpr std::array<BridgeOwnedEncounterDefinition, 94U>
    kBridgeOwnedEncounterCatalog{{
        {CampaignMissionId("ambush_bnd_cliff1"), "ambush_bnd_cliff1", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_bnd_ridge_ambush"), "ambush_bnd_ridge_ambush", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_bnd_sniper_attack"), "ambush_bnd_sniper_attack", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_exc_bridge_trap"), "ambush_exc_bridge_trap", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_exc_hide_cover"), "ambush_exc_hide_cover", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_exc_lookout_attack"), "ambush_exc_lookout_attack", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_exc_road_robbery"), "ambush_exc_road_robbery", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_exc_scm_prec"), "ambush_exc_scm_prec", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_exc_wagon_bomb"), "ambush_exc_wagon_bomb", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_exc_wagon_turret"), "ambush_exc_wagon_turret", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_gen_night_rob"), "ambush_gen_night_rob", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_inb_bridge_ambush"), "ambush_inb_bridge_ambush", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_inb_forest"), "ambush_inb_forest", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_inb_forest_attack"), "ambush_inb_forest_attack", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_inb_harass"), "ambush_inb_harass", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_inb_road_attack"), "ambush_inb_road_attack", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_odr_bridge_ambush"), "ambush_odr_bridge_ambush", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_odr_bridge_prevent"), "ambush_odr_bridge_prevent", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_odr_bridge_trap"), "ambush_odr_bridge_trap", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_odr_hso"), "ambush_odr_hso", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_odr_lookout_attack"), "ambush_odr_lookout_attack", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_odr_ride_out"), "ambush_odr_ride_out", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_odr_road_prec"), "ambush_odr_road_prec", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_odr_road_robbery"), "ambush_odr_road_robbery", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_pnk_type1"), "ambush_pnk_type1", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_rnc_type1"), "ambush_rnc_type1", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_sav_corner"), "ambush_sav_corner", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_sav_forest_attack"), "ambush_sav_forest_attack", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_sav_lookout_attack"), "ambush_sav_lookout_attack", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_sav_river_ambush"), "ambush_sav_river_ambush", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("ambush_sav_tree_line"), "ambush_sav_tree_line", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_campfire_ambush"), "beat_campfire_ambush", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_dark_alley_ambush"), "beat_dark_alley_ambush", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_parlor_ambush"), "beat_parlor_ambush", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_slum_ambush"), "beat_slum_ambush", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_del_lobo_posse"), "beat_del_lobo_posse", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_drunk_dueler"), "beat_drunk_dueler", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_duel_boaster"), "beat_duel_boaster", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_duel_winner"), "beat_duel_winner", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_foot_robbery"), "beat_foot_robbery", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_arms_deal"), "beat_arms_deal", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_bandito_breakout"), "beat_bandito_breakout", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_boat_attack"), "beat_boat_attack", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_booby_trap"), "beat_booby_trap", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_bronte_patrol"), "beat_bronte_patrol", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_brontes_town_encounter"), "beat_brontes_town_encounter", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_checkpoint"), "beat_checkpoint", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_intimidation_tactics"), "beat_intimidation_tactics", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_savage_fight"), "beat_savage_fight", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_savage_warning"), "beat_savage_warning", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_stalking_shadows"), "beat_stalking_shadows", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_street_fight"), "beat_street_fight", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_town_confrontation"), "beat_town_confrontation", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_town_robbery"), "beat_town_robbery", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_town_trouble"), "beat_town_trouble", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_traffic_attack"), "beat_traffic_attack", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_train_holdup"), "beat_train_holdup", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_rowdy_drunks"), "beat_rowdy_drunks", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_lemoyne_town_encounter"), "beat_lemoyne_town_encounter", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_on_the_run"), "beat_on_the_run", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_outlaw_looter"), "beat_outlaw_looter", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_police_chase"), "beat_police_chase", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_posse_breakout"), "beat_posse_breakout", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_rally_dispute"), "beat_rally_dispute", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_laramie_gang_rustling"), "beat_laramie_gang_rustling", AmbientEncounterProfile::RoadsideAmbush},
        {CampaignMissionId("beat_hostage_rescue"), "beat_hostage_rescue", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_kidnap_victim"), "beat_kidnap_victim", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_lone_prisoner"), "beat_lone_prisoner", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_torturing_captive"), "beat_torturing_captive", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_trapped_woman"), "beat_trapped_woman", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_domestic_dispute"), "beat_domestic_dispute", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_drown_murder"), "beat_drown_murder", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_executions"), "beat_executions", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_dark_alley_stabber"), "beat_dark_alley_stabber", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_fleeing_family"), "beat_fleeing_family", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_inbred_kidnap"), "beat_inbred_kidnap", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_bandito_execution"), "beat_bandito_execution", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_chain_gang"), "beat_chain_gang", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_public_hanging"), "beat_public_hanging", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_wilderness_hanging"), "beat_wilderness_hanging", AmbientEncounterProfile::HostageRescue},
        {CampaignMissionId("beat_savage_wagon"), "beat_savage_wagon", AmbientEncounterProfile::WagonDefense},
        {CampaignMissionId("beat_wagon_threat"), "beat_wagon_threat", AmbientEncounterProfile::WagonDefense},
        {CampaignMissionId("beat_prison_wagon"), "beat_prison_wagon", AmbientEncounterProfile::WagonDefense},
        {CampaignMissionId("beat_bounty_transport"), "beat_bounty_transport", AmbientEncounterProfile::WagonDefense},
        {CampaignMissionId("beat_coach_robbery"), "beat_coach_robbery", AmbientEncounterProfile::WagonDefense},
        {CampaignMissionId("beat_outlaw_transport"), "beat_outlaw_transport", AmbientEncounterProfile::WagonDefense},
        {CampaignMissionId("beat_animal_attack"), "beat_animal_attack", AmbientEncounterProfile::AnimalAttack},
        {CampaignMissionId("beat_animal_mauling"), "beat_animal_mauling", AmbientEncounterProfile::AnimalAttack},
        {CampaignMissionId("av_animal_attack"), "av_animal_attack", AmbientEncounterProfile::AnimalAttack},
        {CampaignMissionId("beat_moonshine_camp"), "beat_moonshine_camp", AmbientEncounterProfile::CampClearout},
        {CampaignMissionId("beat_murder_campfire"), "beat_murder_campfire", AmbientEncounterProfile::CampClearout},
        {CampaignMissionId("beat_player_camp_attack"), "beat_player_camp_attack", AmbientEncounterProfile::CampClearout},
        {CampaignMissionId("av_amb_camp_robbery"), "av_amb_camp_robbery", AmbientEncounterProfile::CampClearout},
        {CampaignMissionId("beat_gang_ped1_encounter"), "beat_gang_ped1_encounter", AmbientEncounterProfile::CampClearout},
    }};

[[nodiscard]] constexpr const BridgeOwnedEncounterDefinition*
FindBridgeOwnedEncounter(const std::uint32_t scriptId) noexcept {
    for (const auto& definition : kBridgeOwnedEncounterCatalog) {
        if (definition.scriptId == scriptId) return &definition;
    }
    return nullptr;
}

[[nodiscard]] constexpr std::size_t BridgeOwnedEncounterCount(
    const AmbientEncounterProfile profile) noexcept {
    std::size_t count{};
    for (const auto& definition : kBridgeOwnedEncounterCatalog) {
        if (definition.profile == profile) ++count;
    }
    return count;
}

}  // namespace coopstory::bridge
