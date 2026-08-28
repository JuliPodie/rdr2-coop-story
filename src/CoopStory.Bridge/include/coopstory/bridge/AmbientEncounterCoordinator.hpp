#pragma once

#include "coopstory/bridge/Domain.hpp"

#include <cstdint>
#include <optional>

namespace coopstory::bridge {

// These are bridge-owned activities, never names of Rockstar random-event
// scripts.  A local vanilla event can be proposed, but only this small stable
// vocabulary may become a cross-peer instance.
enum class AmbientEncounterProfile : std::uint8_t {
    RoadsideAmbush = 1,
    HostageRescue = 2,
    WagonDefense = 3,
    AnimalAttack = 4,
    CampClearout = 5,
};

// Every free-roam observation is classified before it can enter the network.
// Only the first lane can be created from ambient evidence. The exact-ID lane
// is reserved for entries that reuse the campaign barrier after a separately
// reviewed dialogue and reward contract; unknown vanilla scripts always stay
// local.
enum class EncounterCoopLane : std::uint8_t {
    BridgeOwned = 1,
    ExactIdBarrier = 2,
    LocalOnly = 3,
};

struct ExactIdEncounterContract final {
    std::uint32_t eventId{};
    bool bothPlayersEligible{};
    bool dialogueMappingVerified{};
    bool rewardMappingVerified{};
};

[[nodiscard]] constexpr EncounterCoopLane ClassifyExactIdEncounter(
    const ExactIdEncounterContract& contract) noexcept {
    return contract.eventId != 0U && contract.bothPlayersEligible &&
        contract.dialogueMappingVerified && contract.rewardMappingVerified
        ? EncounterCoopLane::ExactIdBarrier
        : EncounterCoopLane::LocalOnly;
}

[[nodiscard]] constexpr EncounterCoopLane ClassifyAmbientProfile(
    const AmbientEncounterProfile profile) noexcept {
    return profile >= AmbientEncounterProfile::RoadsideAmbush &&
        profile <= AmbientEncounterProfile::CampClearout
        ? EncounterCoopLane::BridgeOwned
        : EncounterCoopLane::LocalOnly;
}

enum class AmbientEncounterPhase : std::uint8_t {
    Proposed = 1,
    Preparing = 2,
    Active = 3,
    Succeeded = 4,
    Failed = 5,
    Abandoned = 6,
};

// The host records the guest's exact-event preflight before activating the
// common bridge-owned scene.  This is display/reward policy only; it never
// transfers a save, inventory or corpse-loot state.
enum class AmbientEncounterPeerDisposition : std::uint8_t {
    Unknown = 0,
    Participant = 1,
    Companion = 2,
};

enum class AmbientEncounterRejection : std::uint8_t {
    None = 0,
    UnsupportedProfile = 1,
    HostUnavailable = 2,
    ParticipantUnsafe = 3,
    TooFarAway = 4,
    Busy = 5,
    InvalidAnchor = 6,
};

struct AmbientEncounterProposal final {
    std::uint64_t proposalId{};
    AmbientEncounterProfile profile{AmbientEncounterProfile::RoadsideAmbush};
    Vec3 anchor{};
    float radiusMeters{};
    std::uint32_t localEvidenceHash{};
    std::uint32_t suggestedRosterSeed{};
};

struct AmbientEncounterInstance final {
    std::uint64_t instanceId{};
    AmbientEncounterProfile profile{AmbientEncounterProfile::RoadsideAmbush};
    AmbientEncounterPhase phase{AmbientEncounterPhase::Proposed};
    Vec3 anchor{};
    float radiusMeters{};
    std::uint32_t rosterSeed{};
    std::uint16_t rosterCount{};
    std::uint64_t hostStartTick{};
    bool hostOriginated{};
    // This is process-local presentation authority, never transmitted.  The
    // host creates the encounter actors; guests receive those actors through
    // the existing bounded world-entity replication lane.
    bool localAuthority{};
    std::uint32_t exactEventId{};
    AmbientEncounterPeerDisposition guestDisposition{
        AmbientEncounterPeerDisposition::Unknown};
    // Host-local discovery evidence.  Generic bridge scenes use this only to
    // identify which local source actors may be masked during presentation;
    // it is not a reward key and is not needed by the guest proxy scene.
    std::uint32_t sourceEvidenceHash{};
};

struct AmbientEncounterHostContext final {
    bool sessionReady{};
    bool missionOrCinematicActive{};
    bool localAnchorSafe{};
    float peerDistanceMeters{};
};

[[nodiscard]] constexpr bool IsSupportedAmbientEncounterProfile(
    const AmbientEncounterProfile profile) noexcept {
    return ClassifyAmbientProfile(profile) == EncounterCoopLane::BridgeOwned;
}

[[nodiscard]] constexpr bool IsTerminalAmbientEncounterPhase(
    const AmbientEncounterPhase phase) noexcept {
    return phase == AmbientEncounterPhase::Succeeded ||
        phase == AmbientEncounterPhase::Failed ||
        phase == AmbientEncounterPhase::Abandoned;
}

// Pure host authority policy.  It deliberately refuses to adopt while either
// player is in Story/cinematic ownership; the guest then keeps their ordinary
// local encounter and no world state is copied.
[[nodiscard]] constexpr AmbientEncounterRejection CanHostAdoptAmbientEncounter(
    const AmbientEncounterProposal& proposal,
    const AmbientEncounterHostContext& context,
    const bool instanceAlreadyActive) noexcept {
    if (!IsSupportedAmbientEncounterProfile(proposal.profile)) {
        return AmbientEncounterRejection::UnsupportedProfile;
    }
    if (!context.sessionReady) return AmbientEncounterRejection::HostUnavailable;
    if (context.missionOrCinematicActive) return AmbientEncounterRejection::ParticipantUnsafe;
    if (instanceAlreadyActive) return AmbientEncounterRejection::Busy;
    if (!context.localAnchorSafe || proposal.radiusMeters < 8.0F ||
        proposal.radiusMeters > 80.0F || proposal.localEvidenceHash == 0U ||
        proposal.suggestedRosterSeed == 0U) {
        return AmbientEncounterRejection::InvalidAnchor;
    }
    if (context.peerDistanceMeters < 0.0F || context.peerDistanceMeters > 120.0F) {
        return AmbientEncounterRejection::TooFarAway;
    }
    return AmbientEncounterRejection::None;
}

class AmbientEncounterCoordinator final {
public:
    [[nodiscard]] AmbientEncounterRejection StartFromHost(
        const AmbientEncounterProposal& proposal,
        const AmbientEncounterHostContext& context,
        const std::uint64_t instanceId,
        const std::uint64_t hostStartTick) noexcept {
        const auto rejection = CanHostAdoptAmbientEncounter(
            proposal, context, active_.has_value());
        if (rejection != AmbientEncounterRejection::None) return rejection;
        active_ = AmbientEncounterInstance{instanceId, proposal.profile,
            AmbientEncounterPhase::Preparing, proposal.anchor, proposal.radiusMeters,
            proposal.suggestedRosterSeed, RosterCount(proposal.profile), hostStartTick,
            true, true};
        active_->sourceEvidenceHash = proposal.localEvidenceHash;
        return AmbientEncounterRejection::None;
    }

    [[nodiscard]] AmbientEncounterRejection ProposeFromGuest(
        const AmbientEncounterProposal& proposal,
        const AmbientEncounterHostContext& context,
        const std::uint64_t instanceId,
        const std::uint64_t hostStartTick) noexcept {
        const auto rejection = CanHostAdoptAmbientEncounter(
            proposal, context, active_.has_value());
        if (rejection != AmbientEncounterRejection::None) return rejection;
        active_ = AmbientEncounterInstance{instanceId, proposal.profile,
            AmbientEncounterPhase::Preparing, proposal.anchor, proposal.radiusMeters,
            proposal.suggestedRosterSeed, RosterCount(proposal.profile), hostStartTick,
            false, true};
        active_->sourceEvidenceHash = proposal.localEvidenceHash;
        return AmbientEncounterRejection::None;
    }

    [[nodiscard]] bool Advance(
        const AmbientEncounterPhase next) noexcept {
        if (!active_.has_value()) return false;
        const auto current = active_->phase;
        const bool valid = (current == AmbientEncounterPhase::Preparing &&
                            next == AmbientEncounterPhase::Active) ||
            (current == AmbientEncounterPhase::Active &&
             (next == AmbientEncounterPhase::Succeeded ||
              next == AmbientEncounterPhase::Failed ||
              next == AmbientEncounterPhase::Abandoned));
        if (!valid) return false;
        active_->phase = next;
        return true;
    }

    void ClearTerminal() noexcept {
        if (active_.has_value() && IsTerminalAmbientEncounterPhase(active_->phase)) {
            active_.reset();
        }
    }

    [[nodiscard]] const std::optional<AmbientEncounterInstance>& Active() const noexcept {
        return active_;
    }
    [[nodiscard]] std::optional<AmbientEncounterInstance>& Active() noexcept {
        return active_;
    }

private:
    [[nodiscard]] static constexpr std::uint16_t RosterCount(
        const AmbientEncounterProfile profile) noexcept {
        switch (profile) {
            case AmbientEncounterProfile::RoadsideAmbush: return 4U;
            case AmbientEncounterProfile::HostageRescue: return 3U;
            case AmbientEncounterProfile::WagonDefense: return 5U;
            case AmbientEncounterProfile::AnimalAttack: return 2U;
            case AmbientEncounterProfile::CampClearout: return 6U;
        }
        return 0U;
    }

    std::optional<AmbientEncounterInstance> active_{};
};

}  // namespace coopstory::bridge
