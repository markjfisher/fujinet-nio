#pragma once

#include "fujinet/io/transport/legacy/legacy_transport.h"

namespace fujinet::io::transport::legacy {

// Base class for packet-based legacy transports (IWM, etc.)
// These protocols use packet-based communication (status packets, data packets)
// No control bytes like ACK/NAK/COMPLETE/ERROR
class PacketBasedLegacyTransport : public LegacyTransport {
public:
    explicit PacketBasedLegacyTransport(
        Channel& channel,
        const BusTraits& traits
    );
    
    virtual ~PacketBasedLegacyTransport() = default;
    
    bool receive(IORequest& outReq) override;
    void send(const IOResponse& resp) override;
    
protected:
    // Packet-based protocols must implement these packet methods
    virtual void sendStatusPacket(std::uint8_t status) = 0;
    virtual void sendDataPacket(const std::uint8_t* buf, std::size_t len) = 0;
    
    // Read data packet (decoded from protocol-specific encoding)
    virtual std::size_t readDataPacket(std::uint8_t* buf, std::size_t len) = 0;
};

} // namespace fujinet::io::transport::legacy
