#include "coopstory/bridge/WorldMirror.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace coopstory::bridge {
namespace {

// RDR2 may hide an ambient NPC for a moment while loading.
// Wait a short time before telling the guest to remove that NPC.
constexpr std::uint64_t kMissingEntityGraceMilliseconds = 750U;
constexpr std::uint64_t kMinimumResidenceMilliseconds = 3'000U;
constexpr float kIncumbentDistanceHysteresisMeters = 12.0F;
constexpr float kRecentAdmissionHysteresisMeters = 6.0F;

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

[[nodiscard]] bool IsFinite(const Vec3& value) noexcept {
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z);
}

}  // namespace

WorldMirrorHost::WorldMirrorHost(
    const std::uint32_t epoch,
    const std::uint32_t firstCounter,
    const std::size_t maximumNodes)
    // Give each host session its own ID group so old NPC IDs are not reused after reconnecting.
    : generator_(epoch, firstCounter),
      maximumNodes_(std::max<std::size_t>(maximumNodes, 1U)) {}

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

    // Prefer NPCs we already shared so nearby NPCs do not keep swapping places.
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
        // First time we see this RDR2 thing: give it a multiplayer ID.
        // Keep using that ID until the thing is removed.
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
    // Send horses/parents before riders/children so attachments work.
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

std::optional<LocalEntityHandle> WorldMirrorHost::FindLocal(
    const NetEntityId entityId) const noexcept {
    const auto iterator = handlesByNetworkId_.find(entityId);
    if (iterator == handlesByNetworkId_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

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
    const bool object = sample.kind == WorldEntityKind::Object;
    const bool ped = sample.kind == WorldEntityKind::Ped;
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
           objectSemantics &&
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

WorldEntityStatePayload WorldMirrorHost::ToWireState(
    const HostWorldEntitySample& sample,
    const NetEntityId entityId,
    const NetEntityId parentEntityId) const noexcept {
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
        sample.taskTarget};
}

WorldMirrorGuestGraph::WorldMirrorGuestGraph(
    const std::size_t maximumNodes,
    const std::size_t maximumSequenceTombstones)
    : maximumNodes_(std::max<std::size_t>(maximumNodes, 1U)),
      maximumSequenceTombstones_(std::max(
          maximumSequenceTombstones,
          maximumNodes_)) {}

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
    auto [nodeIterator, inserted] = nodes_.try_emplace(
        state.entityId,
        Node{state, sequence, false, true});
    if (!inserted) {
        nodeIterator->second.state = state;
        nodeIterator->second.lastSequence = sequence;
        nodeIterator->second.dirty = true;
    }
    auto reconciled = Reconcile();
    signals.insert(
        signals.end(),
        reconciled.begin(),
        reconciled.end());
    return signals;
}

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
            // A parent tombstone also protects its subtree from a delayed UDP update when the child's own reliable despawn was lost.
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

bool WorldMirrorGuestGraph::Contains(
    const NetEntityId entityId) const noexcept {
    return nodes_.contains(entityId);
}

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
