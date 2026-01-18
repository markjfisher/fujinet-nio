#include "fujinet/io/transport/legacy/iwm_transport.h"
#include "fujinet/io/transport/legacy/bus_traits.h"
#include "fujinet/io/transport/legacy/bus_hardware.h"
#include "fujinet/core/logging.h"

namespace fujinet::io::transport::legacy {

static constexpr const char* TAG = "iwm";

IwmTransport::IwmTransport(Channel& channel)
    : PacketBasedLegacyTransport(channel, make_iwm_traits())
{
    // Create hardware abstraction
    _hardware = make_iwm_hardware();
}

bool IwmTransport::readCommandFrame(cmdFrame_t& frame) {
    // IWM uses phase-based protocol
    PhaseState phase = readPhases();
    
    if (phase == PhaseState::Reset) {
        // Reset: clear all device addresses
        // This will be handled by the transport state machine
        _currentPhase = PhaseState::Reset;
        return false; // No command frame during reset
    }
    
    if (phase == PhaseState::Enable) {
        // Enable: expect a command packet
        // TODO: Read SPI packet and decode into cmdFrame_t
        // For now, placeholder
        _currentPhase = PhaseState::Enable;
        return false;
    }
    
    _currentPhase = PhaseState::Idle;
    return false;
}

void IwmTransport::sendStatusPacket(std::uint8_t status) {
    // IWM sends status packets via SPI
    // Packet format: sync bytes + header + status + checksum
    // TODO: Implement SPI packet encoding and sending
    (void)status;
}

void IwmTransport::sendDataPacket(const std::uint8_t* buf, std::size_t len) {
    // IWM sends data packets via SPI
    // Data is encoded in groups of 7 bytes
    // TODO: Implement SPI packet encoding and sending
    (void)buf;
    (void)len;
}

std::size_t IwmTransport::readDataPacket(std::uint8_t* buf, std::size_t len) {
    // IWM reads data packets via SPI
    // TODO: Implement SPI packet reading and decoding
    (void)buf;
    (void)len;
    return 0;
}

bool IwmTransport::commandNeedsData(std::uint8_t command) const {
    // IWM commands that require a data packet
    switch (command) {
        case 0x02: // WRITEBLOCK
        case 0x42: // EWRITEBLOCK
        case 0x04: // CONTROL (may have data)
        case 0x44: // ECONTROL (may have data)
        case 0x09: // WRITE
        case 0x49: // EWRITE
            return true;
        default:
            return false;
    }
}

IwmTransport::PhaseState IwmTransport::readPhases() {
    // TODO: Read GPIO phase lines (PH0, PH1, PH2, PH3)
    // Phase pattern determines bus state:
    // - Reset: PH3=0, PH2=1, PH1=0, PH0=1 (0b0101)
    // - Enable: PH3=1, PH2=X, PH1=1, PH0=X (0b1X1X)
    // - Idle: other patterns
    return PhaseState::Idle;
}

void IwmTransport::handleInit() {
    // IWM INIT command assigns device IDs dynamically
    // This is handled by the bus during INIT sequence
    // TODO: Implement INIT handling
}

void IwmTransport::sendStatusPacket(std::uint8_t status) {
    // IWM sends status packets via SPI
    // Packet format: sync bytes + header + status + checksum
    // TODO: Implement SPI packet encoding and sending
    (void)status;
}

void IwmTransport::sendDataPacket(const std::uint8_t* buf, std::size_t len) {
    // IWM sends data packets via SPI
    // Data is encoded in groups of 7 bytes
    // TODO: Implement SPI packet encoding and sending
    (void)buf;
    (void)len;
}

} // namespace fujinet::io::transport::legacy
