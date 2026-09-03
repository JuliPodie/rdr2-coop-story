#pragma once

#include "coopstory/bridge/FrameCodec.hpp"

#include <cstdint>

namespace coopstory::bridge {

// Small pure rules for deciding whether a remote action update is newer/allowed.
// BridgeRuntime uses these before delayed combat/action packets affect RDR2.
// A PlayerAction channel is a small, reliable transaction stream.
// Action identifiers are monotonic uint32 values and therefore need serial-number arithmetic rather than a plain numeric comparison at wraparound.
[[nodiscard]] constexpr bool IsNewerPlayerActionId(
    const std::uint32_t candidate,
    const std::uint32_t current) noexcept {
    const auto delta = candidate - current;
    return delta != 0U && delta < 0x80000000U;
}

enum class RemotePlayerActionEpochDecision : std::uint8_t {
    AcceptInitial,
    AcceptRevision,
    AcceptNewBegin,
    IgnoreStaleRevision,
    IgnoreForeignTerminal,
    IgnoreForeignContinuation,
    IgnoreOlderBegin,
};

[[nodiscard]] constexpr bool IsTerminalPlayerActionPhase(
    const PlayerActionPhase phase) noexcept {
    return phase == PlayerActionPhase::End ||
           phase == PlayerActionPhase::Cancel ||
           phase == PlayerActionPhase::Reject;
}

[[nodiscard]] constexpr bool IsPlayerActionEpochStartPhase(
    const PlayerActionPhase phase) noexcept {
    return phase == PlayerActionPhase::Begin ||
           phase == PlayerActionPhase::Snapshot;
}

// TASK_LASSO_PED performs the complete wind-up and throw autonomously.
// Starting it from Begin/target acquisition therefore makes the receiving PC release the rope while the sender is still holding aim.
// Only a sender-confirmed physical catch may start the native task on the receiving PC.
[[nodiscard]] constexpr bool ShouldStartNativeLassoTask(
    const PlayerActionKind kind,
    const PlayerActionPhase phase,
    const bool targetsLocalPlayer,
    const bool targetAcquiredThisRevision,
    const bool physicalTargetEffect) noexcept {
    (void)targetAcquiredThisRevision;
    return targetsLocalPlayer &&
           !IsTerminalPlayerActionPhase(phase) &&
           (kind == PlayerActionKind::Lasso ||
            kind == PlayerActionKind::Hogtie) &&
           physicalTargetEffect;
}

// An End/Cancel belonging to an older action must never cancel the newer visual task currently owning the channel.
// Likewise, a Sustain without its reliable Begin is not allowed to manufacture a new action after reconnect.
[[nodiscard]] constexpr RemotePlayerActionEpochDecision
EvaluateRemotePlayerActionEpoch(
    const std::uint32_t currentActionId,
    const std::uint16_t currentRevision,
    const std::uint32_t incomingActionId,
    const std::uint16_t incomingRevision,
    const PlayerActionPhase incomingPhase) noexcept {
    if (currentActionId == 0U) {
        return IsPlayerActionEpochStartPhase(incomingPhase)
                   ? RemotePlayerActionEpochDecision::AcceptInitial
                   : IsTerminalPlayerActionPhase(incomingPhase)
                         ? RemotePlayerActionEpochDecision::IgnoreForeignTerminal
                         : RemotePlayerActionEpochDecision::IgnoreForeignContinuation;
    }
    if (incomingActionId == currentActionId) {
        return incomingRevision > currentRevision
                   ? RemotePlayerActionEpochDecision::AcceptRevision
                   : RemotePlayerActionEpochDecision::IgnoreStaleRevision;
    }
    if (IsTerminalPlayerActionPhase(incomingPhase)) {
        return RemotePlayerActionEpochDecision::IgnoreForeignTerminal;
    }
    if (!IsPlayerActionEpochStartPhase(incomingPhase)) {
        return RemotePlayerActionEpochDecision::IgnoreForeignContinuation;
    }
    return IsNewerPlayerActionId(incomingActionId, currentActionId)
               ? RemotePlayerActionEpochDecision::AcceptNewBegin
               : RemotePlayerActionEpochDecision::IgnoreOlderBegin;
}

[[nodiscard]] constexpr bool AcceptsRemotePlayerActionEpoch(
    const RemotePlayerActionEpochDecision decision) noexcept {
    return decision == RemotePlayerActionEpochDecision::AcceptInitial ||
           decision == RemotePlayerActionEpochDecision::AcceptRevision ||
           decision == RemotePlayerActionEpochDecision::AcceptNewBegin;
}

}  // namespace coopstory::bridge
