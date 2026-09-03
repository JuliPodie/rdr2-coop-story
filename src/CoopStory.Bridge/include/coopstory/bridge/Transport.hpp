#pragma once

#include "coopstory/bridge/FrameCodec.hpp"

#include <string>
#include <vector>

namespace coopstory::bridge {

// Small interface for the Bridge's message connection.
// NamedPipeClient is the real version; simulations use a memory version with the same Frame methods.
class IFrameTransport {
public:
    virtual ~IFrameTransport() = default;

    [[nodiscard]] virtual bool Connect(std::string& error) = 0;
    virtual void Disconnect() noexcept = 0;
    [[nodiscard]] virtual bool IsConnected() const noexcept = 0;
    [[nodiscard]] virtual bool Send(const Frame& frame, std::string& error) = 0;
    [[nodiscard]] virtual std::vector<Frame> Poll(std::string& error) = 0;
};

}  // namespace coopstory::bridge
