#include "SimulatedScriptHookFacade.hpp"

#include "coopstory/bridge/BridgeRuntime.hpp"
#include "coopstory/bridge/Transport.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Memory-only transport used by the Bridge simulation.
// It acts like the local pipe without opening RDR2 or Windows named-pipe resources.
class MemoryTransport final : public coopstory::bridge::IFrameTransport {
public:
    bool Connect(std::string& error) override {
        error.clear();
        connected_ = true;
        return true;
    }

    void Disconnect() noexcept override { connected_ = false; }

    [[nodiscard]] bool IsConnected() const noexcept override {
        return connected_;
    }

    bool Send(
        const coopstory::bridge::Frame& frame,
        std::string& error) override {
        error.clear();
        if (!connected_) {
            error = "memory transport disconnected";
            return false;
        }
        sent_.push_back(frame);
        if (frame.header.type ==
            coopstory::bridge::MessageType::Hello) {
            coopstory::bridge::Frame acknowledgement;
            acknowledgement.header.type =
                coopstory::bridge::MessageType::HelloAck;
            acknowledgement.header.sequence = ++inboundSequence_;
            acknowledgement.header.tick = frame.header.tick;
            acknowledgement.payload = {
                static_cast<std::uint8_t>(
                    coopstory::bridge::PlayerSlot::Host)};
            inbound_.push_back(std::move(acknowledgement));

            coopstory::bridge::Frame remoteState;
            remoteState.header.type =
                coopstory::bridge::MessageType::PlayerState;
            // HelloAck and forwarded LAN state have independent sequences.
            remoteState.header.sequence = inboundSequence_;
            remoteState.header.tick = frame.header.tick;
            remoteState.payload =
                coopstory::bridge::EncodePlayerState(
                    coopstory::bridge::PlayerStatePayload{
                        coopstory::bridge::NetEntityId::Compose(
                            0x51554C41U,
                            2U),
                        coopstory::bridge::PlayerSlot::Guest,
                        coopstory::bridge::PlayerLifecycle::Alive,
                        {5.0F, 2.0F, 3.0F},
                        {},
                        90.0F,
                        1.0F,
                        0U});
            inbound_.push_back(std::move(remoteState));
        }
        return true;
    }

    std::vector<coopstory::bridge::Frame> Poll(
        std::string& error) override {
        error.clear();
        auto result = std::move(inbound_);
        inbound_.clear();
        return result;
    }

    [[nodiscard]] std::size_t SentCount() const noexcept {
        return sent_.size();
    }

private:
    bool connected_{};
    std::vector<coopstory::bridge::Frame> sent_{};
    std::vector<coopstory::bridge::Frame> inbound_{};
    std::uint32_t inboundSequence_{};
};

}  // namespace

int main() {
    using namespace coopstory::bridge;
    simulation::SimulatedScriptHookFacade facade;
    MemoryTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supportedIdentity{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};

    std::string error;
    if (!runtime.Start(supportedIdentity, error)) {
        std::cerr << "simulation could not start: " << error << '\n';
        return 1;
    }

    for (std::size_t frame = 0; frame < 120U; ++frame) {
        if (frame == 20U) {
            facade.SetMissionActive(true);
            facade.SetDistance(205.0F);
        }
        if (frame == 50U) {
            facade.SetDistance(251.0F);
        }
        if (frame == 70U) {
            facade.SetDistance(25.0F);
        }
        runtime.Tick();
        facade.Advance(10U);
    }

    runtime.Stop("simulation complete");
    std::cout << "Simulation completed; emitted " << transport.SentCount()
              << " framed IPC messages.\n";
    return transport.SentCount() >= 20U ? 0 : 2;
}
