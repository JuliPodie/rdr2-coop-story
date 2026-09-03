#pragma once

#include "coopstory/bridge/EntityRegistry.hpp"
#include "coopstory/bridge/FrameCodec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace coopstory::bridge {

// Host-only admission priority.
// This metadata never crosses IPC or the LAN boundary; it only decides which nearby entities occupy the bounded graph.
enum class HostWorldEntityPriority : std::uint8_t {
    Ambient = 0U,
    Scenario = 1U,
    Interactive = 2U,
    Combat = 3U,
    ScriptOwned = 4U,
};

// A process-local observation made by the authoritative host.
// The local handle is deliberately kept outside WorldEntityStatePayload and must never cross IPC or the LAN boundary.
struct HostWorldEntitySample final {
    LocalEntityHandle localHandle{};
    std::uint32_t modelHash{};
    WorldEntityKind kind{WorldEntityKind::Ped};
    std::uint8_t flags{};
    WorldCombatTargetSlot combatTargetSlot{
        WorldCombatTargetSlot::None};
    Vec3 position{};
    Vec3 velocity{};
    float heading{};
    float healthFraction{1.0F};
    std::uint32_t weaponHash{};
    WorldTaskKind taskKind{WorldTaskKind::Idle};
    LocalEntityHandle parentLocalHandle{};
    Vec3 taskTarget{};
    HostWorldEntityPriority selectionPriority{
        HostWorldEntityPriority::Ambient};
    float selectionDistanceMeters{};
};

// These tell the game what to do with an NPC/object copy: make it, update it, or remove it.
enum class WorldMirrorSignalKind {
    Spawn,
    Update,
    Despawn,
};

// One NPC/object change.
// The revision numbers help ignore old changes.
struct WorldMirrorSignal final {
    WorldMirrorSignalKind kind{WorldMirrorSignalKind::Update};
    WorldEntityStatePayload state{};
    std::uint64_t graphRevision{};
    std::uint32_t entityRevision{};
};

// Numbers shown in logs.
// They help tell whether NPC problems came from limits, old messages, or normal child cleanup.
struct WorldMirrorGraphStats final {
    std::size_t nodeCount{};
    std::size_t activeCount{};
    std::size_t pendingCount{};
    std::size_t edgeCount{};
    std::uint64_t graphRevision{};
    std::uint64_t acceptedMessages{};
    std::uint64_t duplicateMessages{};
    std::uint64_t staleMessages{};
    std::uint64_t capacityRejectedMessages{};
    std::uint64_t cascadedDespawns{};
    std::uint64_t capacityEvictions{};
    std::uint64_t selectionDeferred{};
    std::size_t graceRetained{};
};

// The host picks a limited number of nearby NPCs/objects to share with the guest, gives them IDs, and tells the guest when they change.
class WorldMirrorHost final {
public:
    explicit WorldMirrorHost(
        std::uint32_t epoch,
        std::uint32_t firstCounter = 1'000U,
        std::size_t maximumNodes = 48U);

    [[nodiscard]] std::vector<WorldMirrorSignal> Update(
        std::span<const HostWorldEntitySample> samples,
        std::uint64_t nowMs);
    // Re-emits the retained graph parent-first without allocating new IDs.
    // Used after a sidecar/pipe reconnect so a fresh cache can be rebuilt before a replayed AnimSceneDefinition references the same actors.
    [[nodiscard]] std::vector<WorldMirrorSignal> ReplayStableSpawns();
    [[nodiscard]] std::vector<WorldMirrorSignal> Reset();
    [[nodiscard]] std::optional<LocalEntityHandle> FindLocal(
        NetEntityId entityId) const noexcept;
    // Process-local reverse lookup used to translate captured AnimScene role bindings into stable network identities.
    // Local handles never cross the wire and are not persisted in the scene-definition cache.
    [[nodiscard]] std::optional<NetEntityId> FindNetwork(
        LocalEntityHandle localHandle) const noexcept;
    [[nodiscard]] std::optional<WorldEntityStatePayload> FindState(
        NetEntityId entityId) const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept {
        return entries_.size();
    }
    [[nodiscard]] WorldMirrorGraphStats Stats() const noexcept;

private:
    struct Entry final {
        NetEntityId entityId{};
        LocalEntityHandle localHandle{};
        std::uint32_t modelHash{};
        std::uint64_t lastSeenMs{};
        std::uint64_t admittedMs{};
        std::uint32_t revision{};
        HostWorldEntityPriority priority{
            HostWorldEntityPriority::Ambient};
        float distanceMeters{};
        WorldEntityStatePayload state{};
    };

    [[nodiscard]] static bool IsValid(
        const HostWorldEntitySample& sample) noexcept;
    [[nodiscard]] WorldEntityStatePayload ToWireState(
        const HostWorldEntitySample& sample,
        NetEntityId entityId,
        NetEntityId parentEntityId) const noexcept;

    NetEntityIdGenerator generator_;
    const std::size_t maximumNodes_;
    std::unordered_map<LocalEntityHandle, Entry> entries_{};
    std::unordered_map<NetEntityId, LocalEntityHandle, NetEntityIdHash>
        handlesByNetworkId_{};
    std::uint64_t graphRevision_{};
    std::uint64_t capacityEvictions_{};
    std::uint64_t selectionDeferred_{};
    std::size_t graceRetained_{};
};

// Client-side desired-state graph.
// Network state is accepted independently of local RDR2 handles, so an update can safely arrive before its reliable spawn.
// Mounted riders remain pending until the authoritative parent mount exists; parent removal cascades child-first to prevent floating replicas.
class WorldMirrorGuestGraph final {
public:
    explicit WorldMirrorGuestGraph(
        std::size_t maximumNodes = 48U,
        std::size_t maximumSequenceTombstones = 128U);

    [[nodiscard]] std::vector<WorldMirrorSignal> ApplyState(
        const WorldEntityStatePayload& state,
        std::uint32_t sequence);
    [[nodiscard]] std::vector<WorldMirrorSignal> ApplyDespawn(
        NetEntityId entityId,
        std::uint32_t sequence);
    [[nodiscard]] std::vector<WorldMirrorSignal> Reset(
        bool preserveSequenceTombstones = false);
    [[nodiscard]] bool Contains(NetEntityId entityId) const noexcept;
    [[nodiscard]] WorldMirrorGraphStats Stats() const noexcept;

private:
    struct Node final {
        WorldEntityStatePayload state{};
        std::uint32_t lastSequence{};
        bool locallyActive{};
        bool dirty{true};
    };

    [[nodiscard]] std::vector<WorldMirrorSignal> Reconcile();
    [[nodiscard]] std::vector<NetEntityId> DescendantsChildFirst(
        NetEntityId root) const;
    [[nodiscard]] std::size_t DependencyDepth(
        NetEntityId entityId) const noexcept;
    void TrimSequenceTombstones();

    const std::size_t maximumNodes_;
    const std::size_t maximumSequenceTombstones_;
    std::unordered_map<NetEntityId, Node, NetEntityIdHash> nodes_{};
    std::unordered_map<NetEntityId, SequenceWindow, NetEntityIdHash>
        sequences_{};
    std::uint64_t graphRevision_{};
    std::uint64_t acceptedMessages_{};
    std::uint64_t duplicateMessages_{};
    std::uint64_t staleMessages_{};
    std::uint64_t capacityRejectedMessages_{};
    std::uint64_t cascadedDespawns_{};
};

}  // namespace coopstory::bridge
