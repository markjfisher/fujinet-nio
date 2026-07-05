#include "fujinet/platform/posix/atari_netsio_fujibus_channel.h"

#include "fujinet/core/logging.h"
#include "fujinet/io/transport/legacy/netsio_protocol.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unistd.h>
#include <vector>

namespace fujinet::platform::posix {

static constexpr const char* TAG = "netsio-fb";

class AtariNetSioFujiBusChannel : public fujinet::io::Channel {
public:
    explicit AtariNetSioFujiBusChannel(std::unique_ptr<fujinet::io::Channel> udp)
        : _udp(std::move(udp))
    {
        _netsio.sendDeviceConnect();
        write_netsio();
        _netsio.sendSpeedChange(19200);
        write_netsio();
        send_alive();
        _netsio.sendProceedOn();
        write_netsio();
    }

    ~AtariNetSioFujiBusChannel() override
    {
        _netsio.sendDeviceDisconnect();
        write_netsio();
    }

    bool available() override
    {
        pump();
        return !_rx.empty();
    }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override
    {
        if (!buffer || maxLen == 0) {
            return 0;
        }
        pump();
        const std::size_t n = std::min(maxLen, _rx.size());
        if (n == 0) {
            return 0;
        }
        std::copy_n(_rx.begin(), n, buffer);
        _rx.erase(_rx.begin(), _rx.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

    void write(const std::uint8_t* buffer, std::size_t len) override
    {
        if (!buffer || len == 0) {
            return;
        }
        _tx.insert(_tx.end(), buffer, buffer + len);
        FN_LOGD(TAG, "queued SIO read response: %zu bytes pending=%zu", len, _tx.size());
    }

private:
    void write_netsio()
    {
        if (!_udp) {
            return;
        }
        const auto& msg = _netsio.getEncodedMessage();
        if (!msg.empty()) {
            _udp->write(msg.data(), msg.size());
        }
    }

    void send_alive()
    {
        _netsio.sendAliveRequest();
        write_netsio();
        _lastAlive = std::chrono::steady_clock::now();
    }

    void send_alive_if_due()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - _lastAlive >= std::chrono::seconds(10)) {
            send_alive();
        }
    }

    void send_initial_host_speed_if_needed(std::uint8_t messageType)
    {
        if (_initialHostSpeedSent) {
            return;
        }
        if (messageType >= fujinet::io::transport::legacy::netsio::DEVICE_DISCONNECT) {
            return;
        }
        _netsio.sendSpeedChange(19200);
        write_netsio();
        _initialHostSpeedSent = true;
    }

    void send_empty_sync(std::uint8_t syncNum)
    {
        using namespace fujinet::io::transport::legacy;
        _netsio.sendSyncResponse(syncNum, netsio::EMPTY_SYNC, 0, 0);
        write_netsio();
    }

    void send_ack_sync(std::uint8_t syncNum, std::uint8_t ackByte, std::uint16_t writeSize)
    {
        using namespace fujinet::io::transport::legacy;
        request_credit_before_sync_response();
        _netsio.sendSyncResponse(syncNum, netsio::ACK_SYNC, ackByte, writeSize);
        write_netsio();
    }

    void send_pending_read_response()
    {
        if (_tx.empty()) {
            FN_LOGD(TAG, "SIO READ requested with no pending response");
            return;
        }

        const std::size_t transferSize = _pendingReadSize == 0 ? DEFAULT_SIO_READ_SIZE : _pendingReadSize;
        // Give the custom device a small gap after the command ACK before
        // transmitting payload bytes to the Atari SIO bus.
        ::usleep(1000);
        send_sio_complete();
        send_padded_data_blocks(transferSize);
        const std::uint8_t checksum = sio_checksum_padded(transferSize);
        _netsio.sendDataByte(checksum);
        write_netsio();
        const std::size_t payloadBytes = std::min(_tx.size(), transferSize);
        if (_tx.size() <= transferSize) {
            _tx.clear();
        } else {
            _tx.erase(_tx.begin(), _tx.begin() + static_cast<std::ptrdiff_t>(transferSize));
        }
        FN_LOGI(TAG,
                "sent SIO read response: payload=%zu transfer=%zu checksum=0x%02X remaining=%zu",
                payloadBytes,
                transferSize,
                checksum,
                _tx.size());
        _pendingReadSize = 0;
    }

    void send_sio_complete()
    {
        _netsio.sendDataByte(SIO_COMPLETE);
        write_netsio();
    }

    void send_padded_data_blocks(std::size_t transferSize)
    {
        std::uint8_t chunk[MAX_NETSIO_DATA_BLOCK];
        std::size_t offset = 0;
        while (offset < transferSize) {
            const std::size_t chunkLen = std::min<std::size_t>(MAX_NETSIO_DATA_BLOCK, transferSize - offset);
            for (std::size_t i = 0; i < chunkLen; ++i) {
                const std::size_t sourceOffset = offset + i;
                chunk[i] = sourceOffset < _tx.size() ? _tx[sourceOffset] : 0;
            }
            _netsio.sendDataBlock(chunk, chunkLen);
            write_netsio();
            offset += chunkLen;
        }
    }

    std::uint8_t sio_checksum_padded(std::size_t transferSize) const
    {
        std::uint8_t checksum = 0;
        for (std::size_t i = 0; i < transferSize; ++i) {
            const std::uint8_t value = i < _tx.size() ? _tx[i] : 0;
            checksum = sio_checksum_update(checksum, value);
        }
        return checksum;
    }

    static std::uint8_t sio_checksum_update(std::uint8_t checksum, std::uint8_t value)
    {
        const std::uint16_t sum = static_cast<std::uint16_t>(checksum) + value;
        return static_cast<std::uint8_t>((sum & 0xFF) + (sum >> 8));
    }

    void request_credit_before_sync_response()
    {
        using namespace fujinet::io::transport::legacy;

        _netsio.sendCreditStatus(0);
        write_netsio();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
        std::uint8_t buf[512];
        while (std::chrono::steady_clock::now() < deadline && _udp && _udp->available()) {
            const std::size_t n = _udp->read(buf, sizeof(buf));
            if (!n || !_netsio.parseMessage(buf, n)) {
                continue;
            }
            const auto type = _netsio.getMessageType();
            if (type == netsio::CREDIT_UPDATE) {
                break;
            }
            if (type == netsio::ALIVE_REQUEST) {
                _netsio.sendAliveResponse();
                write_netsio();
            }
        }
    }

    static std::uint16_t aux_word(const std::vector<std::uint8_t>& frame)
    {
        if (frame.size() < 4) {
            return 0;
        }
        return static_cast<std::uint16_t>(frame[2]) |
               static_cast<std::uint16_t>(frame[3] << 8);
    }

    bool handle_command_frame(const std::vector<std::uint8_t>& frame)
    {
        if (frame.size() < 4) {
            FN_LOGW(TAG, "short SIO command frame: %zu bytes", frame.size());
            _pendingCommand = PendingCommand::None;
            _pendingWriteSize = 0;
            _pendingWriteRemaining = 0;
            return false;
        }

        const std::uint8_t device = frame[0];
        const std::uint8_t command = frame[1];
        const std::uint16_t aux = aux_word(frame);

        if (!is_nio_sio_device(device)) {
            FN_LOGD(TAG,
                    "ignoring SIO command dev=0x%02X cmd=0x%02X aux=%u",
                    device, command, static_cast<unsigned>(aux));
            _pendingCommand = PendingCommand::None;
            _pendingWriteSize = 0;
            _pendingWriteRemaining = 0;
            return false;
        }

        if (command == NIO_SIO_WRITE_COMMAND) {
            _pendingCommand = PendingCommand::Write;
            _pendingWriteSize = aux;
            _pendingWriteRemaining = aux;
            _pendingWriteChecksum = 0;
            FN_LOGI(TAG,
                    "SIO command WRITE dev=0x%02X aux(write_size)=%u",
                    device, static_cast<unsigned>(_pendingWriteSize));
            return true;
        }

        if (command == NIO_SIO_READ_COMMAND) {
            _pendingCommand = PendingCommand::Read;
            _pendingWriteSize = 0;
            _pendingWriteRemaining = 0;
            _pendingReadSize = aux == 0 ? DEFAULT_SIO_READ_SIZE : aux;
            FN_LOGI(TAG,
                    "SIO command READ dev=0x%02X aux=%u transfer_size=%u pending_response=%zu",
                    device,
                    static_cast<unsigned>(aux),
                    static_cast<unsigned>(_pendingReadSize),
                    _tx.size());
            return true;
        }

        FN_LOGW(TAG,
                "unknown NIO SIO command dev=0x%02X cmd=0x%02X aux=%u",
                device, command, static_cast<unsigned>(aux));
        _pendingCommand = PendingCommand::None;
        _pendingWriteSize = 0;
        _pendingWriteRemaining = 0;
        return false;
    }

    void handle_sync_request(std::uint8_t syncNum)
    {
        if (_pendingCommand == PendingCommand::Write) {
            const std::uint16_t sioWriteSize = static_cast<std::uint16_t>(_pendingWriteSize + 1);
            send_ack_sync(syncNum, SIO_ACK, sioWriteSize);
            FN_LOGD(TAG,
                    "ACK WRITE sync=%u write_size=%u sio_write_size=%u",
                    static_cast<unsigned>(syncNum),
                    static_cast<unsigned>(_pendingWriteSize),
                    static_cast<unsigned>(sioWriteSize));
            return;
        }

        if (_pendingCommand == PendingCommand::Read) {
            send_ack_sync(syncNum, SIO_ACK, 0);
            FN_LOGD(TAG, "ACK READ sync=%u", static_cast<unsigned>(syncNum));
            _pendingCommand = PendingCommand::None;
            send_pending_read_response();
            return;
        }

        send_empty_sync(syncNum);
    }

    void handle_data_block(const std::vector<std::uint8_t>& payload)
    {
        if (_commandActive) {
            handle_command_frame(payload);
            return;
        }

        if (_pendingCommand == PendingCommand::Write) {
            const std::size_t accepted = std::min<std::size_t>(payload.size(), _pendingWriteRemaining);
            _rx.insert(_rx.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(accepted));
            update_pending_write_checksum(payload.data(), accepted);
            _pendingWriteRemaining = static_cast<std::uint16_t>(_pendingWriteRemaining - accepted);
            FN_LOGI(TAG,
                    "accepted SIO write payload: %zu bytes remaining=%u",
                    accepted,
                    static_cast<unsigned>(_pendingWriteRemaining));
            if (_pendingWriteRemaining == 0) {
                _pendingCommand = PendingCommand::WriteChecksum;
            }
            if (accepted < payload.size()) {
                FN_LOGW(TAG,
                        "discarding %zu extra SIO write byte(s)",
                        payload.size() - accepted);
            }
            return;
        }

        _rx.insert(_rx.end(), payload.begin(), payload.end());
        FN_LOGD(TAG, "accepted raw NetSIO data block: %zu bytes", payload.size());
    }

    void update_pending_write_checksum(const std::uint8_t* data, std::size_t len)
    {
        for (std::size_t i = 0; i < len; ++i) {
            const std::uint16_t sum = static_cast<std::uint16_t>(_pendingWriteChecksum) + data[i];
            _pendingWriteChecksum = static_cast<std::uint8_t>((sum & 0xFF) + (sum >> 8));
        }
    }

    void handle_data_checksum(std::uint8_t checksum, std::uint8_t syncNum)
    {
        if (_pendingCommand != PendingCommand::WriteChecksum) {
            _rx.push_back(checksum);
            send_empty_sync(syncNum);
            return;
        }

        if (checksum != _pendingWriteChecksum) {
            FN_LOGW(TAG,
                    "SIO write checksum mismatch calc=0x%02X got=0x%02X",
                    _pendingWriteChecksum,
                    checksum);
        } else {
            FN_LOGD(TAG, "SIO write checksum ok: 0x%02X", checksum);
        }
        send_ack_sync(syncNum, SIO_ACK, 0);
        send_sio_complete();
        _pendingCommand = PendingCommand::None;
        _pendingWriteSize = 0;
        _pendingWriteRemaining = 0;
        _pendingWriteChecksum = 0;
    }

    void pump()
    {
        if (!_udp) {
            return;
        }

        send_alive_if_due();

        std::uint8_t buf[512];
        while (_udp->available()) {
            const std::size_t n = _udp->read(buf, sizeof(buf));
            if (n == 0) {
                break;
            }
            if (!_netsio.parseMessage(buf, n)) {
                continue;
            }

            using namespace fujinet::io::transport::legacy;
            const std::uint8_t type = _netsio.getMessageType();
            const auto& payload = _netsio.getPayload();
            send_initial_host_speed_if_needed(type);
            switch (type) {
            case netsio::DATA_BYTE:
                if (!payload.empty()) {
                    _rx.push_back(payload[0]);
                }
                break;
            case netsio::DATA_BYTE_SYNC:
                if (payload.size() > 1) {
                    handle_data_checksum(payload[0], payload[1]);
                } else if (!payload.empty()) {
                    _rx.push_back(payload[0]);
                }
                break;
            case netsio::DATA_BLOCK:
                handle_data_block(payload);
                break;
            case netsio::PING_REQUEST:
                _netsio.sendPingResponse();
                write_netsio();
                break;
            case netsio::ALIVE_REQUEST:
                _netsio.sendAliveResponse();
                write_netsio();
                break;
            case netsio::ALIVE_RESPONSE:
            case netsio::PING_RESPONSE:
                break;
            case netsio::CREDIT_UPDATE:
            case netsio::CREDIT_STATUS:
            case netsio::BUS_IDLE:
                break;
            case netsio::COMMAND_ON:
                _commandActive = true;
                break;
            case netsio::COMMAND_OFF:
                _commandActive = false;
                break;
            case netsio::COMMAND_OFF_SYNC:
                if (!payload.empty()) {
                    _commandActive = false;
                    handle_sync_request(payload[0]);
                }
                break;
            case netsio::MOTOR_ON:
            case netsio::MOTOR_OFF:
            case netsio::PROCEED_ON:
            case netsio::PROCEED_OFF:
            case netsio::INTERRUPT_ON:
            case netsio::INTERRUPT_OFF:
            case netsio::SPEED_CHANGE:
            case netsio::WARM_RESET:
            case netsio::COLD_RESET:
                _initialHostSpeedSent = false;
                break;
            default:
                break;
            }
        }
    }

    std::unique_ptr<fujinet::io::Channel> _udp;
    fujinet::io::transport::legacy::NetSIOProtocol _netsio;
    std::vector<std::uint8_t> _rx;
    std::vector<std::uint8_t> _tx;
    enum class PendingCommand {
        None,
        Write,
        Read,
        WriteChecksum,
    };
    static constexpr std::uint8_t NIO_SIO_DEVICE = 0x7F;
    static constexpr std::uint8_t NETSIO_NETWORK_DEVICE = 0x71;
    static constexpr std::uint8_t NIO_SIO_WRITE_COMMAND = 'W';
    static constexpr std::uint8_t NIO_SIO_READ_COMMAND = 'R';
    static constexpr std::uint8_t SIO_ACK = 'A';
    static constexpr std::uint8_t SIO_COMPLETE = 'C';
    static constexpr std::uint16_t DEFAULT_SIO_READ_SIZE = 768;
    static constexpr std::size_t MAX_NETSIO_DATA_BLOCK = 512;
    static bool is_nio_sio_device(std::uint8_t device)
    {
        return device == NIO_SIO_DEVICE || device == NETSIO_NETWORK_DEVICE;
    }
    bool _commandActive{false};
    bool _initialHostSpeedSent{false};
    PendingCommand _pendingCommand{PendingCommand::None};
    std::uint16_t _pendingWriteSize{0};
    std::uint16_t _pendingWriteRemaining{0};
    std::uint16_t _pendingReadSize{0};
    std::uint8_t _pendingWriteChecksum{0};
    std::chrono::steady_clock::time_point _lastAlive{};
};

std::unique_ptr<fujinet::io::Channel>
create_atari_netsio_fujibus_channel(std::unique_ptr<fujinet::io::Channel> udp)
{
    return std::make_unique<AtariNetSioFujiBusChannel>(std::move(udp));
}

} // namespace fujinet::platform::posix
