#pragma once

#include "fujinet/io/transport/legacy/legacy_transport.h"

#include <memory>

namespace fujinet::io::transport::legacy {

// Forward declaration
class LegacyNetworkBridge;

// Base class for byte-based legacy transports (SIO, IEC, etc.)
// These protocols use control bytes (ACK/NAK/COMPLETE/ERROR) for flow control
class ByteBasedLegacyTransport : public LegacyTransport {
public:
    explicit ByteBasedLegacyTransport(
        Channel& channel,
        const BusTraits& traits
    );
    
    ~ByteBasedLegacyTransport() override;
    
    bool receive(IORequest& outReq) override;
    void send(const IOResponse& resp) override;
    
protected:
    // Byte-based protocols must implement these control byte methods
    virtual void sendAck() = 0;
    virtual void sendNak() = 0;
    virtual void sendComplete() = 0;
    virtual void sendError() = 0;
    
    // Read/write data frames with checksum validation
    virtual std::size_t readDataFrame(std::uint8_t* buf, std::size_t len) = 0;
    virtual void writeDataFrame(const std::uint8_t* buf, std::size_t len) = 0;

private:
    // Internal bridge for converting legacy network device IDs (0x71-0x78) to NetworkService (0xFD)
    // This is transport-internal and never exposed to core services
    std::unique_ptr<LegacyNetworkBridge> _networkBridge;
};

} // namespace fujinet::io::transport::legacy
