#pragma once

#include "coopstory/bridge/MenuController.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace coopstory::bridge {

enum class SessionOverlayAction {
    Host,
    JoinFromClipboard,
    StopSession,
    ToggleHud,
    Close,
};

enum class SessionOverlayPhase {
    WaitingForSidecar,
    ChooseMode,
    StartingHost,
    StartingGuest,
    ReadyHost,
    ReadyGuest,
    Error,
};

struct SessionMenuUpdate final {
    bool visibilityChanged{};
    bool hudVisibilityChanged{};
    std::optional<SessionOverlayAction> action{};
};

struct SessionOverlayView final {
    bool open{};
    bool hudVisible{};
    bool sessionReady{};
    SessionOverlayPhase phase{SessionOverlayPhase::WaitingForSidecar};
    std::size_t selection{};
    std::span<const SessionOverlayAction> actions{};
    std::string_view status{};
};

class SessionMenuController final {
public:
    [[nodiscard]] SessionMenuUpdate Update(const MenuInputState& input);
    void SetSidecarConnected(bool connected);
    void SetStatus(SessionOverlayPhase phase, std::string message);
    void MarkSessionReady(bool host, std::string message);
    void MarkSessionStopped(std::string message);

    [[nodiscard]] SessionOverlayView View() const noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return open_; }
    [[nodiscard]] bool IsHudVisible() const noexcept { return hudVisible_; }
    [[nodiscard]] bool IsSessionReady() const noexcept {
        return sessionReady_;
    }
    [[nodiscard]] static std::string_view Label(
        SessionOverlayAction action) noexcept;

private:
    [[nodiscard]] static bool Rising(bool current, bool previous) noexcept {
        return current && !previous;
    }

    bool open_{};
    bool hudVisible_{true};
    bool sessionReady_{};
    bool sidecarConnected_{};
    std::size_t selection_{};
    SessionOverlayPhase phase_{SessionOverlayPhase::WaitingForSidecar};
    std::string status_{
        "HOST/JOIN i haslo ustawiaj w launcherze. F8 otwiera panel awaryjny."};
    MenuInputState previous_{};
};

}  // namespace coopstory::bridge
