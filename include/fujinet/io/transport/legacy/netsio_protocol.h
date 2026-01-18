#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace fujinet::io::transport::legacy {

// NetSIO protocol message types (from netsio.py)
namespace netsio {
    constexpr std::uint8_t DATA_BYTE        = 0x01;
    constexpr std::uint8_t DATA_BLOCK       = 0x02;
    constexpr std::uint8_t DATA_BYTE_SYNC   = 0x09;
    constexpr std::uint8_t COMMAND_OFF      = 0x10;
    constexpr std::uint8_t COMMAND_ON       = 0x11;
    constexpr std::uint8_t COMMAND_OFF_SYNC = 0x18;
    constexpr std::uint8_t MOTOR_OFF        = 0x20;
    constexpr std::uint8_t MOTOR_ON         = 0x21;
    constexpr std::uint8_t PROCEED_OFF      = 0x30;
    constexpr std::uint8_t PROCEED_ON       = 0x31;
    constexpr std::uint8_t INTERRUPT_OFF    = 0x40;
    constexpr std::uint8_t INTERRUPT_ON     = 0x41;
    constexpr std::uint8_t SPEED_CHANGE     = 0x80;
    constexpr std::uint8_t SYNC_RESPONSE    = 0x81;
    constexpr std::uint8_t BUS_IDLE         = 0x88;
    constexpr std::uint8_t DEVICE_DISCONNECT = 0xC0;
    constexpr std::uint8_t DEVICE_CONNECT   = 0xC1;
    constexpr std::uint8_t PING_REQUEST     = 0xC2;
    constexpr std::uint8_t PING_RESPONSE    = 0xC3;
    constexpr std::uint8_t ALIVE_REQUEST    = 0xC4;
    constexpr std::uint8_t ALIVE_RESPONSE   = 0xC5;
    constexpr std::uint8_t CREDIT_STATUS    = 0xC6;
    constexpr std::uint8_t CREDIT_UPDATE    = 0xC7;
    constexpr std::uint8_t WARM_RESET       = 0xFE;
    constexpr std::uint8_t COLD_RESET       = 0xFF;

    // SYNC_RESPONSE ack types
    constexpr std::uint8_t EMPTY_SYNC       = 0x00;
    constexpr std::uint8_t ACK_SYNC         = 0x01;
}

// NetSIO protocol message parser/encoder
// Handles encoding/decoding NetSIO protocol messages over UDP
class NetSIOProtocol {
public:
    NetSIOProtocol();
    ~NetSIOProtocol() = default;

    // Process incoming UDP datagram and extract NetSIO messages
    // Returns true if a complete message was parsed
    bool parseMessage(const std::uint8_t* data, std::size_t len);

    // Get the last parsed message type
    std::uint8_t getMessageType() const { return _lastMessageType; }

    // Get message payload (for DATA_BYTE, DATA_BLOCK, etc.)
    const std::vector<std::uint8_t>& getPayload() const { return _lastPayload; }

    // Encode and send messages
    void sendDataByte(std::uint8_t byte);
    void sendDataBlock(const std::uint8_t* data, std::size_t len);
    void sendCommandOn();
    void sendCommandOff();
    void sendMotorOn();
    void sendMotorOff();
    void sendProceedOn();
    void sendProceedOff();
    void sendInterruptOn();
    void sendInterruptOff();
    void sendSyncResponse(std::uint8_t syncNum, std::uint8_t ackType, std::uint8_t ackByte, std::uint16_t writeSize);
    void sendDeviceConnect();
    void sendDeviceDisconnect();
    void sendPingRequest();
    void sendPingResponse();
    void sendAliveRequest();
    void sendAliveResponse();
    void sendCreditStatus(std::uint8_t credit);
    void sendCreditUpdate(std::uint8_t credit);

    // Get encoded message for sending (caller writes to UDP socket)
    const std::vector<std::uint8_t>& getEncodedMessage() const { return _txBuffer; }

    // State tracking
    bool commandAsserted() const { return _commandAsserted; }
    bool motorAsserted() const { return _motorAsserted; }
    std::uint32_t getBaudrate() const { return _baudrate; }

private:
    std::uint8_t _lastMessageType{0};
    std::vector<std::uint8_t> _lastPayload;
    std::vector<std::uint8_t> _txBuffer;

    // SIO signal state
    bool _commandAsserted{false};
    bool _motorAsserted{false};
    std::uint32_t _baudrate{19200};

    // Connection management
    std::uint8_t _syncRequestNum{0};
    std::uint8_t _credit{3};
};

} // namespace fujinet::io::transport::legacy
