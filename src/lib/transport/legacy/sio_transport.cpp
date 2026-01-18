#include "fujinet/io/transport/legacy/sio_transport.h"
#include "fujinet/io/transport/legacy/bus_traits.h"
#include "fujinet/io/transport/legacy/bus_hardware.h"
#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config.h"
#include "fujinet/core/logging.h"

#include <cstring>

namespace fujinet::io::transport::legacy {

static constexpr const char* TAG = "sio";

// SIO timing constants
static constexpr std::uint32_t DELAY_T4 = 850; // microseconds
static constexpr std::uint32_t DELAY_T5 = 250; // microseconds

void SioTransport::poll() {
    // For NetSIO, we need to poll the hardware abstraction first to process
    // incoming UDP packets and parse NetSIO protocol messages.
    // The hardware abstraction will populate its internal FIFO with SIO bytes.
    // For hardware SIO, this is a no-op or triggers internal UART/GPIO reads.
    _hardware->poll();
    
    // Don't call base class poll() - it reads raw bytes from channel,
    // but for NetSIO the channel is UDP and bytes are already processed by hardware.
    // For hardware SIO, we might want base class poll, but hardware handles it.
}

SioTransport::SioTransport(Channel& channel, 
                           const build::BuildProfile& profile,
                           const config::NetSioConfig* netsioConfig)
    : ByteBasedLegacyTransport(channel, make_sio_traits())
{
    // Create hardware abstraction - factory decides based on channel type and config
    bool useNetsio = (profile.primaryChannel == build::ChannelKind::UdpSocket);
    if (useNetsio && netsioConfig) {
        _hardware = make_sio_hardware(&channel, netsioConfig);
        FN_LOGI(TAG, "SIO Transport initialized in NetSIO mode (UDP)");
    } else {
        _hardware = make_sio_hardware(&channel, nullptr);
        FN_LOGI(TAG, "SIO Transport initialized in hardware mode");
    }
}

bool SioTransport::readCommandFrame(cmdFrame_t& frame) {
    // Poll hardware to process incoming messages (critical for NetSIO)
    _hardware->poll();
    
    // Wait for CMD pin to be asserted (works for both hardware and NetSIO)
    bool cmdAsserted = _hardware->commandAsserted();
    FN_LOGI(TAG, "readCommandFrame(): commandAsserted=%d", cmdAsserted ? 1 : 0);
    if (!cmdAsserted) {
        return false;
    }
    
    // Read 5-byte command frame (BusHardware handles NetSIO vs hardware differences)
    std::size_t bytes_read = _hardware->read(reinterpret_cast<std::uint8_t*>(&frame), sizeof(frame));
    FN_LOGI(TAG, "readCommandFrame(): read %zu bytes (expected %zu)", bytes_read, sizeof(frame));
    if (bytes_read != sizeof(frame)) {
        FN_LOGW(TAG, "Failed to read complete command frame: got %zu bytes, expected %zu",
            bytes_read, sizeof(frame));
        return false;
    }
    
    FN_LOGI(TAG, "readCommandFrame(): CF: %02x %02x %02x %02x %02x",
        frame.device, frame.comnd, frame.aux1, frame.aux2, frame.checksum);
    
    return true;
}

void SioTransport::sendAck() {
    // Try sync response first (NetSIO mode), fall back to regular ACK
    if (!_hardware->sendSyncResponseIfNeeded('A')) {
        _hardware->write('A');
        _hardware->flush();
    }
    FN_LOGD(TAG, "ACK!");
}

void SioTransport::sendNak() {
    // Try sync response first (NetSIO mode), fall back to regular NAK
    if (!_hardware->sendSyncResponseIfNeeded('N')) {
        _hardware->write('N');
        _hardware->flush();
    }
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
    // Read data frame (BusHardware handles NetSIO vs hardware differences)
    std::size_t bytes_read = _hardware->read(buf, len);
    if (bytes_read != len) {
        FN_LOGW(TAG, "Failed to read complete data frame: got %zu bytes, expected %zu",
            bytes_read, len);
        sendNak();
        return 0;
    }
    
    // Wait for checksum byte
    while (_hardware->available() == 0) {
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
    // Write data frame (BusHardware handles NetSIO vs hardware differences)
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
