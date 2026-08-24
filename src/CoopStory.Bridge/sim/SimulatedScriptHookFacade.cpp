#include "SimulatedScriptHookFacade.hpp"

#include <algorithm>
#include <iostream>

namespace coopstory::bridge::simulation {

SimulatedScriptHookFacade::SimulatedScriptHookFacade() {
    sample_.slot = PlayerSlot::Host;
    sample_.position = {1.0F, 2.0F, 3.0F};
    sample_.velocity = {0.1F, 0.0F, 0.0F};
    sample_.heading = 90.0F;
    sample_.healthFraction = 1.0F;
}

std::uint64_t SimulatedScriptHookFacade::TickMilliseconds() noexcept {
    return tickMs_;
}

RuntimeMode SimulatedScriptHookFacade::QueryRuntimeMode() noexcept {
    return mode_;
}

std::optional<LocalPlayerSample>
SimulatedScriptHookFacade::SampleLocalPlayer() noexcept {
    return sample_;
}

std::optional<PlayerAnimationStatePayload>
SimulatedScriptHookFacade::SampleLocalAnimationState(
    const NetEntityId entityId,
    const PlayerSlot slot,
    const std::uint16_t locomotionEpoch,
    const std::uint32_t sampleSequence) noexcept {
    if (motionReplicationMode_ !=
            MotionReplicationWireMode::AnimGraphReplica ||
        !entityId.IsValid() || locomotionEpoch == 0U) {
        return std::nullopt;
    }
    return PlayerAnimationStatePayload{
        entityId,
        slot,
        kPlayerAnimationStateSchemaVersion,
        PlayerAnimationSampleSource::None,
        locomotionEpoch,
        sampleSequence};
}

std::optional<WorldStatePayload>
SimulatedScriptHookFacade::SampleWorldState() noexcept {
    return worldState_;
}

std::vector<HostWorldEntitySample>
SimulatedScriptHookFacade::SampleWorldEntities(
    const float,
    const std::size_t maximumEntities) noexcept {
    try {
        auto result = sampledWorldEntities_;
        if (result.size() > maximumEntities) {
            result.resize(maximumEntities);
        }
        return result;
    } catch (...) {
        return {};
    }
}

std::optional<DamageIntentPayload>
SimulatedScriptHookFacade::SampleWorldDamageIntent(
    const NetEntityId attackerId) noexcept {
    auto result = pendingWorldDamageIntent_;
    pendingWorldDamageIntent_.reset();
    if (result.has_value()) {
        result->attackerId = attackerId;
    }
    return result;
}

std::optional<float>
SimulatedScriptHookFacade::HostGuestDistanceMeters() noexcept {
    return distanceMeters_;
}

MenuInputState SimulatedScriptHookFacade::ReadMenuInput() noexcept {
    const auto result = input_;
    input_ = {};
    return result;
}

void SimulatedScriptHookFacade::DrawMenu(
    const bool open,
    const std::span<const BridgeCommand>,
    const std::size_t selected) noexcept {
    if (open != menuOpen_ || selected != selected_) {
        menuOpen_ = open;
        selected_ = selected;
        Log(open ? "F9 menu opened" : "F9 menu closed");
    }
}

void SimulatedScriptHookFacade::DrawSessionMenu(
    const SessionOverlayView& state) noexcept {
    sessionOverlay_ = state;
}

void SimulatedScriptHookFacade::DrawBridgeHud(
    const BridgeHudState& state) noexcept {
    const bool changed =
        state.bridgeActive != hudState_.bridgeActive ||
        state.sidecarConnected != hudState_.sidecarConnected ||
        state.localSlot != hudState_.localSlot ||
        state.remoteConnected != hudState_.remoteConnected ||
        state.diagnosticsEnabled != hudState_.diagnosticsEnabled ||
        state.soloOverrideEnabled != hudState_.soloOverrideEnabled;
    hudState_ = state;
    if (changed) {
        Log(
            state.remoteConnected
                ? "HUD: remote connected"
                : "HUD: waiting for remote");
    }
}

void SimulatedScriptHookFacade::DrawPauseVoteStatus(
    const PauseVoteView& state) noexcept {
    pauseVoteState_ = state;
}

std::string SimulatedScriptHookFacade::ReadClipboardText() noexcept {
    return clipboard_;
}

bool SimulatedScriptHookFacade::WriteClipboardText(
    const std::string_view text) noexcept {
    clipboard_.assign(text);
    return true;
}

void SimulatedScriptHookFacade::ShowMissionBubbleWarning(
    const float distanceMeters) noexcept {
    Log(
        "mission bubble warning at " +
        std::to_string(distanceMeters) +
        " m");
}

bool SimulatedScriptHookFacade::ExecuteCommand(
    const BridgeCommand command) noexcept {
    Log(
        "simulated menu command " +
        std::to_string(static_cast<std::uint16_t>(command)));
    return true;
}

bool SimulatedScriptHookFacade::ApplyNetworkCommand(
    const CommandPayload& command) noexcept {
    Log(
        "simulated network opcode " +
        std::to_string(static_cast<std::uint16_t>(command.opcode)));
    return true;
}

bool SimulatedScriptHookFacade::ApplyRemoteTransform(
    const PlayerStatePayload& state) noexcept {
    Log(
        "simulated smooth transform for " +
        std::to_string(state.entityId.Value()));
    return true;
}

bool SimulatedScriptHookFacade::ApplyRemoteAnimationState(
    const PlayerAnimationStatePayload& state) noexcept {
    if (motionReplicationMode_ !=
            MotionReplicationWireMode::AnimGraphReplica ||
        !state.entityId.IsValid()) {
        return false;
    }
    Log(
        "simulated remote animation sample " +
        std::to_string(state.sampleSequence));
    return true;
}

void SimulatedScriptHookFacade::ConfigureMotionReplication(
    const MotionReplicationConfigPayload& config) noexcept {
    if (config.revision == 0U) {
        return;
    }
    motionReplicationMode_ = config.mode;
    Log(
        config.mode == MotionReplicationWireMode::AnimGraphReplica
            ? "simulated motion mode AnimGraph Replica"
            : "simulated motion mode Task/Navmesh");
}

bool SimulatedScriptHookFacade::ApplyRemoteTraversal(
    const PlayerTraversalPayload& traversal) noexcept {
    Log(
        "simulated traversal transaction " +
        std::to_string(traversal.actionId) +
        " revision " +
        std::to_string(traversal.revision));
    return true;
}

bool SimulatedScriptHookFacade::ApplyRemotePlayerAction(
    const PlayerActionPayload& action) noexcept {
    Log(
        "simulated player action " +
        std::to_string(action.actionId) +
        " revision " +
        std::to_string(action.revision));
    return true;
}

bool SimulatedScriptHookFacade::ApplyRemoteIdentity(
    const PlayerIdentityPayload& identity) noexcept {
    remoteNickname_ = identity.nickname;
    Log(
        "simulated remote identity " +
        identity.nickname);
    return true;
}

bool SimulatedScriptHookFacade::ApplyWorldState(
    const WorldStatePayload& state) noexcept {
    worldState_ = state;
    Log("simulated host world state applied");
    return true;
}

bool SimulatedScriptHookFacade::ApplyRemoteEquipment(
    const EquipmentStatePayload& state) noexcept {
    Log(
        "simulated remote weapon " +
        std::to_string(state.weaponHash));
    return true;
}

bool SimulatedScriptHookFacade::MaintainRemoteMount(
    const PlayerMountStatePayload& state,
    const std::optional<PlayerMountStatePayload>& localState) noexcept {
    (void)localState;
    Log(
        "simulated remote mount " +
        std::to_string(state.mountEntityId.Value()));
    return true;
}

void SimulatedScriptHookFacade::ClearRemoteMount() noexcept {
    Log("simulated remote mount cleared");
}

bool SimulatedScriptHookFacade::SpawnWorldEntityProxy(
    const WorldEntityStatePayload& state) noexcept {
    try {
        const auto existing = std::find_if(
            worldEntityProxies_.begin(),
            worldEntityProxies_.end(),
            [&](const WorldEntityStatePayload& value) {
                return value.entityId == state.entityId;
            });
        if (existing == worldEntityProxies_.end()) {
            worldEntityProxies_.push_back(state);
        } else {
            *existing = state;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool SimulatedScriptHookFacade::UpdateWorldEntityProxy(
    const WorldEntityStatePayload& state) noexcept {
    return SpawnWorldEntityProxy(state);
}

void SimulatedScriptHookFacade::DespawnWorldEntityProxy(
    const NetEntityId entityId) noexcept {
    std::erase_if(
        worldEntityProxies_,
        [&](const WorldEntityStatePayload& value) {
            return value.entityId == entityId;
        });
}

void SimulatedScriptHookFacade::MaintainWorldMirrorGuest(
    const bool active,
    const bool,
    const float) noexcept {
    if (!active) {
        worldEntityProxies_.clear();
    }
}

bool SimulatedScriptHookFacade::ApplyWorldEntityDamage(
    const LocalEntityHandle,
    const float) noexcept {
    return true;
}

void SimulatedScriptHookFacade::MaintainRealtimeSession(
    const bool active,
    const bool synchronizedPaused) noexcept {
    synchronizedPaused_ =
        active && synchronizedPaused;
    if (active != realtimeSessionActive_) {
        realtimeSessionActive_ = active;
        Log(
            active
                ? "simulated real-time session enabled"
                : "simulated real-time session disabled");
    }
}

GuestMissionIsolationStatus
SimulatedScriptHookFacade::MaintainMissionAuthority(
    const bool active,
    const bool hostMissionActive,
    const bool hostPresentationActive) noexcept {
    if (active && hostMissionActive) {
        Log("simulated host mission authority enabled");
    }
    GuestMissionIsolationStatus status;
    status.missionGateAsserted =
        ShouldAssertGuestMissionGate(
            active,
            hostMissionActive,
            hostPresentationActive,
            false,
            false);
    return status;
}

void SimulatedScriptHookFacade::RequestCheckpointRetry() noexcept {
    ++retryCount_;
    Log("simulated checkpoint retry");
}

void SimulatedScriptHookFacade::Log(const std::string_view text) noexcept {
    try {
        logLines_.emplace_back(text);
        std::cout << "[bridge-sim] " << text << '\n';
    } catch (...) {
        // Simulation logging must not violate facade noexcept guarantees.
    }
}

void SimulatedScriptHookFacade::WaitForNextTick() noexcept {
    Advance(10U);
}

void SimulatedScriptHookFacade::Advance(
    const std::uint64_t milliseconds) noexcept {
    tickMs_ += milliseconds;
    sample_.position.x +=
        sample_.velocity.x * (static_cast<float>(milliseconds) / 1'000.0F);
}

void SimulatedScriptHookFacade::SetDistance(
    const std::optional<float> distance) noexcept {
    distanceMeters_ = distance;
}

void SimulatedScriptHookFacade::SetMissionActive(const bool active) noexcept {
    sample_.missionActive = active;
}

void SimulatedScriptHookFacade::SetCutsceneActive(const bool active) noexcept {
    sample_.cutsceneActive = active;
}

void SimulatedScriptHookFacade::SetDowned(const bool downed) noexcept {
    sample_.downed = downed;
}

}  // namespace coopstory::bridge::simulation
