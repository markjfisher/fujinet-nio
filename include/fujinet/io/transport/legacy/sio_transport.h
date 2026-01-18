#pragma once

#include <memory>

#include "fujinet/io/core/channel.h"
#include "fujinet/io/transport/legacy/byte_based_legacy_transport.h"
#include "fujinet/io/transport/legacy/bus_hardware.h"

namespace fujinet::io::transport::legacy {

// Atari SIO (Serial Input/Output) transport implementation
// Byte-based protocol using ACK/NAK/COMPLETE/ERROR control bytes
class SioTransport : public ByteBasedLegacyTransport {
public:
    explicit SioTransport(Channel& channel);
    virtual ~SioTransport() = default;
    
protected:
    bool readCommandFrame(cmdFrame_t& frame) override;
    void sendAck() override;
    void sendNak() override;
    void sendComplete() override;
    void sendError() override;
    std::size_t readDataFrame(std::uint8_t* buf, std::size_t len) override;
    void writeDataFrame(const std::uint8_t* buf, std::size_t len) override;
    bool commandNeedsData(std::uint8_t command) const override;
    
private:
    std::unique_ptr<BusHardware> _hardware;
    
    // SIO-specific: read data frame with checksum validation
    std::size_t readDataFrameWithChecksum(std::uint8_t* buf, std::size_t len);
    
    // SIO-specific: write data frame with checksum
    void writeDataFrameWithChecksum(const std::uint8_t* buf, std::size_t len);
};

} // namespace fujinet::io::transport::legacy
