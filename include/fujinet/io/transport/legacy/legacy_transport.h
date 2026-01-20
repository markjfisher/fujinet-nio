#pragma once

#include <vector>
#include <cstdint>

#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/transport/transport.h"
#include "fujinet/io/transport/legacy/bus_traits.h"
#include "fujinet/io/transport/legacy/cmd_frame.h"

namespace fujinet::io::transport::legacy {

// Base class for legacy bus transports (SIO, IWM, IEC, etc.)
// Provides common functionality: polling, frame conversion, state management
// Protocol-specific behavior is handled by derived classes:
// - ByteBasedLegacyTransport (SIO, IEC) - uses control bytes
// - PacketBasedLegacyTransport (IWM) - uses packet-based protocol
class LegacyTransport : public ITransport {
public:
    explicit LegacyTransport(
        Channel& channel,
        const BusTraits& traits  // Accept by reference, but store by value
    );
    
    virtual ~LegacyTransport() = default;
    
    void poll() override;
    
    // Derived classes implement receive() and send() based on their protocol style
    virtual bool receive(IORequest& outReq) = 0;
    virtual void send(const IOResponse& resp) = 0;
    
protected:
    // Platform-specific implementations must provide these methods
    
    // Read a complete command frame from hardware
    // Returns true if a valid frame was read, false otherwise
    virtual bool readCommandFrame(cmdFrame_t& frame) = 0;
    
    // Check if command requires a data frame
    virtual bool commandNeedsData(std::uint8_t command) const;
    
    // Convert cmdFrame_t to IORequest (shared by all protocols)
    IORequest convertToIORequest(const cmdFrame_t& frame);
    
protected:
    Channel& _channel;
    BusTraits _traits;  // Store by value, not reference, to avoid dangling reference
    
    // State machine for legacy protocol
    enum class State {
        WaitingForCommand,
        WaitingForData,
        SendingResponse,
    };
    
    State _state{State::WaitingForCommand};
    std::vector<std::uint8_t> _rxBuffer;
    RequestID _nextRequestId{1};
    cmdFrame_t _pendingFrame{};
};

} // namespace fujinet::io::transport::legacy
