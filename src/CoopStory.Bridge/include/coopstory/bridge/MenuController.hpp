#pragma once

#include "coopstory/bridge/FrameCodec.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace coopstory::bridge {

// Converts in-game key presses into local Bridge menu commands such as resync.
// BridgeRuntime/Sidecar still decide which commands may affect shared state.
struct MenuInputState final {
    bool f7{};
    bool f8{};
    bool f9{};
    bool f10{};
    bool up{};
    bool down{};
    bool left{};
    bool right{};
    bool confirm{};
    bool cancel{};
};

struct MenuUpdate final {
    bool visibilityChanged{};
    std::optional<BridgeCommand> command{};
};

class MenuController final {
public:
    [[nodiscard]] MenuUpdate Update(const MenuInputState& input);
    [[nodiscard]] bool IsOpen() const noexcept { return open_; }
    [[nodiscard]] std::size_t Selection() const noexcept { return selection_; }
    [[nodiscard]] std::span<const BridgeCommand> Commands() const noexcept;
    [[nodiscard]] static std::string_view Label(BridgeCommand command) noexcept;
    [[nodiscard]] static constexpr std::size_t PrimaryCommandCount() noexcept {
        return 13U;
    }
    void Close() noexcept { open_ = false; }

private:
    [[nodiscard]] static bool Rising(bool current, bool previous) noexcept {
        return current && !previous;
    }

    bool open_{};
    std::size_t selection_{};
    MenuInputState previous_{};
};

}  // namespace coopstory::bridge
