#include "fujinet/io/transport/legacy/bus_hardware.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/transport/legacy/netsio_protocol.h"
#include "fujinet/config/fuji_config.h"
#include "fujinet/core/logging.h"

#include <cstring>
#include <memory>
#include <thread>
#include <chrono>

namespace fujinet::io::transport::legacy {

static constexpr const char* TAG = "netsio_hw";

// NetSIO bus hardware implementation
// Wraps UDP channel + NetSIO protocol to provide BusHardware interface
class NetSioBusHardware : public BusHardware {
public:
    NetSioBusHardware(Channel& channel, const config::NetSioConfig& config)
        : _channel(channel)
        , _netsio(std::make_unique<NetSIOProtocol>())
        , _commandAsserted(false)
        , _motorAsserted(false)
    {
        FN_LOGI(TAG, "NetSioBusHardware created (host=%s, port=%u)", 
                config.host.c_str(), config.port);
        
        // Send device connect message
        _netsio->sendDeviceConnect();
        const auto& msg = _netsio->getEncodedMessage();
        _channel.write(msg.data(), msg.size());
    }
    
    ~NetSioBusHardware() override {
        // Send device disconnect
        if (_netsio) {
            _netsio->sendDeviceDisconnect();
            const auto& msg = _netsio->getEncodedMessage();
            _channel.write(msg.data(), msg.size());
        }
    }
    
    bool commandAsserted() const override {
        return _commandAsserted;
    }
    
    bool motorAsserted() const override {
        return _motorAsserted;
    }
    
    void setInterrupt(bool level) override {
        // NetSIO: send INTERRUPT_ON/OFF message
        if (level) {
            _netsio->sendInterruptOn();
        } else {
            _netsio->sendInterruptOff();
        }
        const auto& msg = _netsio->getEncodedMessage();
        _channel.write(msg.data(), msg.size());
    }
    
    std::size_t read(std::uint8_t* buf, std::size_t len) override {
        processNetsioMessages();
        
        // Read from FIFO (for NetSIO, data and checksum come as separate messages)
        std::size_t toRead = (_netsioFifo.size() < len) ? _netsioFifo.size() : len;
        if (toRead == 0) {
            return 0;
        }
        
        std::memcpy(buf, _netsioFifo.data(), toRead);
        _netsioFifo.erase(_netsioFifo.begin(), _netsioFifo.begin() + toRead);
        return toRead;
    }
    
    void write(const std::uint8_t* buf, std::size_t len) override {
        // NetSIO: send DATA_BLOCK
        _netsio->sendDataBlock(buf, len);
        const auto& msg = _netsio->getEncodedMessage();
        _channel.write(msg.data(), msg.size());
    }
    
    void write(std::uint8_t byte) override {
        // NetSIO: send DATA_BYTE
        _netsio->sendDataByte(byte);
        const auto& msg = _netsio->getEncodedMessage();
        _channel.write(msg.data(), msg.size());
    }
    
    void flush() override {
        // UDP is packet-based, no flush needed
    }
    
    std::size_t available() const override {
        // Non-const because processNetsioMessages modifies state
        const_cast<NetSioBusHardware*>(this)->processNetsioMessages();
        return _netsioFifo.size();
    }
    
    void discardInput() override {
        _netsioFifo.clear();
    }
    
    void delayMicroseconds(std::uint32_t us) override {
        // NetSIO doesn't need precise timing (network latency dominates)
        // Use std::this_thread::sleep_for for approximate delay
        std::this_thread::sleep_for(std::chrono::microseconds(us));
    }
    
    bool sendSyncResponseIfNeeded(std::uint8_t ackByte, std::uint16_t writeSize = 0) override {
        if (_syncRequestNum == 0) {
            return false; // No sync requested
        }
        
        std::uint8_t ackType = (ackByte == 'A' || ackByte == 'N') ? netsio::ACK_SYNC : netsio::EMPTY_SYNC;
        _netsio->sendSyncResponse(_syncRequestNum, ackType, ackByte, writeSize);
        const auto& msg = _netsio->getEncodedMessage();
        _channel.write(msg.data(), msg.size());
        
        _syncRequestNum = 0; // Clear after sending
        return true;
    }

private:
    void processNetsioMessages() {
        // Read UDP datagrams and parse NetSIO messages
        std::uint8_t buffer[512];
        while (_channel.available()) {
            std::size_t len = _channel.read(buffer, sizeof(buffer));
            if (len == 0) {
                break;
            }
            
            if (_netsio->parseMessage(buffer, len)) {
                std::uint8_t msgType = _netsio->getMessageType();
                const auto& payload = _netsio->getPayload();
                
                switch (msgType) {
                case netsio::COMMAND_ON:
                    _commandAsserted = true;
                    _netsioFifo.clear();
                    _syncRequestNum = 0;
                    FN_LOGD(TAG, "COMMAND_ON");
                    break;
                    
                case netsio::COMMAND_OFF:
                    _commandAsserted = false;
                    FN_LOGD(TAG, "COMMAND_OFF");
                    break;
                    
                case netsio::COMMAND_OFF_SYNC:
                    _commandAsserted = false;
                    if (payload.size() >= 1) {
                        _syncRequestNum = payload[0];
                    }
                    FN_LOGD(TAG, "COMMAND_OFF_SYNC (sync=%u)", _syncRequestNum);
                    break;
                    
                case netsio::DATA_BYTE:
                    if (payload.size() >= 1) {
                        _netsioFifo.push_back(payload[0]);
                    }
                    break;
                    
                case netsio::DATA_BLOCK:
                    _netsioFifo.insert(_netsioFifo.end(), payload.begin(), payload.end());
                    FN_LOGD(TAG, "DATA_BLOCK (%zu bytes)", payload.size());
                    break;
                    
                case netsio::DATA_BYTE_SYNC:
                    if (payload.size() >= 2) {
                        _netsioFifo.push_back(payload[0]);
                        _syncRequestNum = payload[1];
                        FN_LOGD(TAG, "DATA_BYTE_SYNC (byte=0x%02X sync=%u)", payload[0], payload[1]);
                    }
                    break;
                    
                case netsio::MOTOR_ON:
                    _motorAsserted = true;
                    break;
                    
                case netsio::MOTOR_OFF:
                    _motorAsserted = false;
                    break;
                    
                case netsio::PING_REQUEST:
                    _netsio->sendPingResponse();
                    {
                        const auto& msg = _netsio->getEncodedMessage();
                        _channel.write(msg.data(), msg.size());
                    }
                    break;
                    
                case netsio::ALIVE_REQUEST:
                    _netsio->sendAliveResponse();
                    {
                        const auto& msg = _netsio->getEncodedMessage();
                        _channel.write(msg.data(), msg.size());
                    }
                    break;
                    
                default:
                    // Other messages (PROCEED, INTERRUPT, SPEED_CHANGE, etc.)
                    break;
                }
            }
        }
    }
    
    Channel& _channel;
    std::unique_ptr<NetSIOProtocol> _netsio;
    
    // NetSIO FIFO buffer (accumulates bytes from NetSIO messages)
    std::vector<std::uint8_t> _netsioFifo;
    
    // SIO signal state
    bool _commandAsserted;
    bool _motorAsserted;
    std::uint8_t _syncRequestNum{0};
};

// Factory function
std::unique_ptr<BusHardware> make_netsio_bus_hardware(Channel& channel, const config::NetSioConfig& config) {
    return std::make_unique<NetSioBusHardware>(channel, config);
}

} // namespace fujinet::io::transport::legacy
