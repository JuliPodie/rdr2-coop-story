#include "coopstory/bridge/SessionMenuController.hpp"

#include <array>
#include <utility>

namespace coopstory::bridge {
namespace {

constexpr std::array kActions{
    SessionOverlayAction::StopSession,
    SessionOverlayAction::ToggleHud,
    SessionOverlayAction::Close,
};

}  // namespace

SessionMenuUpdate SessionMenuController::Update(
    const MenuInputState& input) {
    SessionMenuUpdate result;

    if (Rising(input.f8, previous_.f8)) {
        open_ = !open_;
        result.visibilityChanged = true;
    }
    if (Rising(input.f10, previous_.f10)) {
        hudVisible_ = !hudVisible_;
        result.hudVisibilityChanged = true;
    }

    if (open_) {
        if (Rising(input.cancel, previous_.cancel)) {
            open_ = false;
            result.visibilityChanged = true;
        } else {
            if (Rising(input.up, previous_.up)) {
                selection_ =
                    selection_ == 0U
                        ? kActions.size() - 1U
                        : selection_ - 1U;
            }
            if (Rising(input.down, previous_.down)) {
                selection_ = (selection_ + 1U) % kActions.size();
            }
            if (Rising(input.confirm, previous_.confirm)) {
                const auto action = kActions[selection_];
                if (action == SessionOverlayAction::ToggleHud) {
                    hudVisible_ = !hudVisible_;
                    result.hudVisibilityChanged = true;
                } else if (action == SessionOverlayAction::Close) {
                    open_ = false;
                    result.visibilityChanged = true;
                } else {
                    result.action = action;
                }
            }
        }
    }

    previous_ = input;
    return result;
}

void SessionMenuController::SetSidecarConnected(const bool connected) {
    if (connected == sidecarConnected_) {
        return;
    }
    sidecarConnected_ = connected;
    if (!connected && !sessionReady_) {
        phase_ = SessionOverlayPhase::WaitingForSidecar;
        status_ =
            "Brak sidecara. Uruchom HOST/JOIN bezposrednio w launcherze.";
    } else if (
        connected &&
        phase_ == SessionOverlayPhase::WaitingForSidecar) {
        phase_ = SessionOverlayPhase::ChooseMode;
        status_ =
            "HOST/JOIN i haslo ustawiasz w launcherze. F8 sluzy awaryjnie.";
    }
}

void SessionMenuController::SetStatus(
    const SessionOverlayPhase phase,
    std::string message) {
    phase_ = phase;
    status_ = std::move(message);
    if (phase == SessionOverlayPhase::Error) {
        open_ = true;
    }
}

void SessionMenuController::MarkSessionReady(
    const bool host,
    std::string message) {
    sessionReady_ = true;
    phase_ = host
                 ? SessionOverlayPhase::ReadyHost
                 : SessionOverlayPhase::ReadyGuest;
    status_ = std::move(message);
    open_ = false;
}

void SessionMenuController::MarkSessionStopped(
    std::string message) {
    sessionReady_ = false;
    phase_ = SessionOverlayPhase::ChooseMode;
    status_ = std::move(message);
    selection_ = 0U;
    open_ = false;
}

SessionOverlayView SessionMenuController::View() const noexcept {
    return {
        open_,
        hudVisible_,
        sessionReady_,
        phase_,
        selection_,
        kActions,
        status_};
}

std::string_view SessionMenuController::Label(
    const SessionOverlayAction action) noexcept {
    switch (action) {
        case SessionOverlayAction::Host:
            return "HOST jest dostepny tylko w launcherze";
        case SessionOverlayAction::JoinFromClipboard:
            return "JOIN jest dostepny tylko w launcherze";
        case SessionOverlayAction::StopSession:
            return "ZATRZYMAJ BIEZACA SESJE COOP";
        case SessionOverlayAction::ToggleHud:
            return "Pokaz/ukryj pasek statusu";
        case SessionOverlayAction::Close:
            return "Zamknij menu (F8 otwiera ponownie)";
    }
    return "Nieznana akcja";
}

}  // namespace coopstory::bridge
