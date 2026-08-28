#pragma once

#include "coopstory/bridge/IScriptHookFacade.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace coopstory::bridge::simulation {

class SimulatedScriptHookFacade final : public IScriptHookFacade {
public:
    SimulatedScriptHookFacade();

    [[nodiscard]] std::uint64_t TickMilliseconds() noexcept override;
    [[nodiscard]] RuntimeMode QueryRuntimeMode() noexcept override;
    [[nodiscard]] std::optional<LocalPlayerSample> SampleLocalPlayer()
        noexcept override;
    [[nodiscard]] std::optional<PlayerAnimationStatePayload>
    SampleLocalAnimationState(
        NetEntityId entityId,
        PlayerSlot slot,
        std::uint16_t locomotionEpoch,
        std::uint32_t sampleSequence) noexcept override;
    [[nodiscard]] std::optional<WorldStatePayload> SampleWorldState()
        noexcept override;
    [[nodiscard]] std::vector<HostWorldEntitySample> SampleWorldEntities(
        float radiusMeters,
        std::size_t maximumEntities) noexcept override;
    [[nodiscard]] std::optional<DamageIntentPayload> SampleWorldDamageIntent(
        NetEntityId attackerId) noexcept override;
    [[nodiscard]] std::optional<float> HostGuestDistanceMeters()
        noexcept override;
    [[nodiscard]] MenuInputState ReadMenuInput() noexcept override;
    void DrawMenu(
        bool open,
        std::span<const BridgeCommand> commands,
        std::size_t selected) noexcept override;
    void DrawSessionMenu(
        const SessionOverlayView& state) noexcept override;
    void DrawBridgeHud(const BridgeHudState& state) noexcept override;
    void DrawPauseVoteStatus(
        const PauseVoteView& state) noexcept override;
    [[nodiscard]] std::string ReadClipboardText() noexcept override;
    [[nodiscard]] bool WriteClipboardText(
        std::string_view text) noexcept override;
    void ShowMissionBubbleWarning(float distanceMeters) noexcept override;
    [[nodiscard]] bool ExecuteCommand(BridgeCommand command) noexcept override;
    [[nodiscard]] bool ApplyNetworkCommand(
        const CommandPayload& command) noexcept override;
    [[nodiscard]] bool ApplyRemoteTransform(
        const PlayerStatePayload& state) noexcept override;
    [[nodiscard]] bool ApplyRemoteAnimationState(
        const PlayerAnimationStatePayload& state) noexcept override;
    void ConfigureMotionReplication(
        const MotionReplicationConfigPayload& config) noexcept override;
    [[nodiscard]] bool ApplyRemoteTraversal(
        const PlayerTraversalPayload& traversal) noexcept override;
    [[nodiscard]] bool ApplyRemotePlayerAction(
        const PlayerActionPayload& action) noexcept override;
    [[nodiscard]] bool ApplyRemoteIdentity(
        const PlayerIdentityPayload& identity) noexcept override;
    [[nodiscard]] bool ApplyWorldState(
        const WorldStatePayload& state) noexcept override;
    [[nodiscard]] bool ApplyRemoteEquipment(
        const EquipmentStatePayload& state) noexcept override;
    [[nodiscard]] bool MaintainRemoteMount(
        const PlayerMountStatePayload& state,
        const std::optional<PlayerMountStatePayload>& localState) noexcept override;
    void ClearRemoteMount() noexcept override;
    [[nodiscard]] bool SpawnWorldEntityProxy(
        const WorldEntityStatePayload& state) noexcept override;
    [[nodiscard]] bool UpdateWorldEntityProxy(
        const WorldEntityStatePayload& state) noexcept override;
    void DespawnWorldEntityProxy(NetEntityId entityId) noexcept override;
    void MaintainWorldMirrorGuest(
        bool active,
        bool authoritativePopulationReady,
        float radiusMeters) noexcept override;
    [[nodiscard]] bool ApplyWorldEntityDamage(
        LocalEntityHandle target,
        float damage) noexcept override;
    void MaintainRealtimeSession(
        bool active,
        bool synchronizedPaused) noexcept override;
    [[nodiscard]] GuestMissionIsolationStatus MaintainMissionAuthority(
        bool active,
        bool hostMissionActive,
        bool hostPresentationActive,
        bool allowExpectedLocalMissionInstance = false) noexcept override;
    void RequestCheckpointRetry() noexcept override;
    void Log(std::string_view text) noexcept override;
    void WaitForNextTick() noexcept override;

    void Advance(std::uint64_t milliseconds) noexcept;
    void SetDistance(std::optional<float> distance) noexcept;
    void SetMissionActive(bool active) noexcept;
    void SetCutsceneActive(bool active) noexcept;
    void SetDowned(bool downed) noexcept;

    [[nodiscard]] const std::vector<std::string>& LogLines() const noexcept {
        return logLines_;
    }
    [[nodiscard]] std::size_t RetryCount() const noexcept {
        return retryCount_;
    }

private:
    std::uint64_t tickMs_{100U};
    RuntimeMode mode_{true, true, false};
    LocalPlayerSample sample_{};
    std::optional<float> distanceMeters_{50.0F};
    MenuInputState input_{};
    std::vector<std::string> logLines_{};
    std::size_t retryCount_{};
    bool menuOpen_{};
    std::size_t selected_{};
    BridgeHudState hudState_{};
    PauseVoteView pauseVoteState_{};
    SessionOverlayView sessionOverlay_{};
    std::string clipboard_{};
    std::string remoteNickname_{};
    std::vector<HostWorldEntitySample> sampledWorldEntities_{};
    std::vector<WorldEntityStatePayload> worldEntityProxies_{};
    std::optional<DamageIntentPayload> pendingWorldDamageIntent_{};
    WorldStatePayload worldState_{12U, 0U, 0U, 0U, 0U, 0U, 0.0F};
    bool realtimeSessionActive_{};
    bool synchronizedPaused_{};
    MotionReplicationWireMode motionReplicationMode_{
        MotionReplicationWireMode::TaskNavmesh};
};

}  // namespace coopstory::bridge::simulation
