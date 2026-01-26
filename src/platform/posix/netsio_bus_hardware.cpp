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

static constexpr const char* TAG = "netsio";

// NetSIO bus hardware implementation
// Wraps UDP channel + NetSIO protocol to provide BusHardware interface
class NetSioBusHardware : public BusHardware {
public:
    NetSioBusHardware(fujinet::io::Channel& channel, const fujinet::config::NetSioConfig& config)
        : _channel(channel)
        , _netsio(std::make_unique<NetSIOProtocol>())
        , _commandAsserted(false)
        , _motorAsserted(false)
        , _syncRequestNum(0)
        , _commandFrameReady(false)
    {
        FN_LOGI(TAG, "NetSioBusHardware created (host=%s, port=%u)", 
                config.host.c_str(), config.port);
        
        // Send device connect message
        _netsio->sendDeviceConnect();
        const auto& msg = _netsio->getEncodedMessage();
        _channel.write(msg.data(), msg.size());
        _lastActivity = clock::now();
    }
    
    ~NetSioBusHardware() override {
        // Send device disconnect
        if (_netsio) {
            _netsio->sendDeviceDisconnect();
            const auto& msg = _netsio->getEncodedMessage();
            _channel.write(msg.data(), msg.size());
        }
    }
    
    void poll() override {
        // Process incoming NetSIO messages from UDP channel
        bool hasData = _channel.available();
        if (hasData) {
            FN_LOGI(TAG, "poll(): UDP data available, processing messages");
        }
        processNetsioMessages();
        sendKeepAliveIfIdle();
    }
    
    bool commandAsserted() const override {
        // For NetSIO, also return true if we have a complete command frame ready
        // (even though COMMAND is de-asserted, the frame is available)
        return _commandAsserted || _commandFrameReady;
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
        _lastActivity = clock::now();
    }
    
    std::size_t read(std::uint8_t* buf, std::size_t len) override {
        processNetsioMessages();
        
        // Read from FIFO (for NetSIO, data and checksum come as separate messages)
        std::size_t toRead = (_netsioFifo.size() < len) ? _netsioFifo.size() : len;
        if (toRead == 0) {
            return 0;
        }
        
        FN_LOGI(TAG, "read(buf, len=%zu): Reading %zu bytes from FIFO (FIFO had %zu bytes)", 
                len, toRead, _netsioFifo.size());
        std::memcpy(buf, _netsioFifo.data(), toRead);
        _netsioFifo.erase(_netsioFifo.begin(), _netsioFifo.begin() + toRead);
        
        // Clear command frame ready flag after reading (we've consumed the frame)
        if (_commandFrameReady && toRead >= 5) {
            _commandFrameReady = false;
        }
        
        FN_LOGI(TAG, "read(): FIFO now has %zu bytes remaining", _netsioFifo.size());
        return toRead;
    }
    
    void write(const std::uint8_t* buf, std::size_t len) override {
        // NetSIO: send DATA_BLOCK
        FN_LOGI(TAG, "write(buf, len=%zu) - sending DATA_BLOCK", len);
        _netsio->sendDataBlock(buf, len);
        const auto& msg = _netsio->getEncodedMessage();
        _channel.write(msg.data(), msg.size());
        _lastActivity = clock::now();
    }
    
    void write(std::uint8_t byte) override {
        // NetSIO: send DATA_BYTE
        FN_LOGI(TAG, "write(byte=0x%02X) - sending DATA_BYTE", byte);
        _netsio->sendDataByte(byte);
        const auto& msg = _netsio->getEncodedMessage();
        _channel.write(msg.data(), msg.size());
        _lastActivity = clock::now();
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
        
        FN_LOGI(TAG, "sendSyncResponseIfNeeded: sync=%u, ackByte=0x%02X, writeSize=%u", 
                _syncRequestNum, ackByte, writeSize);
        std::uint8_t ackType = (ackByte == 'A' || ackByte == 'N') ? netsio::ACK_SYNC : netsio::EMPTY_SYNC;
        _netsio->sendSyncResponse(_syncRequestNum, ackType, ackByte, writeSize);
        const auto& msg = _netsio->getEncodedMessage();
        _channel.write(msg.data(), msg.size());
        _lastActivity = clock::now();
        
        _syncRequestNum = 0; // Clear after sending
        return true;
    }

private:
    using clock = std::chrono::steady_clock;
    static constexpr auto keepalive_interval = std::chrono::seconds(10);

    void sendKeepAliveIfIdle()
    {
        // NetSIO HUB expires UDP "devices" if it receives no packets for ~30s.
        // Keep this logic in the NetSIO bus layer (legacy transport only), not core.
        const auto now = clock::now();
        if ((now - _lastActivity) < keepalive_interval) {
            return;
        }

        _netsio->sendAliveRequest();
        const auto& msg = _netsio->getEncodedMessage();
        _channel.write(msg.data(), msg.size());
        _lastActivity = now;
        FN_LOGD(TAG, "Sent ALIVE_REQUEST keepalive");
    }

    void processNetsioMessages() {
        // Read UDP datagrams and parse NetSIO messages
        std::uint8_t buffer[512];
        int packetCount = 0;
        while (_channel.available()) {
            std::size_t len = _channel.read(buffer, sizeof(buffer));
            if (len == 0) {
                break;
            }

            _lastActivity = clock::now();
            
            packetCount++;
            FN_LOGI(TAG, "processNetsioMessages(): Received UDP packet #%d, %zu bytes, msgType=0x%02X", 
                    packetCount, len, len > 0 ? buffer[0] : 0);
            
            bool parsed = _netsio->parseMessage(buffer, len);
            if (!parsed) {
                FN_LOGW(TAG, "processNetsioMessages(): Failed to parse message (len=%zu, first_byte=0x%02X)", 
                        len, len > 0 ? buffer[0] : 0);
                continue;
            }
            
            std::uint8_t msgType = _netsio->getMessageType();
            const auto& payload = _netsio->getPayload();
            FN_LOGI(TAG, "processNetsioMessages(): Parsed message type=0x%02X, payload_size=%zu", 
                    msgType, payload.size());
                
                switch (msgType) {
                case netsio::COMMAND_ON:
                    _commandAsserted = true;
                    _commandFrameReady = false;
                    _netsioFifo.clear();
                    _syncRequestNum = 0;
                    FN_LOGI(TAG, "COMMAND_ON - command asserted, FIFO cleared");
                    break;
                    
                case netsio::COMMAND_OFF:
                    _commandAsserted = false;
                    _commandFrameReady = false;
                    FN_LOGI(TAG, "COMMAND_OFF - command de-asserted");
                    break;
                    
                case netsio::COMMAND_OFF_SYNC:
                    FN_LOGI(TAG, "COMMAND_OFF_SYNC received, payload_size=%zu, FIFO_size=%zu", payload.size(), _netsioFifo.size());
                    _commandAsserted = false;
                    if (payload.size() >= 1) {
                        _syncRequestNum = payload[0];
                    }
                    // For NetSIO, command frame is complete when COMMAND_OFF_SYNC is received
                    // and we have 5 bytes in the FIFO (the command frame)
                    // Note: DATA_BLOCK may include extra bytes (sequence numbers), so check for >= 5
                    if (_netsioFifo.size() >= 5) {
                        _commandFrameReady = true;
                        FN_LOGI(TAG, "COMMAND_OFF_SYNC - command frame ready (%zu bytes in FIFO), sync=%u", _netsioFifo.size(), _syncRequestNum);
                    } else {
                        FN_LOGW(TAG, "COMMAND_OFF_SYNC - FIFO has %zu bytes, expected at least 5", _netsioFifo.size());
                    }
                    break;
                    
                case netsio::DATA_BYTE:
                    if (payload.size() >= 1) {
                        _netsioFifo.push_back(payload[0]);
                        FN_LOGI(TAG, "DATA_BYTE: 0x%02X added to FIFO (FIFO size now %zu)", 
                                payload[0], _netsioFifo.size());
                    }
                    break;
                    
                case netsio::DATA_BLOCK:
                    _netsioFifo.insert(_netsioFifo.end(), payload.begin(), payload.end());
                    FN_LOGI(TAG, "DATA_BLOCK: %zu bytes added to FIFO (FIFO size now %zu)", 
                            payload.size(), _netsioFifo.size());
                    break;
                    
                case netsio::DATA_BYTE_SYNC:
                    if (payload.size() >= 2) {
                        _netsioFifo.push_back(payload[0]);
                        _syncRequestNum = payload[1];
                        FN_LOGI(TAG, "DATA_BYTE_SYNC: byte=0x%02X sync=%u added to FIFO (FIFO size now %zu)", 
                                payload[0], payload[1], _netsioFifo.size());
                    }
                    break;
                    
                case netsio::MOTOR_ON:
                    _motorAsserted = true;
                    FN_LOGI(TAG, "MOTOR_ON");
                    break;
                    
                case netsio::MOTOR_OFF:
                    _motorAsserted = false;
                    FN_LOGI(TAG, "MOTOR_OFF");
                    break;
                    
                case netsio::PING_REQUEST:
                    FN_LOGI(TAG, "PING_REQUEST - sending PING_RESPONSE");
                    _netsio->sendPingResponse();
                    {
                        const auto& msg = _netsio->getEncodedMessage();
                        _channel.write(msg.data(), msg.size());
                        _lastActivity = clock::now();
                    }
                    break;
                    
                case netsio::ALIVE_REQUEST:
                    FN_LOGI(TAG, "ALIVE_REQUEST - sending ALIVE_RESPONSE");
                    _netsio->sendAliveResponse();
                    {
                        const auto& msg = _netsio->getEncodedMessage();
                        _channel.write(msg.data(), msg.size());
                        _lastActivity = clock::now();
                    }
                    break;
                    
                case netsio::CREDIT_UPDATE:
                    FN_LOGI(TAG, "CREDIT_UPDATE received (handled by protocol parser)");
                    break;
                    
                case netsio::CREDIT_STATUS:
                    FN_LOGI(TAG, "CREDIT_STATUS received (handled by protocol parser)");
                    break;
                    
                case netsio::WARM_RESET:
                    FN_LOGI(TAG, "WARM_RESET received");
                    break;
                    
                case netsio::COLD_RESET:
                    FN_LOGI(TAG, "COLD_RESET received");
                    break;
                    
                case netsio::BUS_IDLE:
                    FN_LOGI(TAG, "BUS_IDLE received");
                    break;
                    
                case netsio::SPEED_CHANGE:
                    FN_LOGI(TAG, "SPEED_CHANGE received (handled by protocol parser)");
                    break;
                    
                case netsio::PROCEED_ON:
                    FN_LOGI(TAG, "PROCEED_ON received");
                    break;
                    
                case netsio::PROCEED_OFF:
                    FN_LOGI(TAG, "PROCEED_OFF received");
                    break;
                    
                case netsio::INTERRUPT_ON:
                    FN_LOGI(TAG, "INTERRUPT_ON received");
                    break;
                    
                case netsio::INTERRUPT_OFF:
                    FN_LOGI(TAG, "INTERRUPT_OFF received");
                    break;
                    
                default:
                    FN_LOGW(TAG, "Unknown/unhandled message type: 0x%02X", msgType);
                    break;
                }
        }
        
        if (packetCount > 0) {
            FN_LOGI(TAG, "processNetsioMessages(): Processed %d packets in this call", packetCount);
        }
    }
    
    fujinet::io::Channel& _channel;
    std::unique_ptr<NetSIOProtocol> _netsio;
    
            // NetSIO FIFO buffer (accumulates bytes from NetSIO messages)
            std::vector<std::uint8_t> _netsioFifo;
            
            // SIO signal state
            bool _commandAsserted;
            bool _motorAsserted;
            std::uint8_t _syncRequestNum{0};
            
            // NetSIO-specific: track when a complete command frame is ready
            // (for NetSIO, command frame arrives as DATA_BLOCK after COMMAND_ON,
            //  and is complete when COMMAND_OFF_SYNC is received with 5 bytes in FIFO)
            bool _commandFrameReady{false};

            // Track most recent RX/TX activity to avoid NetSIO HUB idle expiry.
            clock::time_point _lastActivity{clock::now()};
};

// Factory function
std::unique_ptr<BusHardware> make_netsio_bus_hardware(fujinet::io::Channel& channel, const fujinet::config::NetSioConfig& netsioConfig) {
    return std::make_unique<NetSioBusHardware>(channel, netsioConfig);
}

} // namespace fujinet::io::transport::legacy
