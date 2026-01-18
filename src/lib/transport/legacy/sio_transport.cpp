#include "fujinet/io/transport/legacy/sio_transport.h"
#include "fujinet/io/transport/legacy/bus_traits.h"
#include "fujinet/io/transport/legacy/bus_hardware.h"
#include "fujinet/core/logging.h"

namespace fujinet::io::transport::legacy {

static constexpr const char* TAG = "sio";

// SIO timing constants
static constexpr std::uint32_t DELAY_T4 = 850; // microseconds
static constexpr std::uint32_t DELAY_T5 = 250; // microseconds

SioTransport::SioTransport(Channel& channel)
    : ByteBasedLegacyTransport(channel, make_sio_traits())
{
    // Create hardware abstraction
    // Note: For SIO, the Channel may not be used directly - hardware handles UART/GPIO
    _hardware = make_sio_hardware();
}

bool SioTransport::readCommandFrame(cmdFrame_t& frame) {
    // Wait for CMD pin to be asserted (platform-specific)
    if (!_hardware->commandAsserted()) {
        return false;
    }
    
    // Read 5-byte command frame
    std::size_t bytes_read = _hardware->read(reinterpret_cast<std::uint8_t*>(&frame), sizeof(frame));
    if (bytes_read != sizeof(frame)) {
        FN_LOGW(TAG, "Failed to read complete command frame: got %zu bytes, expected %zu",
            bytes_read, sizeof(frame));
        return false;
    }
    
    // Wait for CMD line to de-assert (platform-specific)
    // This is handled by the hardware abstraction
    
    FN_LOGD(TAG, "CF: %02x %02x %02x %02x %02x",
        frame.device, frame.comnd, frame.aux1, frame.aux2, frame.checksum);
    
    return true;
}

void SioTransport::sendAck() {
    _hardware->write('A');
    _hardware->flush();
    FN_LOGD(TAG, "ACK!");
}

void SioTransport::sendNak() {
    _hardware->write('N');
    _hardware->flush();
    FN_LOGD(TAG, "NAK!");
}

void SioTransport::sendComplete() {
    _hardware->delayMicroseconds(DELAY_T5);
    _hardware->write('C');
    _hardware->flush();
    FN_LOGD(TAG, "COMPLETE!");
}

void SioTransport::sendError() {
    _hardware->delayMicroseconds(DELAY_T5);
    _hardware->write('E');
    _hardware->flush();
    FN_LOGD(TAG, "ERROR!");
}

std::size_t SioTransport::readDataFrame(std::uint8_t* buf, std::size_t len) {
    return readDataFrameWithChecksum(buf, len);
}

void SioTransport::writeDataFrame(const std::uint8_t* buf, std::size_t len) {
    writeDataFrameWithChecksum(buf, len);
}

std::size_t SioTransport::readDataFrameWithChecksum(std::uint8_t* buf, std::size_t len) {
    // Read data frame
    std::size_t bytes_read = _hardware->read(buf, len);
    if (bytes_read != len) {
        FN_LOGW(TAG, "Failed to read complete data frame: got %zu bytes, expected %zu",
            bytes_read, len);
        sendNak();
        return 0;
    }
    
    // Wait for checksum byte
    while (_hardware->available() == 0) {
        // Yield or delay - platform-specific
        _hardware->delayMicroseconds(10);
    }
    
    std::uint8_t received_checksum = 0;
    if (_hardware->read(&received_checksum, 1) != 1) {
        FN_LOGW(TAG, "Failed to read checksum byte");
        sendNak();
        return 0;
    }
    
    // Calculate checksum
    std::uint8_t calculated_checksum = _traits.checksum(buf, len);
    
    // Delay T4 before responding
    _hardware->delayMicroseconds(DELAY_T4);
    
    if (calculated_checksum != received_checksum) {
        FN_LOGW(TAG, "Data frame checksum error: calc=0x%02X, recv=0x%02X",
            calculated_checksum, received_checksum);
        sendNak();
        return 0;
    }
    
    sendAck();
    return bytes_read;
}

void SioTransport::writeDataFrameWithChecksum(const std::uint8_t* buf, std::size_t len) {
    // Write data frame
    _hardware->write(buf, len);
    
    // Calculate and write checksum
    std::uint8_t checksum = _traits.checksum(buf, len);
    _hardware->write(checksum);
    
    _hardware->flush();
    
    FN_LOGD(TAG, "->SIO write %zu bytes (chksum=0x%02X)", len, checksum);
}

bool SioTransport::commandNeedsData(std::uint8_t command) const {
    // SIO commands that require a data frame
    switch (command) {
        case 'W': // Write sector
        case 'P': // Put sector
        case 'S': // Status (sometimes has data)
        case '!': // Format
            return true;
        default:
            return false;
    }
}

} // namespace fujinet::io::transport::legacy
