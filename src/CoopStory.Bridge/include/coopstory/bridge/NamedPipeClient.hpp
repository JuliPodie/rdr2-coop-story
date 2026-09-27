#pragma once

#include "coopstory/bridge/Transport.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace coopstory::bridge {

// Client side of the private local pipe between RDR2 Bridge and C# Sidecar.
// It is not the host-to-guest network connection; both PCs have their own pipe.
inline constexpr std::wstring_view kDefaultPipeStem = L"CoopStory.Bridge.v1";
inline constexpr std::uint32_t kDefaultPipeConnectTimeoutMs = 1U;

[[nodiscard]] std::wstring BuildCurrentUserPipeName(
    std::wstring_view stem = kDefaultPipeStem);

class NamedPipeClient final : public IFrameTransport {
public:
    explicit NamedPipeClient(
        std::wstring pipeName = BuildCurrentUserPipeName(),
        std::uint32_t connectTimeoutMs = kDefaultPipeConnectTimeoutMs,
        std::uint32_t writeTimeoutMs = 10U);
    ~NamedPipeClient() override;

    NamedPipeClient(const NamedPipeClient&) = delete;
    NamedPipeClient& operator=(const NamedPipeClient&) = delete;

    [[nodiscard]] bool Connect(std::string& error) override;
    void Disconnect() noexcept override;
    [[nodiscard]] bool IsConnected() const noexcept override;
    [[nodiscard]] bool Send(const Frame& frame, std::string& error) override;
    [[nodiscard]] std::vector<Frame> Poll(std::string& error) override;

    [[nodiscard]] const std::wstring& PipeName() const noexcept { return pipeName_; }

private:
    [[nodiscard]] bool VerifyServerOwner(std::string& error) const;
    [[nodiscard]] bool WriteAll(
        const std::uint8_t* data,
        std::size_t size,
        std::string& error);

    std::wstring pipeName_;
    std::uint32_t connectTimeoutMs_;
    std::uint32_t writeTimeoutMs_;
    void* pipe_{};
    FrameStreamDecoder decoder_{};
};

}  // namespace coopstory::bridge
