#pragma once

#include "fujinet/io/transport/legacy/legacy_transport.h"

namespace fujinet::io::transport::legacy {

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

    // Determine expected data-frame length for a command that requires a data phase.
    //
    // IMPORTANT: byte-based buses typically require exact-length reads for data frames;
    // reading "up to N" can wedge the protocol if the host sends fewer bytes.
    //
    // Default is conservative (256) to match common legacy fixed-frame usage (e.g. devicespec buffers).
    virtual std::size_t expectedDataFrameLength(const cmdFrame_t& frame) const;
};

} // namespace fujinet::io::transport::legacy
