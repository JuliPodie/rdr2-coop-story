#pragma once

#include "coopstory/bridge/Domain.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace coopstory::bridge {

inline constexpr std::uint32_t kReviveDurationMs = 4'000U;
inline constexpr float kReviveMaximumDistanceMeters = 2.0F;
inline constexpr float kRevivedHealthFraction = 0.35F;

struct PlayerRuntimeState final {
    PlayerLifecycle lifecycle{PlayerLifecycle::Alive};
    float healthFraction{1.0F};
    std::uint32_t reviveProgressMs{};
};

struct ReviveAttempt final {
    PlayerSlot reviver{PlayerSlot::Host};
    PlayerSlot target{PlayerSlot::Guest};
    bool interactionHeld{};
    float distanceMeters{};
};

enum class PlayerRuntimeSignalKind {
    ReviveStarted,
    ReviveCancelled,
    ReviveCompleted,
    RetryCheckpoint,
    SpectatorEntered,
    SpectatorExited,
};

struct PlayerRuntimeSignal final {
    PlayerRuntimeSignalKind kind{PlayerRuntimeSignalKind::ReviveStarted};
    PlayerSlot subject{PlayerSlot::Host};
    float value{};
};

class CoopPlayerStateMachine final {
public:
    CoopPlayerStateMachine();

    [[nodiscard]] const PlayerRuntimeState& State(PlayerSlot slot) const noexcept;
    void SetDowned(PlayerSlot slot);
    void SetAlive(PlayerSlot slot, float healthFraction = 1.0F);
    [[nodiscard]] std::vector<PlayerRuntimeSignal> SetSpectator(
        PlayerSlot slot,
        bool enabled);
    [[nodiscard]] std::vector<PlayerRuntimeSignal> Tick(
        std::uint32_t elapsedMs,
        std::optional<ReviveAttempt> reviveAttempt);

private:
    [[nodiscard]] static std::size_t Index(PlayerSlot slot) noexcept;
    [[nodiscard]] bool BothIncapacitated() const noexcept;
    void CancelRevive(PlayerSlot slot, std::vector<PlayerRuntimeSignal>& signals);

    std::array<PlayerRuntimeState, 2> players_{};
    std::array<PlayerLifecycle, 2> stateBeforeSpectator_{};
    bool retrySignalLatched_{};
};

}  // namespace coopstory::bridge
