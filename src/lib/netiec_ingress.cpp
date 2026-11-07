#include "fujinet/io/netiec_ingress.h"

#include <algorithm>
#include <utility>

namespace fujinet::io {

namespace {

constexpr std::uint8_t cmd_hello = 0x01;
constexpr std::uint8_t cmd_atn = 0x02;
constexpr std::uint8_t cmd_write = 0x03;
constexpr std::uint8_t cmd_read = 0x04;
constexpr std::uint8_t cmd_reset = 0x05;

constexpr std::uint8_t reply_bit = 0x80;
constexpr std::uint8_t reply_unsupported = 0xFF;
constexpr std::uint8_t flag_eoi = 0x01;
constexpr std::uint8_t atn_acknowledged = 0;
constexpr std::uint8_t atn_not_present = 1;

constexpr std::size_t header_bytes = 3;
constexpr std::size_t max_read_chunk = 4096;

void put_frame(std::vector<std::uint8_t>& out, std::uint8_t command, const std::vector<std::uint8_t>& payload)
{
    out.push_back(command);
    out.push_back(static_cast<std::uint8_t>(payload.size() & 0xFF));
    out.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
    out.insert(out.end(), payload.begin(), payload.end());
}

} // namespace

void NetIecServer::receive(const std::uint8_t* data, std::size_t length, std::vector<std::uint8_t>& out)
{
    _pending.insert(_pending.end(), data, data + length);

    std::size_t at = 0;
    while (_pending.size() - at >= header_bytes) {
        const std::size_t payload = static_cast<std::size_t>(_pending[at + 1]) |
                                    (static_cast<std::size_t>(_pending[at + 2]) << 8);
        if (_pending.size() - at < header_bytes + payload) {
            break;
        }
        handle(_pending[at], _pending.data() + at + header_bytes, payload, out);
        at += header_bytes + payload;
    }
    _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(at));
}

void NetIecServer::handle(std::uint8_t command, const std::uint8_t* payload, std::size_t length,
                          std::vector<std::uint8_t>& out)
{
    _reply.clear();

    switch (command) {
        case cmd_hello: {
            // A new connection may be the same machine reconnecting, so the devices
            // keep their state; only an unfinished transaction is dropped. A reset of
            // the machine arrives as its own request.
            _bus.forget_addressing();
            const std::uint32_t mask = _bus.device_mask();
            _reply.push_back(version);
            for (int shift = 0; shift < 32; shift += 8) {
                _reply.push_back(static_cast<std::uint8_t>(mask >> shift));
            }
            _reply.insert(_reply.end(), _name.begin(), _name.end());
            break;
        }

        case cmd_atn: {
            const IecAtnStatus status = _bus.atn(payload, length);
            _reply.push_back(status == IecAtnStatus::Acknowledged ? atn_acknowledged : atn_not_present);
            break;
        }

        case cmd_write: {
            std::size_t accepted = 0;
            if (length >= 1) {
                const bool eoi = (payload[0] & flag_eoi) != 0;
                accepted = _bus.write(payload + 1, length - 1, eoi);
            }
            _reply.push_back(static_cast<std::uint8_t>(accepted & 0xFF));
            _reply.push_back(static_cast<std::uint8_t>(accepted >> 8));
            break;
        }

        case cmd_read: {
            std::size_t wanted = 0;
            if (length >= 2) {
                wanted = static_cast<std::size_t>(payload[0]) | (static_cast<std::size_t>(payload[1]) << 8);
            }
            wanted = std::min(wanted, max_read_chunk);
            _reply.resize(1 + wanted);
            const IecReadResult result = _bus.read(_reply.data() + 1, wanted);
            _reply.resize(1 + result.bytes);
            _reply[0] = result.eoi ? flag_eoi : 0;
            break;
        }

        case cmd_reset:
            _bus.reset();
            break;

        default:
            _reply.push_back(command);
            put_frame(out, reply_unsupported, _reply);
            return;
    }

    put_frame(out, static_cast<std::uint8_t>(command | reply_bit), _reply);
}

NetIecIngress::NetIecIngress(std::unique_ptr<Channel> channel, IecBus& bus, std::string name)
    : _channel(std::move(channel)), _epoch(_channel ? _channel->connection_epoch() : 0),
      _server(bus, std::move(name))
{
}

void NetIecIngress::poll()
{
    if (!_channel) {
        return;
    }
    std::uint8_t buffer[1024];
    while (_channel->available()) {
        // A client that went away mid-request must not have its half frame
        // completed by the next one's first bytes.
        const std::uint32_t epoch = _channel->connection_epoch();
        if (epoch != _epoch) {
            _epoch = epoch;
            _server.drop_partial_request();
        }
        const std::size_t got = _channel->read(buffer, sizeof(buffer));
        if (got == 0) {
            break;
        }
        _out.clear();
        _server.receive(buffer, got, _out);
        if (!_out.empty()) {
            _channel->write(_out.data(), _out.size());
        }
    }
}

bool NetIecIngress::supports_work_wait() const
{
    return _channel && _channel->supports_readable_wait();
}

bool NetIecIngress::wait_for_work(std::chrono::milliseconds timeout)
{
    return _channel ? _channel->wait_for_readable(timeout) : false;
}

} // namespace fujinet::io
