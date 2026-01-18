#pragma once

#include <memory>

#include "fujinet/io/core/channel.h"
#include "fujinet/io/transport/legacy/packet_based_legacy_transport.h"
#include "fujinet/io/transport/legacy/bus_hardware.h"

namespace fujinet::io::transport::legacy {

// Apple II IWM (Integrated Woz Machine) transport implementation
// Packet-based protocol using status and data packets (no control bytes)
class IwmTransport : public PacketBasedLegacyTransport {
public:
    explicit IwmTransport(Channel& channel);
    virtual ~IwmTransport() = default;
    
protected:
    bool readCommandFrame(cmdFrame_t& frame) override;
    void sendStatusPacket(std::uint8_t status) override;
    void sendDataPacket(const std::uint8_t* buf, std::size_t len) override;
    std::size_t readDataPacket(std::uint8_t* buf, std::size_t len) override;
    bool commandNeedsData(std::uint8_t command) const override;
    
private:
    std::unique_ptr<BusHardware> _hardware;
    
    // IWM phase states
    enum class PhaseState {
        Idle,
        Reset,
        Enable
    };
    
    PhaseState _currentPhase{PhaseState::Idle};
    
    // IWM-specific: read phase lines to determine bus state
    PhaseState readPhases();
    
    // IWM-specific: handle INIT command (assigns device IDs dynamically)
    void handleInit();
    
    // IWM-specific: send status packet (IWM uses packet-based protocol)
    void sendStatusPacket(std::uint8_t status);
    
    // IWM-specific: send data packet
    void sendDataPacket(const std::uint8_t* buf, std::size_t len);
};

} // namespace fujinet::io::transport::legacy
