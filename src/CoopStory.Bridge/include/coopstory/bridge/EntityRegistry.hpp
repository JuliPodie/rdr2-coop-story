#pragma once

#include "coopstory/bridge/Domain.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace coopstory::bridge {

// Gives host-created NPC/object proxies shared NetEntityIds and remembers each PC's own local handle for that ID.
// The two IDs must never be confused.
class NetEntityIdGenerator final {
public:
    explicit NetEntityIdGenerator(std::uint32_t epoch, std::uint32_t firstCounter = 1U);

    [[nodiscard]] NetEntityId Next();
    [[nodiscard]] std::uint32_t Epoch() const noexcept { return epoch_; }

private:
    std::uint32_t epoch_{};
    std::uint32_t nextCounter_{};
};

class EntityRegistry final {
public:
    [[nodiscard]] bool Bind(NetEntityId id, LocalEntityHandle handle);
    [[nodiscard]] std::optional<LocalEntityHandle> FindLocal(NetEntityId id) const;
    [[nodiscard]] std::optional<NetEntityId> FindNetwork(LocalEntityHandle handle) const;
    [[nodiscard]] bool Remove(NetEntityId id);
    [[nodiscard]] std::vector<LocalEntityHandle> Drain();
    void Clear() noexcept;
    [[nodiscard]] std::size_t Size() const noexcept { return byNetworkId_.size(); }

private:
    std::unordered_map<NetEntityId, LocalEntityHandle, NetEntityIdHash> byNetworkId_{};
    std::unordered_map<LocalEntityHandle, NetEntityId> byLocalHandle_{};
};

}  // namespace coopstory::bridge
