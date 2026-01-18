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
// Handles common protocol logic while allowing platform-specific hardware access
class LegacyTransport : public ITransport {
public:
    explicit LegacyTransport(
        Channel& channel,
        const BusTraits& traits
    );
    
    virtual ~LegacyTransport() = default;
    
    void poll() override;
    bool receive(IORequest& outReq) override;
    void send(const IOResponse& resp) override;
    
protected:
    // Platform-specific implementations must provide these methods
    
    // Read a complete command frame from hardware
    // Returns true if a valid frame was read, false otherwise
    virtual bool readCommandFrame(cmdFrame_t& frame) = 0;
    
    // Send protocol control bytes
    virtual void sendAck() = 0;
    virtual void sendNak() = 0;
    virtual void sendComplete() = 0;
    virtual void sendError() = 0;
    
    // Read/write data frames (after command frame)
    // Returns number of bytes read/written
    virtual std::size_t readDataFrame(std::uint8_t* buf, std::size_t len) = 0;
    virtual void writeDataFrame(const std::uint8_t* buf, std::size_t len) = 0;
    
    // Check if command requires a data frame
    virtual bool commandNeedsData(std::uint8_t command) const;
    
protected:
    Channel& _channel;
    const BusTraits& _traits;
    
private:
    // Convert cmdFrame_t to IORequest
    IORequest convertToIORequest(const cmdFrame_t& frame);
    
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
