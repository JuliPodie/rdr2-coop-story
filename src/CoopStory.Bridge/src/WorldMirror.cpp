#include "coopstory/bridge/WorldMirror.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace coopstory::bridge {
// Keep track of the NPCs, horses, and objects shared between the host and guest.
// The host selects entities from samples supplied by the game-facing code and gives them network IDs.
// The guest remembers the host's latest description and requests local copies in the right order.
// A graph here means entities plus links between them, such as a rider linked to its horse.
// This file produces Spawn, Update, and Despawn signals for other Bridge code to carry out in RDR2.
// Returning a Spawn signal alone does not prove that RDR2 successfully created the copy.
namespace {

// RDR2 may hide an ambient NPC for a moment while loading.
// Wait a short time before telling the guest to remove that NPC.
constexpr std::uint64_t kMissingEntityGraceMilliseconds = 750U;
// Give entities already being shared a selection advantage to reduce constant swapping near the limit.
// These values affect ranking; they do not move NPCs or guarantee that an NPC can never be removed early.
constexpr std::uint64_t kMinimumResidenceMilliseconds = 3'000U;
constexpr float kIncumbentDistanceHysteresisMeters = 12.0F;
constexpr float kRecentAdmissionHysteresisMeters = 6.0F;

// Choose how long to remember an entity missing from the selected samples when there is space to keep it.
// Mission actors get more time because loading a scene can temporarily hide them from the host's scan.
[[nodiscard]] std::uint64_t MissingGraceMilliseconds(
    const HostWorldEntityPriority priority) noexcept {
    switch (priority) {
        case HostWorldEntityPriority::ScriptOwned:
            // Mission actors can briefly leave the ped pool during camera, interior and streaming transitions.
            // Retain their stable NetEntityId long enough to avoid visible despawn/spawn churn.
            return 15'000U;
        case HostWorldEntityPriority::Interactive:
            return 5'000U;
        case HostWorldEntityPriority::Combat:
            return 2'000U;
        case HostWorldEntityPriority::Scenario:
            return 1'500U;
        case HostWorldEntityPriority::Ambient:
        default:
            return kMissingEntityGraceMilliseconds;
    }
}

// Reject undefined or infinite coordinates before they can be sent to the other player's game.
[[nodiscard]] bool IsFinite(const Vec3& value) noexcept {
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z);
}

}  // namespace

// Prepare the host's entity-ID generator and the maximum number of tracked entities.
// Local RDR2 handles belong to this computer, so the guest receives generated network IDs instead.
WorldMirrorHost::WorldMirrorHost(
    const std::uint32_t epoch,
    const std::uint32_t firstCounter,
    const std::size_t maximumNodes)
    // Give each host session its own ID group so old NPC IDs are not reused after reconnecting.
    : generator_(epoch, firstCounter),
      maximumNodes_(std::max<std::size_t>(maximumNodes, 1U)) {}

// Compare the current game samples with the entities we shared on earlier updates.
// Return removals for retired entries, creations for new entries, and current state for retained entries.
// The caller decides when to run this method and how to send the returned signals.
std::vector<WorldMirrorSignal> WorldMirrorHost::Update(
    const std::span<const HostWorldEntitySample> samples,
    const std::uint64_t nowMs) {
    std::vector<WorldMirrorSignal> signals;
    signals.reserve(samples.size() + entries_.size());
    std::unordered_set<LocalEntityHandle> candidateHandles;
    candidateHandles.reserve(samples.size());
    std::vector<const HostWorldEntitySample*> candidates;
    candidates.reserve(samples.size());

    // Ignore bad or duplicate RDR2 handles before choosing NPCs to share.
    for (const auto& sample : samples) {
        if (!IsValid(sample) ||
            !candidateHandles.insert(sample.localHandle).second) {
            continue;
        }
        candidates.push_back(&sample);
    }

    // Do not share a rider unless we also share its horse/mount.
    std::erase_if(
        candidates,
        [&](const HostWorldEntitySample* sample) {
            const auto mounted =
                (sample->flags &
                 static_cast<std::uint8_t>(
                     WorldEntityStateFlag::Mounted)) != 0U;
            if (!mounted ||
                candidateHandles.contains(sample->parentLocalHandle)) {
                return false;
            }
            candidateHandles.erase(sample->localHandle);
            return true;
        });

    std::unordered_map<
        LocalEntityHandle,
        const HostWorldEntitySample*> candidatesByHandle;
    candidatesByHandle.reserve(candidates.size());
    for (const auto* candidate : candidates) {
        candidatesByHandle.emplace(candidate->localHandle, candidate);
    }

    // Rank important entities first, then use distance to choose between entities of equal priority.
    // Subtracting a distance bonus makes an existing entry more likely to keep its place in the list.
    // Recently admitted entries get an additional short-lived bonus to reduce immediate removal and respawn.
    const auto selectionDistance = [&](
                                       const HostWorldEntitySample* sample) {
        auto distance = sample->selectionDistanceMeters;
        const auto incumbent = entries_.find(sample->localHandle);
        if (incumbent == entries_.end()) {
            return distance;
        }
        distance -= kIncumbentDistanceHysteresisMeters;
        const auto admittedRecently =
            nowMs < incumbent->second.admittedMs ||
            nowMs - incumbent->second.admittedMs <
                kMinimumResidenceMilliseconds;
        if (admittedRecently) {
            distance -= kRecentAdmissionHysteresisMeters;
        }
        return distance;
    };
    std::ranges::sort(
        candidates,
        [&](const auto* lhs, const auto* rhs) {
            if (lhs->selectionPriority != rhs->selectionPriority) {
                return lhs->selectionPriority > rhs->selectionPriority;
            }
            const auto lhsDistance = selectionDistance(lhs);
            const auto rhsDistance = selectionDistance(rhs);
            if (lhsDistance != rhsDistance) {
                return lhsDistance < rhsDistance;
            }
            const auto lhsIncumbent =
                entries_.contains(lhs->localHandle);
            const auto rhsIncumbent =
                entries_.contains(rhs->localHandle);
            if (lhsIncumbent != rhsIncumbent) {
                return lhsIncumbent;
            }
            return lhs->localHandle < rhs->localHandle;
        });

    std::vector<const HostWorldEntitySample*> accepted;
    accepted.reserve(std::min(maximumNodes_, candidates.size()));
    std::unordered_set<LocalEntityHandle> observed;
    observed.reserve(maximumNodes_);
    // There is a hard NPC limit.
    // Extra NPCs are not sent, so far-away NPCs can disappear from the guest view.
    for (const auto* candidate : candidates) {
        if (observed.contains(candidate->localHandle)) {
            continue;
        }
        if (accepted.size() >= maximumNodes_) {
            ++selectionDeferred_;
            continue;
        }
        if (candidate->parentLocalHandle != 0 &&
            !observed.contains(candidate->parentLocalHandle)) {
            const auto parent = candidatesByHandle.find(
                candidate->parentLocalHandle);
            if (parent == candidatesByHandle.end() ||
                accepted.size() + 2U > maximumNodes_) {
                ++selectionDeferred_;
                continue;
            }
            accepted.push_back(parent->second);
            observed.insert(candidate->parentLocalHandle);
        }
        if (accepted.size() >= maximumNodes_) {
            ++selectionDeferred_;
            continue;
        }
        accepted.push_back(candidate);
        observed.insert(candidate->localHandle);
    }

    // A reused pool handle is a new generation.
    // If it is a parent, retire the complete old subtree child-first so no rider can remain attached to a deleted mount.
    // Every recreated node receives a fresh NetEntityId.
    std::unordered_set<LocalEntityHandle> replacedHandles;
    for (const auto* samplePointer : accepted) {
        const auto iterator = entries_.find(
            samplePointer->localHandle);
        if (iterator != entries_.end() &&
            (iterator->second.modelHash != samplePointer->modelHash ||
             iterator->second.state.kind != samplePointer->kind)) {
            replacedHandles.insert(iterator->first);
        }
    }
    // Extend a removal set to include every entity depending on an entity already in that set.
    // Repeat until no children are added, which also covers chains longer than horse plus rider.
    const auto expandDescendants = [&](auto& handles) {
        bool foundDescendant = true;
        while (foundDescendant) {
            foundDescendant = false;
            for (const auto& [handle, entry] : entries_) {
                if (handles.contains(handle) ||
                    !entry.state.parentEntityId.IsValid()) {
                    continue;
                }
                const auto parent = handlesByNetworkId_.find(
                    entry.state.parentEntityId);
                if (parent != handlesByNetworkId_.end() &&
                    handles.contains(parent->second)) {
                    handles.insert(handle);
                    foundDescendant = true;
                }
            }
        }
    };
    expandDescendants(replacedHandles);

    // Count parent links so a removal can process the furthest attached children first.
    // Remember visited IDs so a broken circular relationship cannot trap this loop forever.
    const auto hostDepth = [&](const LocalEntityHandle handle) {
        std::size_t depth{};
        auto iterator = entries_.find(handle);
        std::unordered_set<NetEntityId, NetEntityIdHash> visited;
        while (iterator != entries_.end() &&
               iterator->second.state.parentEntityId.IsValid() &&
               visited.insert(iterator->second.entityId).second &&
               depth <= entries_.size()) {
            const auto parentHandle = handlesByNetworkId_.find(
                iterator->second.state.parentEntityId);
            if (parentHandle == handlesByNetworkId_.end()) {
                break;
            }
            ++depth;
            iterator = entries_.find(parentHandle->second);
        }
        return depth;
    };

    // Expired nodes leave normally.
    // Nodes still inside the short grace window may stay only while the strict graph budget has spare room.
    std::unordered_set<LocalEntityHandle> retireHandles = replacedHandles;
    for (const auto& [handle, entry] : entries_) {
        const auto missing = !observed.contains(handle);
        const auto graceExpired =
            nowMs < entry.lastSeenMs ||
            nowMs - entry.lastSeenMs >=
                MissingGraceMilliseconds(entry.priority);
        if (missing && graceExpired) {
            retireHandles.insert(handle);
        }
    }
    expandDescendants(retireHandles);

    // Work out the size after both removals and new admissions, before changing the real dictionaries.
    const auto projectedNodeCount = [&]() {
        auto surviving = entries_.size() - retireHandles.size();
        for (const auto* sample : accepted) {
            if (!entries_.contains(sample->localHandle) ||
                retireHandles.contains(sample->localHandle)) {
                ++surviving;
            }
        }
        return surviving;
    };

    // If the list is full, remove the least important old NPCs first.
    while (projectedNodeCount() > maximumNodes_) {
        std::optional<LocalEntityHandle> victim;
        for (const auto& [handle, entry] : entries_) {
            if (retireHandles.contains(handle) ||
                observed.contains(handle)) {
                continue;
            }
            if (!victim.has_value()) {
                victim = handle;
                continue;
            }
            const auto& current = entries_.at(*victim);
            if (entry.priority < current.priority ||
                (entry.priority == current.priority &&
                 entry.distanceMeters > current.distanceMeters) ||
                (entry.priority == current.priority &&
                 entry.distanceMeters == current.distanceMeters &&
                 entry.lastSeenMs < current.lastSeenMs) ||
                (entry.priority == current.priority &&
                 entry.distanceMeters == current.distanceMeters &&
                 entry.lastSeenMs == current.lastSeenMs &&
                 handle < *victim)) {
                victim = handle;
            }
        }
        if (!victim.has_value()) {
            break;
        }
        const auto before = retireHandles.size();
        retireHandles.insert(*victim);
        expandDescendants(retireHandles);
        capacityEvictions_ += retireHandles.size() - before;
    }

    std::vector<LocalEntityHandle> retireOrder(
        retireHandles.begin(),
        retireHandles.end());
    std::ranges::sort(
        retireOrder,
        [&](const auto lhs, const auto rhs) {
            const auto lhsDepth = hostDepth(lhs);
            const auto rhsDepth = hostDepth(rhs);
            if (lhsDepth != rhsDepth) {
                return lhsDepth > rhsDepth;
            }
            return lhs < rhs;
        });
    // Remove riders/attached objects before removing their horse/parent.
    for (const auto handle : retireOrder) {
        const auto iterator = entries_.find(handle);
        // Another removal may already have cleared this entry, so skip handles that are no longer tracked.
        if (iterator == entries_.end()) {
            continue;
        }
        signals.push_back(
            WorldMirrorSignal{
                WorldMirrorSignalKind::Despawn,
                iterator->second.state,
                ++graphRevision_,
                iterator->second.revision});
        handlesByNetworkId_.erase(iterator->second.entityId);
        entries_.erase(iterator);
    }

    std::unordered_set<LocalEntityHandle> createdHandles;
    createdHandles.reserve(accepted.size());
    // Register the selected entities before building their outgoing descriptions.
    // An entity that is no longer in entries_ gets a new network ID when admitted again.
    for (const auto* samplePointer : accepted) {
        const auto& sample = *samplePointer;
        auto iterator = entries_.find(sample.localHandle);
        if (iterator == entries_.end()) {
            const auto entityId = generator_.Next();
            auto [inserted, created] = entries_.emplace(
                sample.localHandle,
                Entry{
                    entityId,
                    sample.localHandle,
                    sample.modelHash,
                    nowMs,
                    nowMs,
                    0U,
                    sample.selectionPriority,
                    sample.selectionDistanceMeters,
                    {}});
            if (!created) {
                continue;
            }
            handlesByNetworkId_[entityId] = sample.localHandle;
            createdHandles.insert(sample.localHandle);
            iterator = inserted;
        }

        iterator->second.lastSeenMs = nowMs;
        iterator->second.priority = sample.selectionPriority;
        iterator->second.distanceMeters =
            sample.selectionDistanceMeters;
    }

    // Parent nodes are emitted before children.
    // This ordering is stable even when worldGetAllPeds changes its pool order between frames.
    std::unordered_map<LocalEntityHandle, const HostWorldEntitySample*>
        samplesByHandle;
    samplesByHandle.reserve(accepted.size());
    for (const auto* samplePointer : accepted) {
        samplesByHandle.emplace(
            samplePointer->localHandle,
            samplePointer);
    }
    const auto sampleDepth = [&](const HostWorldEntitySample* sample) {
        std::size_t depth{};
        std::unordered_set<LocalEntityHandle> visited;
        auto current = sample;
        while (current != nullptr &&
               current->parentLocalHandle != 0 &&
               visited.insert(current->localHandle).second &&
               depth <= accepted.size()) {
            const auto parent = samplesByHandle.find(
                current->parentLocalHandle);
            if (parent == samplesByHandle.end()) {
                break;
            }
            ++depth;
            current = parent->second;
        }
        return depth;
    };
    std::ranges::sort(
        accepted,
        [&](const auto* lhs, const auto* rhs) {
            const auto lhsDepth = sampleDepth(lhs);
            const auto rhsDepth = sampleDepth(rhs);
            if (lhsDepth != rhsDepth) {
                return lhsDepth < rhsDepth;
            }
            return lhs->localHandle < rhs->localHandle;
        });

    for (const auto* samplePointer : accepted) {
        const auto& sample = *samplePointer;
        const auto iterator = entries_.find(sample.localHandle);
        if (iterator == entries_.end()) {
            continue;
        }
        NetEntityId parentEntityId{};
        if (sample.parentLocalHandle != 0) {
            const auto parent =
                entries_.find(sample.parentLocalHandle);
            if (parent != entries_.end()) {
                parentEntityId = parent->second.entityId;
            }
        }
        iterator->second.state = ToWireState(
            sample,
            iterator->second.entityId,
            parentEntityId);
        // Track both this entity's update count and the overall graph's change count.
        // The receiver uses the transmitted ordering information to avoid applying old descriptions later.
        ++iterator->second.revision;
        signals.push_back(
            WorldMirrorSignal{
                createdHandles.contains(sample.localHandle)
                    ? WorldMirrorSignalKind::Spawn
                    : WorldMirrorSignalKind::Update,
                iterator->second.state,
                ++graphRevision_,
                iterator->second.revision});
    }

    graceRetained_ = 0U;
    for (const auto& [handle, entry] : entries_) {
        (void)entry;
        if (!observed.contains(handle)) {
            ++graceRetained_;
        }
    }

    return signals;
}

// Describe every currently tracked entity again while preserving its network ID and saved state.
// This rebuilds the guest's knowledge after reconnecting without making the host rediscover all entities.
std::vector<WorldMirrorSignal> WorldMirrorHost::ReplayStableSpawns() {
    // After reconnecting, resend all current NPCs in the right order.
    std::vector<WorldMirrorSignal> signals;
    signals.reserve(entries_.size());
    std::vector<LocalEntityHandle> order;
    order.reserve(entries_.size());
    for (const auto& [handle, entry] : entries_) {
        (void)entry;
        order.push_back(handle);
    }
    const auto depth = [&](const LocalEntityHandle handle) {
        std::size_t result{};
        auto iterator = entries_.find(handle);
        std::unordered_set<NetEntityId, NetEntityIdHash> visited;
        while (iterator != entries_.end() &&
               iterator->second.state.parentEntityId.IsValid() &&
               visited.insert(iterator->second.entityId).second &&
               result <= entries_.size()) {
            const auto parent = handlesByNetworkId_.find(
                iterator->second.state.parentEntityId);
            if (parent == handlesByNetworkId_.end()) {
                break;
            }
            ++result;
            iterator = entries_.find(parent->second);
        }
        return result;
    };
    std::ranges::sort(
        order,
        [&](const auto lhs, const auto rhs) {
            const auto lhsDepth = depth(lhs);
            const auto rhsDepth = depth(rhs);
            if (lhsDepth != rhsDepth) {
                return lhsDepth < rhsDepth;
            }
            return lhs < rhs;
        });
    for (const auto handle : order) {
        const auto& entry = entries_.at(handle);
        signals.push_back(
            WorldMirrorSignal{
                WorldMirrorSignalKind::Spawn,
                entry.state,
                ++graphRevision_,
                entry.revision});
    }
    return signals;
}

// Produce removal signals for the whole shared list and then forget its tracked entries.
// This is a full rebuild boundary, so callers must expect the guest's copies to be removed.
// The network-ID generator is retained so later admissions can receive new IDs.
std::vector<WorldMirrorSignal> WorldMirrorHost::Reset() {
    // On a full reset, tell the guest to remove children first, then forget IDs.
    std::vector<WorldMirrorSignal> signals;
    signals.reserve(entries_.size());
    std::vector<LocalEntityHandle> order;
    order.reserve(entries_.size());
    for (const auto& [handle, entry] : entries_) {
        (void)entry;
        order.push_back(handle);
    }
    const auto depth = [&](const LocalEntityHandle handle) {
        std::size_t result{};
        auto iterator = entries_.find(handle);
        std::unordered_set<NetEntityId, NetEntityIdHash> visited;
        while (iterator != entries_.end() &&
               iterator->second.state.parentEntityId.IsValid() &&
               visited.insert(iterator->second.entityId).second &&
               result <= entries_.size()) {
            const auto parent = handlesByNetworkId_.find(
                iterator->second.state.parentEntityId);
            if (parent == handlesByNetworkId_.end()) {
                break;
            }
            ++result;
            iterator = entries_.find(parent->second);
        }
        return result;
    };
    std::ranges::sort(
        order,
        [&](const auto lhs, const auto rhs) {
            const auto lhsDepth = depth(lhs);
            const auto rhsDepth = depth(rhs);
            if (lhsDepth != rhsDepth) {
                return lhsDepth > rhsDepth;
            }
            return lhs < rhs;
        });
    for (const auto handle : order) {
        const auto& entry = entries_.at(handle);
        signals.push_back(
            WorldMirrorSignal{
                WorldMirrorSignalKind::Despawn,
                entry.state,
                ++graphRevision_,
                entry.revision});
    }
    entries_.clear();
    handlesByNetworkId_.clear();
    graceRetained_ = 0U;
    return signals;
}

// Report list size, parent links, and selection/removal counters for diagnostics.
// These numbers describe our tracking list, rather than checking visible RDR2 objects directly.
WorldMirrorGraphStats WorldMirrorHost::Stats() const noexcept {
    std::size_t edges{};
    for (const auto& [handle, entry] : entries_) {
        (void)handle;
        if (entry.state.parentEntityId.IsValid()) {
            ++edges;
        }
    }
    return {
        entries_.size(),
        entries_.size(),
        0U,
        edges,
        graphRevision_,
        0U,
        0U,
        0U,
        0U,
        0U,
        capacityEvictions_,
        selectionDeferred_,
        graceRetained_};
}

// Translate a shared network ID back to the host's local RDR2 handle.
// For example, a hit request names a network ID and the host needs its own handle to find the target.
// std::nullopt means this list no longer contains a matching entity.
std::optional<LocalEntityHandle> WorldMirrorHost::FindLocal(
    const NetEntityId entityId) const noexcept {
    const auto iterator = handlesByNetworkId_.find(entityId);
    if (iterator == handlesByNetworkId_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

// Look up the network ID already assigned to a local entity without creating a new entry.
std::optional<NetEntityId> WorldMirrorHost::FindNetwork(
    const LocalEntityHandle localHandle) const noexcept {
    if (localHandle == 0) {
        return std::nullopt;
    }
    const auto iterator = entries_.find(localHandle);
    return iterator == entries_.end()
               ? std::nullopt
               : std::optional<NetEntityId>{iterator->second.entityId};
}

// Return the last stored shared description for this entity, if it is still tracked.
// This reads the saved sample rather than asking RDR2 for a fresh sample.
std::optional<WorldEntityStatePayload> WorldMirrorHost::FindState(
    const NetEntityId entityId) const noexcept {
    const auto handle = FindLocal(entityId);
    if (!handle.has_value()) {
        return std::nullopt;
    }
    const auto iterator = entries_.find(*handle);
    if (iterator == entries_.end()) {
        return std::nullopt;
    }
    return iterator->second.state;
}

// Check that a local sample can be represented by the shared entity format.
// Flags must agree with each other, such as a rider being human and naming a separate parent.
// Positions and health must also contain usable numbers before any network ID is assigned.
bool WorldMirrorHost::IsValid(
    const HostWorldEntitySample& sample) noexcept {
    const auto knownFlags =
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Human) |
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Horse) |
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Dead) |
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::InCombat) |
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Firing) |
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Aiming) |
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Mounted) |
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::ScriptOwned);
    const auto human =
        (sample.flags &
         static_cast<std::uint8_t>(
             WorldEntityStateFlag::Human)) != 0U;
    const auto horse =
        (sample.flags &
         static_cast<std::uint8_t>(
             WorldEntityStateFlag::Horse)) != 0U;
    const auto inCombat =
        (sample.flags &
         static_cast<std::uint8_t>(
             WorldEntityStateFlag::InCombat)) != 0U;
    const auto usesWeapon =
        (sample.flags &
         (static_cast<std::uint8_t>(
              WorldEntityStateFlag::Firing) |
          static_cast<std::uint8_t>(
              WorldEntityStateFlag::Aiming))) != 0U;
    const auto mounted =
        (sample.flags &
         static_cast<std::uint8_t>(
             WorldEntityStateFlag::Mounted)) != 0U;
    const auto combatTarget =
        static_cast<std::uint8_t>(
            sample.combatTargetSlot);
    const bool object = sample.kind == WorldEntityKind::Object ||
                        sample.kind == WorldEntityKind::TrainCar;
    const bool ped = sample.kind == WorldEntityKind::Ped;
    // A scenery object must not carry character-only facts such as aiming, riding, or a weapon.
    const bool objectSemantics =
        !object ||
        (!human && !horse && !inCombat && !usesWeapon && !mounted &&
         sample.combatTargetSlot == WorldCombatTargetSlot::None &&
         sample.parentLocalHandle == 0 && sample.weaponHash == 0U &&
         (sample.taskKind == WorldTaskKind::Idle ||
          sample.taskKind == WorldTaskKind::Cinematic));
    return sample.localHandle != 0 &&
           sample.modelHash != 0U &&
           (ped || object) &&
           ValidHorseComponents(sample.horseComponents) &&
           (!sample.horseComponents || (ped && horse)) &&
           objectSemantics &&
           (sample.kind != WorldEntityKind::TrainCar ||
            (sample.taskKind == WorldTaskKind::Idle &&
             (sample.flags & ~static_cast<std::uint8_t>(WorldEntityStateFlag::ScriptOwned)) == 0U &&
             std::abs(sample.taskTarget.x) <= 360.0F &&
             std::abs(sample.taskTarget.y) <= 360.0F &&
             std::abs(sample.taskTarget.z) <= 360.0F)) &&
           (sample.flags & ~knownFlags) == 0U &&
           combatTarget <=
               static_cast<std::uint8_t>(
                   WorldCombatTargetSlot::Guest) &&
           (inCombat ||
            sample.combatTargetSlot ==
                WorldCombatTargetSlot::None) &&
           !(human && horse) &&
           static_cast<std::uint8_t>(sample.taskKind) <=
               static_cast<std::uint8_t>(
                   WorldTaskKind::Cinematic) &&
           (mounted == (sample.parentLocalHandle != 0)) &&
           (!mounted ||
            (human &&
             sample.parentLocalHandle !=
                 sample.localHandle &&
             sample.taskKind ==
                 WorldTaskKind::Mounted)) &&
           (sample.taskKind != WorldTaskKind::Dead ||
            (sample.flags &
             static_cast<std::uint8_t>(
                 WorldEntityStateFlag::Dead)) != 0U) &&
           (human || sample.weaponHash == 0U) &&
           (!usesWeapon ||
            (human && sample.weaponHash != 0U)) &&
           IsFinite(sample.position) &&
           IsFinite(sample.velocity) &&
           std::isfinite(sample.heading) &&
           sample.heading >= 0.0F &&
           sample.heading < 360.0F &&
           std::isfinite(sample.healthFraction) &&
           sample.healthFraction >= 0.0F &&
           sample.healthFraction <= 1.0F &&
           IsFinite(sample.taskTarget) &&
           static_cast<std::uint8_t>(sample.selectionPriority) <=
               static_cast<std::uint8_t>(
                   HostWorldEntityPriority::ScriptOwned) &&
           std::isfinite(sample.selectionDistanceMeters) &&
           sample.selectionDistanceMeters >= 0.0F;
}

// Copy the sampled position, health, and behavior into the payload sent between computers.
// Replace local parent handles with network IDs so the guest can identify its corresponding horse or object.
WorldEntityStatePayload WorldMirrorHost::ToWireState(
    const HostWorldEntitySample& sample,
    const NetEntityId entityId,
    const NetEntityId parentEntityId) const {
    return {
        entityId,
        sample.modelHash,
        sample.kind,
        sample.flags,
        sample.combatTargetSlot,
        sample.position,
        sample.velocity,
        sample.heading,
        sample.healthFraction,
        sample.weaponHash,
        sample.taskKind,
        parentEntityId,
        sample.taskTarget,
        sample.horseComponents};
}

// Set limits for the guest's desired entities and its remembered sequence history.
// A sequence tombstone is a small record kept after removal to help reject delayed older messages.
WorldMirrorGuestGraph::WorldMirrorGuestGraph(
    const std::size_t maximumNodes,
    const std::size_t maximumSequenceTombstones)
    : maximumNodes_(std::max<std::size_t>(maximumNodes, 1U)),
      maximumSequenceTombstones_(std::max(
          maximumSequenceTombstones,
          maximumNodes_)) {}

// Accept a newer host description for one entity and work out the necessary local changes.
// An accepted state can introduce an unknown ID, so this path can result in a Spawn as well as an Update.
// The surrounding protocol and runtime code are responsible for validating the full payload before this call.
std::vector<WorldMirrorSignal> WorldMirrorGuestGraph::ApplyState(
    const WorldEntityStatePayload& state,
    const std::uint32_t sequence) {
    if (!state.entityId.IsValid()) {
        return {};
    }
    TrimSequenceTombstones();
    // Each NPC remembers its last update number so old UDP updates are ignored.
    auto& window = sequences_[state.entityId];
    const auto disposition = window.Observe(sequence);
    if (disposition == SequenceDisposition::Duplicate) {
        ++duplicateMessages_;
        return {};
    }
    if (disposition == SequenceDisposition::Stale) {
        ++staleMessages_;
        return {};
    }
    auto iterator = nodes_.find(state.entityId);
    if (iterator == nodes_.end() &&
        nodes_.size() >= maximumNodes_) {
        ++capacityRejectedMessages_;
        return {};
    }

    ++acceptedMessages_;
    ++graphRevision_;
    std::vector<WorldMirrorSignal> signals;
    // If an NPC model changed, remove its old copy and make a new one.
    if (iterator != nodes_.end() &&
        iterator->second.state.modelHash != state.modelHash &&
        iterator->second.locallyActive) {
        signals.push_back(
            WorldMirrorSignal{
                WorldMirrorSignalKind::Despawn,
                iterator->second.state,
                graphRevision_,
                iterator->second.lastSequence});
        iterator->second.locallyActive = false;
    }
    // New nodes start inactive and dirty, meaning they still need their first local presentation signal.
    // Presentation here means asking other Bridge code to create or update the visible game copy.
    auto [nodeIterator, inserted] = nodes_.try_emplace(
        state.entityId,
        Node{state, sequence, false, true});
    if (!inserted) {
        nodeIterator->second.state = state;
        nodeIterator->second.lastSequence = sequence;
        nodeIterator->second.dirty = true;
    }
    // A newly available parent may also allow previously waiting riders to become active.
    auto reconciled = Reconcile();
    signals.insert(
        signals.end(),
        reconciled.begin(),
        reconciled.end());
    return signals;
}

// Apply a host removal only if its sequence passes the same ordering checks as entity updates.
// Remove the entity and its dependent children, retaining sequence history within the configured limit.
std::vector<WorldMirrorSignal> WorldMirrorGuestGraph::ApplyDespawn(
    const NetEntityId entityId,
    const std::uint32_t sequence) {
    if (!entityId.IsValid()) {
        return {};
    }
    TrimSequenceTombstones();
    auto& window = sequences_[entityId];
    const auto disposition = window.Observe(sequence);
    if (disposition == SequenceDisposition::Duplicate) {
        ++duplicateMessages_;
        return {};
    }
    if (disposition == SequenceDisposition::Stale) {
        ++staleMessages_;
        return {};
    }
    ++acceptedMessages_;
    ++graphRevision_;

    // Removing a parent also removes riders/children.
    // Remember their last update number so an old packet cannot bring a child back.
    const auto order = DescendantsChildFirst(entityId);
    std::vector<WorldMirrorSignal> signals;
    signals.reserve(order.size());
    for (const auto current : order) {
        const auto iterator = nodes_.find(current);
        if (iterator == nodes_.end()) {
            continue;
        }
        if (current != entityId) {
            // Record the parent's removal sequence for this child too, so an older UDP update cannot immediately restore it.
            (void)sequences_[current].Observe(sequence);
            ++cascadedDespawns_;
        }
        if (iterator->second.locallyActive) {
            signals.push_back(
                WorldMirrorSignal{
                    WorldMirrorSignalKind::Despawn,
                    iterator->second.state,
                    graphRevision_,
                    iterator->second.lastSequence});
        }
        nodes_.erase(iterator);
    }
    TrimSequenceTombstones();
    return signals;
}

// Request removal of every active guest copy and clear the desired entity list.
// The caller chooses whether old-message protection survives this reset or starts fresh as well.
std::vector<WorldMirrorSignal> WorldMirrorGuestGraph::Reset(
    const bool preserveSequenceTombstones) {
    // Turn every live NPC into a remove message, children first.
    std::vector<NetEntityId> order;
    order.reserve(nodes_.size());
    for (const auto& [entityId, node] : nodes_) {
        if (node.locallyActive) {
            order.push_back(entityId);
        }
    }
    std::ranges::sort(
        order,
        [&](const auto lhs, const auto rhs) {
            const auto lhsDepth = DependencyDepth(lhs);
            const auto rhsDepth = DependencyDepth(rhs);
            if (lhsDepth != rhsDepth) {
                return lhsDepth > rhsDepth;
            }
            return lhs < rhs;
        });
    std::vector<WorldMirrorSignal> signals;
    signals.reserve(order.size());
    for (const auto entityId : order) {
        const auto& node = nodes_.at(entityId);
        signals.push_back(
            WorldMirrorSignal{
                WorldMirrorSignalKind::Despawn,
                node.state,
                ++graphRevision_,
                node.lastSequence});
    }
    nodes_.clear();
    if (!preserveSequenceTombstones) {
        sequences_.clear();
    } else {
        TrimSequenceTombstones();
    }
    return signals;
}

// Say whether the guest knows this ID, even if its local creation is still waiting on a parent.
bool WorldMirrorGuestGraph::Contains(
    const NetEntityId entityId) const noexcept {
    return nodes_.contains(entityId);
}

// Count desired, active, and waiting entries together with rejected or repeated message counters.
// locallyActive means this graph issued an activation signal, not a fresh check that the game handle exists.
WorldMirrorGraphStats WorldMirrorGuestGraph::Stats() const noexcept {
    std::size_t active{};
    std::size_t edges{};
    for (const auto& [entityId, node] : nodes_) {
        (void)entityId;
        active += node.locallyActive ? 1U : 0U;
        edges += node.state.parentEntityId.IsValid() ? 1U : 0U;
    }
    return {
        nodes_.size(),
        active,
        nodes_.size() - active,
        edges,
        graphRevision_,
        acceptedMessages_,
        duplicateMessages_,
        staleMessages_,
        capacityRejectedMessages_,
        cascadedDespawns_};
}

// Compare what the host says should exist with what this graph has already asked the guest to present.
// Root entities can activate immediately, while attached entities wait until their whole parent chain is ready.
// Produce only the necessary removals, creations, and changed states in dependency order.
std::vector<WorldMirrorSignal> WorldMirrorGuestGraph::Reconcile() {
    // Do not make a rider/child until all its parents exist.
    std::unordered_set<NetEntityId, NetEntityIdHash> ready;
    ready.reserve(nodes_.size());
    for (const auto& [entityId, node] : nodes_) {
        if (!node.state.parentEntityId.IsValid()) {
            ready.insert(entityId);
        }
    }
    bool changed = true;
    while (changed) {
        // Each pass can make another level of children ready after their parents were found on an earlier pass.
        changed = false;
        for (const auto& [entityId, node] : nodes_) {
            if (ready.contains(entityId) ||
                !node.state.parentEntityId.IsValid() ||
                !ready.contains(node.state.parentEntityId)) {
                continue;
            }
            ready.insert(entityId);
            changed = true;
        }
    }

    // An active child loses its local copy if its required parent chain is unavailable.
    // A dirty entry has newer data to present; an unchanged clean entry needs no new signal.
    std::vector<NetEntityId> deactivate;
    std::vector<NetEntityId> activateOrUpdate;
    for (const auto& [entityId, node] : nodes_) {
        if (node.locallyActive && !ready.contains(entityId)) {
            deactivate.push_back(entityId);
        } else if (ready.contains(entityId) && node.dirty) {
            activateOrUpdate.push_back(entityId);
        }
    }
    const auto byDepthThenId = [&](
        const NetEntityId lhs,
        const NetEntityId rhs) {
        const auto lhsDepth = DependencyDepth(lhs);
        const auto rhsDepth = DependencyDepth(rhs);
        if (lhsDepth != rhsDepth) {
            return lhsDepth < rhsDepth;
        }
        return lhs < rhs;
    };
    std::ranges::sort(
        deactivate,
        [&](const auto lhs, const auto rhs) {
            return byDepthThenId(rhs, lhs);
        });
    std::ranges::sort(activateOrUpdate, byDepthThenId);

    std::vector<WorldMirrorSignal> signals;
    signals.reserve(deactivate.size() + activateOrUpdate.size());
    // Remove children first, then make/update parents first.
    // This keeps mounts and attachments working.
    for (const auto entityId : deactivate) {
        auto& node = nodes_.at(entityId);
        signals.push_back(
            WorldMirrorSignal{
                WorldMirrorSignalKind::Despawn,
                node.state,
                graphRevision_,
                node.lastSequence});
        node.locallyActive = false;
        node.dirty = true;
    }
    for (const auto entityId : activateOrUpdate) {
        auto& node = nodes_.at(entityId);
        signals.push_back(
            WorldMirrorSignal{
                node.locallyActive
                    ? WorldMirrorSignalKind::Update
                    : WorldMirrorSignalKind::Spawn,
                node.state,
                graphRevision_,
                node.lastSequence});
        node.locallyActive = true;
        node.dirty = false;
    }
    return signals;
}

// Gather one entity and everything attached beneath it, then sort children before their parents.
// A horse removal can therefore remove its rider first instead of leaving the rider attached to nothing.
std::vector<NetEntityId>
WorldMirrorGuestGraph::DescendantsChildFirst(
    const NetEntityId root) const {
    if (!nodes_.contains(root)) {
        return {};
    }
    std::unordered_set<NetEntityId, NetEntityIdHash> subtree{root};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [entityId, node] : nodes_) {
            if (!subtree.contains(entityId) &&
                subtree.contains(node.state.parentEntityId)) {
                subtree.insert(entityId);
                changed = true;
            }
        }
    }
    std::vector<NetEntityId> order(subtree.begin(), subtree.end());
    std::ranges::sort(
        order,
        [&](const auto lhs, const auto rhs) {
            const auto lhsDepth = DependencyDepth(lhs);
            const auto rhsDepth = DependencyDepth(rhs);
            if (lhsDepth != rhsDepth) {
                return lhsDepth > rhsDepth;
            }
            return lhs < rhs;
        });
    return order;
}

// Count how many parent links an entity has for ordering creation and removal.
// The visited set and size limit protect this walk from circular or broken relationships.
std::size_t WorldMirrorGuestGraph::DependencyDepth(
    const NetEntityId entityId) const noexcept {
    std::size_t depth{};
    auto iterator = nodes_.find(entityId);
    std::unordered_set<NetEntityId, NetEntityIdHash> visited;
    while (iterator != nodes_.end() &&
           iterator->second.state.parentEntityId.IsValid() &&
           visited.insert(iterator->first).second &&
           depth <= nodes_.size()) {
        ++depth;
        iterator = nodes_.find(
            iterator->second.state.parentEntityId);
    }
    return depth;
}

// Limit memory used by sequence records for entities that have already been removed.
// Keep sequence history for currently tracked entities, and discard inactive-ID records when space is needed.
// History for a discarded ID can no longer reject an old message by itself.
void WorldMirrorGuestGraph::TrimSequenceTombstones() {
    if (sequences_.size() < maximumSequenceTombstones_) {
        return;
    }
    for (auto iterator = sequences_.begin();
         iterator != sequences_.end() &&
         sequences_.size() >= maximumSequenceTombstones_;) {
        if (!nodes_.contains(iterator->first)) {
            iterator = sequences_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

}  // namespace coopstory::bridge
