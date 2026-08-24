#include "coopstory/bridge/MenuController.hpp"

#include <array>

namespace coopstory::bridge {
namespace {

constexpr std::array kCommands{
    BridgeCommand::SkipCutscene,
    BridgeCommand::TeleportToPlayer,
    BridgeCommand::TeleportGuest,
    BridgeCommand::EmergencyRecover,
    BridgeCommand::ResyncEquipment,
    BridgeCommand::ResyncEntities,
    BridgeCommand::ToggleSoloOverride,
    BridgeCommand::StopSession,
    BridgeCommand::ToggleDiagnostics,
    BridgeCommand::RetryCheckpoint,
    BridgeCommand::GrantTestPistol,
    BridgeCommand::GrantTestLasso,
    BridgeCommand::ToggleSoloTest,
    BridgeCommand::ToggleGhostRecord,
    BridgeCommand::ToggleGhostReplay,
    BridgeCommand::Unload,
};

static_assert(
    kCommands.size() == MenuController::PrimaryCommandCount() * 2U);

}  // namespace

MenuUpdate MenuController::Update(const MenuInputState& input) {
    MenuUpdate result;
    if (Rising(input.f9, previous_.f9)) {
        open_ = !open_;
        result.visibilityChanged = true;
    }

    if (open_) {
        if (Rising(input.cancel, previous_.cancel)) {
            open_ = false;
            result.visibilityChanged = true;
        } else {
            constexpr auto kColumnSize =
                MenuController::PrimaryCommandCount();
            const auto columnStart =
                selection_ >= kColumnSize ? kColumnSize : 0U;
            const auto row = selection_ - columnStart;
            if (Rising(input.up, previous_.up)) {
                selection_ = columnStart +
                    (row == 0U ? kColumnSize - 1U : row - 1U);
            }
            if (Rising(input.down, previous_.down)) {
                selection_ = columnStart + ((row + 1U) % kColumnSize);
            }
            if (Rising(input.left, previous_.left) &&
                selection_ >= kColumnSize) {
                selection_ -= kColumnSize;
            }
            if (Rising(input.right, previous_.right) &&
                selection_ < kColumnSize) {
                selection_ += kColumnSize;
            }
            if (Rising(input.confirm, previous_.confirm)) {
                result.command = kCommands[selection_];
            }
        }
    }

    previous_ = input;
    return result;
}

std::span<const BridgeCommand> MenuController::Commands() const noexcept {
    return kCommands;
}

std::string_view MenuController::Label(const BridgeCommand command) noexcept {
    switch (command) {
        case BridgeCommand::SkipCutscene:
            return "Vote: skip cutscene";
        case BridgeCommand::ToggleSoloTest:
            return "Test solo: start / stop";
        case BridgeCommand::ToggleGhostRecord:
            return "Ghost Record: start / stop";
        case BridgeCommand::ToggleGhostReplay:
            return "Ghost Replay: start / stop";
        case BridgeCommand::ToggleSoloOverride:
            return "Solo override";
        case BridgeCommand::TeleportGuest:
            return "Teleport guest here (host)";
        case BridgeCommand::TeleportToPlayer:
            return "Teleport to player";
        case BridgeCommand::ResyncEntities:
            return "Resync entities";
        case BridgeCommand::ResyncEquipment:
            return "Resync equipment";
        case BridgeCommand::GrantTestPistol:
            return "Give pistol + max ammo (test)";
        case BridgeCommand::GrantTestLasso:
            return "Give lasso (test)";
        case BridgeCommand::EmergencyRecover:
            return "Emergency player recovery";
        case BridgeCommand::RetryCheckpoint:
            return "Retry checkpoint";
        case BridgeCommand::ToggleDiagnostics:
            return "Diagnostics";
        case BridgeCommand::SaveProblemMarker:
            return "Save problem marker";
        case BridgeCommand::StopSession:
            return "Stop co-op session";
        case BridgeCommand::Unload:
            return "Unload bridge";
    }
    return "Unknown";
}

}  // namespace coopstory::bridge
