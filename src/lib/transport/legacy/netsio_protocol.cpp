#include "fujinet/io/transport/legacy/netsio_protocol.h"
#include "fujinet/core/logging.h"

namespace fujinet::io::transport::legacy {

static constexpr const char* TAG = "netsio";

NetSIOProtocol::NetSIOProtocol()
{
    FN_LOGI(TAG, "NetSIO protocol initialized");
}

bool NetSIOProtocol::parseMessage(const std::uint8_t* data, std::size_t len)
{
    if (len == 0) {
        return false;
    }

    _lastMessageType = data[0];
    _lastPayload.clear();

    switch (_lastMessageType) {
    case netsio::DATA_BYTE:
        if (len >= 2) {
            _lastPayload.push_back(data[1]);
            FN_LOGD(TAG, "DATA_BYTE: 0x%02X", data[1]);
            return true;
        }
        break;

    case netsio::DATA_BLOCK:
        if (len >= 2) {
            _lastPayload.assign(data + 1, data + len);
            FN_LOGD(TAG, "DATA_BLOCK: %zu bytes", _lastPayload.size());
            return true;
        }
        break;

    case netsio::DATA_BYTE_SYNC:
        if (len >= 3) {
            _lastPayload.push_back(data[1]); // data byte
            _lastPayload.push_back(data[2]); // sync number
            FN_LOGD(TAG, "DATA_BYTE_SYNC: byte=0x%02X sync=%u", data[1], data[2]);
            return true;
        }
        break;

    case netsio::COMMAND_ON:
        _commandAsserted = true;
        FN_LOGD(TAG, "COMMAND_ON");
        return true;

    case netsio::COMMAND_OFF:
        _commandAsserted = false;
        FN_LOGD(TAG, "COMMAND_OFF");
        return true;

    case netsio::COMMAND_OFF_SYNC:
        if (len >= 2) {
            _commandAsserted = false;
            _lastPayload.push_back(data[1]); // sync number
            FN_LOGD(TAG, "COMMAND_OFF_SYNC: sync=%u", data[1]);
            return true;
        }
        break;

    case netsio::MOTOR_ON:
        _motorAsserted = true;
        FN_LOGD(TAG, "MOTOR_ON");
        return true;

    case netsio::MOTOR_OFF:
        _motorAsserted = false;
        FN_LOGD(TAG, "MOTOR_OFF");
        return true;

    case netsio::PROCEED_ON:
        FN_LOGD(TAG, "PROCEED_ON");
        return true;

    case netsio::PROCEED_OFF:
        FN_LOGD(TAG, "PROCEED_OFF");
        return true;

    case netsio::INTERRUPT_ON:
        FN_LOGD(TAG, "INTERRUPT_ON");
        return true;

    case netsio::INTERRUPT_OFF:
        FN_LOGD(TAG, "INTERRUPT_OFF");
        return true;

    case netsio::SPEED_CHANGE:
        if (len >= 5) {
            _baudrate = static_cast<std::uint32_t>(data[1]) |
                       (static_cast<std::uint32_t>(data[2]) << 8) |
                       (static_cast<std::uint32_t>(data[3]) << 16) |
                       (static_cast<std::uint32_t>(data[4]) << 24);
            FN_LOGD(TAG, "SPEED_CHANGE: %u baud", _baudrate);
            return true;
        }
        break;

    case netsio::SYNC_RESPONSE:
        // This is sent by device, not received
        FN_LOGW(TAG, "Unexpected SYNC_RESPONSE received");
        return false;

    case netsio::BUS_IDLE:
        FN_LOGD(TAG, "BUS_IDLE");
        return true;

    case netsio::DEVICE_CONNECT:
        FN_LOGI(TAG, "DEVICE_CONNECT");
        return true;

    case netsio::DEVICE_DISCONNECT:
        FN_LOGI(TAG, "DEVICE_DISCONNECT");
        return true;

    case netsio::PING_REQUEST:
        FN_LOGD(TAG, "PING_REQUEST");
        return true;

    case netsio::PING_RESPONSE:
        FN_LOGD(TAG, "PING_RESPONSE");
        return true;

    case netsio::ALIVE_REQUEST:
        FN_LOGD(TAG, "ALIVE_REQUEST");
        return true;

    case netsio::ALIVE_RESPONSE:
        FN_LOGD(TAG, "ALIVE_RESPONSE");
        return true;

    case netsio::CREDIT_STATUS:
        if (len >= 2) {
            _credit = data[1];
            FN_LOGD(TAG, "CREDIT_STATUS: %u", _credit);
            return true;
        }
        break;

    case netsio::CREDIT_UPDATE:
        if (len >= 2) {
            _credit = data[1];
            FN_LOGD(TAG, "CREDIT_UPDATE: %u", _credit);
            return true;
        }
        break;

    case netsio::WARM_RESET:
        FN_LOGI(TAG, "WARM_RESET");
        return true;

    case netsio::COLD_RESET:
        FN_LOGI(TAG, "COLD_RESET");
        return true;

    default:
        FN_LOGW(TAG, "Unknown NetSIO message type: 0x%02X", _lastMessageType);
        return false;
    }

    return false;
}

void NetSIOProtocol::sendDataByte(std::uint8_t byte)
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::DATA_BYTE);
    _txBuffer.push_back(byte);
}

void NetSIOProtocol::sendDataBlock(const std::uint8_t* data, std::size_t len)
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::DATA_BLOCK);
    _txBuffer.insert(_txBuffer.end(), data, data + len);
}

void NetSIOProtocol::sendCommandOn()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::COMMAND_ON);
}

void NetSIOProtocol::sendCommandOff()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::COMMAND_OFF);
}

void NetSIOProtocol::sendMotorOn()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::MOTOR_ON);
}

void NetSIOProtocol::sendMotorOff()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::MOTOR_OFF);
}

void NetSIOProtocol::sendProceedOn()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::PROCEED_ON);
}

void NetSIOProtocol::sendProceedOff()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::PROCEED_OFF);
}

void NetSIOProtocol::sendInterruptOn()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::INTERRUPT_ON);
}

void NetSIOProtocol::sendInterruptOff()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::INTERRUPT_OFF);
}

void NetSIOProtocol::sendSyncResponse(std::uint8_t syncNum, std::uint8_t ackType, std::uint8_t ackByte, std::uint16_t writeSize)
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::SYNC_RESPONSE);
    _txBuffer.push_back(syncNum);
    _txBuffer.push_back(ackType);
    _txBuffer.push_back(ackByte);
    _txBuffer.push_back(static_cast<std::uint8_t>(writeSize & 0xFF));
    _txBuffer.push_back(static_cast<std::uint8_t>((writeSize >> 8) & 0xFF));
}

void NetSIOProtocol::sendDeviceConnect()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::DEVICE_CONNECT);
}

void NetSIOProtocol::sendDeviceDisconnect()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::DEVICE_DISCONNECT);
}

void NetSIOProtocol::sendPingRequest()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::PING_REQUEST);
}

void NetSIOProtocol::sendPingResponse()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::PING_RESPONSE);
}

void NetSIOProtocol::sendAliveRequest()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::ALIVE_REQUEST);
}

void NetSIOProtocol::sendAliveResponse()
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::ALIVE_RESPONSE);
}

void NetSIOProtocol::sendCreditStatus(std::uint8_t credit)
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::CREDIT_STATUS);
    _txBuffer.push_back(credit);
}

void NetSIOProtocol::sendCreditUpdate(std::uint8_t credit)
{
    _txBuffer.clear();
    _txBuffer.push_back(netsio::CREDIT_UPDATE);
    _txBuffer.push_back(credit);
}

} // namespace fujinet::io::transport::legacy
