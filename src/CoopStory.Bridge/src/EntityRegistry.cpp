#include "coopstory/bridge/EntityRegistry.hpp"

#include <limits>
#include <stdexcept>

namespace coopstory::bridge {

NetEntityIdGenerator::NetEntityIdGenerator(
    const std::uint32_t epoch,
    const std::uint32_t firstCounter)
    : epoch_(epoch), nextCounter_(firstCounter) {
    // Zero is reserved for "no entity"; every replicated identity must carry a nonzero session epoch and counter before entering maps or frames.
    if (epoch_ == 0U) {
        throw std::invalid_argument("NetEntityId epoch must be non-zero");
    }
    if (nextCounter_ == 0U) {
        throw std::invalid_argument("NetEntityId counter must be non-zero");
    }
}

NetEntityId NetEntityIdGenerator::Next() {
    // Counter exhaustion is fatal for this epoch instead of silently reusing an ID that a guest could still associate with another RDR2 proxy.
    if (nextCounter_ == 0U) {
        throw std::overflow_error("NetEntityId counter exhausted for this epoch");
    }
    const auto id = NetEntityId::Compose(epoch_, nextCounter_);
    if (nextCounter_ == std::numeric_limits<std::uint32_t>::max()) {
        nextCounter_ = 0U;
    } else {
        ++nextCounter_;
    }
    return id;
}

bool EntityRegistry::Bind(
    const NetEntityId id,
    const LocalEntityHandle handle) {
    if (!id.IsValid() || handle == 0) {
        return false;
    }

    // Maintain a strict one-to-one relationship between local RDR2 handles and portable network IDs.
    // A contradictory binding would corrupt despawns.
    const auto existingId = byLocalHandle_.find(handle);
    if (existingId != byLocalHandle_.end() && existingId->second != id) {
        return false;
    }
    const auto existingHandle = byNetworkId_.find(id);
    if (existingHandle != byNetworkId_.end() &&
        existingHandle->second != handle) {
        return false;
    }

    byNetworkId_[id] = handle;
    byLocalHandle_[handle] = id;
    return true;
}

std::optional<LocalEntityHandle> EntityRegistry::FindLocal(
    const NetEntityId id) const {
    const auto iterator = byNetworkId_.find(id);
    if (iterator == byNetworkId_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

std::optional<NetEntityId> EntityRegistry::FindNetwork(
    const LocalEntityHandle handle) const {
    const auto iterator = byLocalHandle_.find(handle);
    if (iterator == byLocalHandle_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

bool EntityRegistry::Remove(const NetEntityId id) {
    const auto iterator = byNetworkId_.find(id);
    if (iterator == byNetworkId_.end()) {
        return false;
    }
    byLocalHandle_.erase(iterator->second);
    byNetworkId_.erase(iterator);
    return true;
}

std::vector<LocalEntityHandle> EntityRegistry::Drain() {
    // Teardown callers need the native handles before clearing both lookup directions, so they can safely delete/release their game resources.
    std::vector<LocalEntityHandle> handles;
    handles.reserve(byNetworkId_.size());
    for (const auto& [id, handle] : byNetworkId_) {
        (void)id;
        handles.push_back(handle);
    }
    Clear();
    return handles;
}

void EntityRegistry::Clear() noexcept {
    byNetworkId_.clear();
    byLocalHandle_.clear();
}

}  // namespace coopstory::bridge
