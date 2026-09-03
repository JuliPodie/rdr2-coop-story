#include "coopstory/bridge/PlayerRuntime.hpp"

#include <algorithm>
#include <cmath>

namespace coopstory::bridge {

CoopPlayerStateMachine::CoopPlayerStateMachine() {
    // Spectator is an overlay state; remember the genuine lifecycle it must restore when a player leaves solo/spectator mode.
    stateBeforeSpectator_.fill(PlayerLifecycle::Alive);
}

std::size_t CoopPlayerStateMachine::Index(const PlayerSlot slot) noexcept {
    return slot == PlayerSlot::Host ? 0U : 1U;
}

const PlayerRuntimeState& CoopPlayerStateMachine::State(
    const PlayerSlot slot) const noexcept {
    return players_[Index(slot)];
}

void CoopPlayerStateMachine::SetDowned(const PlayerSlot slot) {
    // Network health is logical co-op state.
    // The facade separately applies the RDR2 downed presentation once BridgeRuntime receives these signals.
    auto& state = players_[Index(slot)];
    state.lifecycle = PlayerLifecycle::Downed;
    state.healthFraction = 0.0F;
    state.reviveProgressMs = 0U;
}

void CoopPlayerStateMachine::SetAlive(
    const PlayerSlot slot,
    const float healthFraction) {
    auto& state = players_[Index(slot)];
    state.lifecycle = PlayerLifecycle::Alive;
    state.healthFraction = std::clamp(healthFraction, 0.01F, 1.0F);
    state.reviveProgressMs = 0U;
}

std::vector<PlayerRuntimeSignal> CoopPlayerStateMachine::SetSpectator(
    const PlayerSlot slot,
    const bool enabled) {
    std::vector<PlayerRuntimeSignal> signals;
    auto& state = players_[Index(slot)];
    // Enter/exit emits an edge signal only once so the native facade does not recreate camera/visibility effects on every following simulation tick.
    if (enabled && state.lifecycle != PlayerLifecycle::Spectator) {
        stateBeforeSpectator_[Index(slot)] =
            state.lifecycle == PlayerLifecycle::Reviving
                ? PlayerLifecycle::Downed
                : state.lifecycle;
        state.lifecycle = PlayerLifecycle::Spectator;
        state.reviveProgressMs = 0U;
        signals.push_back(
            {PlayerRuntimeSignalKind::SpectatorEntered, slot, 0.0F});
    } else if (!enabled && state.lifecycle == PlayerLifecycle::Spectator) {
        state.lifecycle = stateBeforeSpectator_[Index(slot)];
        state.reviveProgressMs = 0U;
        signals.push_back(
            {PlayerRuntimeSignalKind::SpectatorExited, slot, 0.0F});
    }
    return signals;
}

void CoopPlayerStateMachine::CancelRevive(
    const PlayerSlot slot,
    std::vector<PlayerRuntimeSignal>& signals) {
    auto& state = players_[Index(slot)];
    if (state.lifecycle != PlayerLifecycle::Reviving) {
        return;
    }
    state.lifecycle = PlayerLifecycle::Downed;
    state.reviveProgressMs = 0U;
    signals.push_back(
        {PlayerRuntimeSignalKind::ReviveCancelled, slot, 0.0F});
}

std::vector<PlayerRuntimeSignal> CoopPlayerStateMachine::Tick(
    const std::uint32_t elapsedMs,
    const std::optional<ReviveAttempt> reviveAttempt) {
    std::vector<PlayerRuntimeSignal> signals;
    std::optional<PlayerSlot> validTarget;

    // Advance a revive only while its interaction, range, lifecycle and reviver eligibility remain valid; releasing any condition cancels it below.
    if (reviveAttempt.has_value()) {
        const auto& attempt = *reviveAttempt;
        auto& target = players_[Index(attempt.target)];
        const auto& reviver = players_[Index(attempt.reviver)];
        const bool distanceValid =
            std::isfinite(attempt.distanceMeters) &&
            attempt.distanceMeters >= 0.0F &&
            attempt.distanceMeters <= kReviveMaximumDistanceMeters;
        const bool targetCanBeRevived =
            target.lifecycle == PlayerLifecycle::Downed ||
            target.lifecycle == PlayerLifecycle::Reviving;
        const bool valid =
            attempt.reviver != attempt.target &&
            attempt.interactionHeld &&
            distanceValid &&
            targetCanBeRevived &&
            reviver.lifecycle == PlayerLifecycle::Alive;

        if (valid) {
            validTarget = attempt.target;
            if (target.lifecycle == PlayerLifecycle::Downed) {
                target.lifecycle = PlayerLifecycle::Reviving;
                target.reviveProgressMs = 0U;
                signals.push_back(
                    {PlayerRuntimeSignalKind::ReviveStarted,
                     attempt.target,
                     0.0F});
            }

            const auto remaining =
                kReviveDurationMs - target.reviveProgressMs;
            target.reviveProgressMs += std::min(elapsedMs, remaining);
            if (target.reviveProgressMs >= kReviveDurationMs) {
                target.lifecycle = PlayerLifecycle::Alive;
                target.healthFraction = kRevivedHealthFraction;
                target.reviveProgressMs = 0U;
                signals.push_back(
                    {PlayerRuntimeSignalKind::ReviveCompleted,
                     attempt.target,
                     kRevivedHealthFraction});
            }
        }
    }

    // Any target not confirmed during this tick loses its in-progress revive.
    for (const auto slot : {PlayerSlot::Host, PlayerSlot::Guest}) {
        if (!validTarget.has_value() || validTarget.value() != slot) {
            CancelRevive(slot, signals);
        }
    }

    // A mutual down is a revive/recovery state, not consent to discard a checkpoint.
    // The host must explicitly choose any retry from the menu.
    return signals;
}

}  // namespace coopstory::bridge
