#pragma once

#include "fake_fs.h"
#include "packet_io_double.h"

#include "fujinet/core/core.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/devices/clock_commands.h"
#include "fujinet/io/devices/clock_device.h"
#include "fujinet/io/devices/file_device.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/protocol/slip_codec.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/io/transport/native_framer.h"
#include "fujinet/io/transport/slip_framer.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::platform {
void set_test_unix_time_seconds(std::optional<std::uint64_t> frozen);
}

namespace file_clock_core_parity {

using fujinet::core::FujinetCore;
using fujinet::io::ClockDevice;
using fujinet::io::FileDevice;
using fujinet::io::FujiBusTransport;
using fujinet::io::NativeFramer;
using fujinet::io::SlipFramer;
using fujinet::io::StatusCode;
using fujinet::io::protocol::ByteBuffer;
using fujinet::io::protocol::FujiBusPacket;
using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::decodeSLIP;
using fujinet::io::protocol::to_device_id;
using packet_io_test::PacketChannel;
using packet_io_test::PacketIODouble;

class FrozenUnixTime {
public:
    explicit FrozenUnixTime(std::uint64_t unix_seconds)
    {
        fujinet::platform::set_test_unix_time_seconds(unix_seconds);
    }
    FrozenUnixTime(const FrozenUnixTime&) = delete;
    FrozenUnixTime& operator=(const FrozenUnixTime&) = delete;
    ~FrozenUnixTime() { fujinet::platform::set_test_unix_time_seconds(std::nullopt); }
};

class LoopbackChannel : public fujinet::io::Channel {
public:
    void push(const std::vector<std::uint8_t>& data)
    {
        for (auto b : data) _rx.push_back(b);
    }

    std::vector<std::uint8_t> takeTx()
    {
        std::vector<std::uint8_t> out;
        out.reserve(_tx.size());
        while (!_tx.empty()) {
            out.push_back(_tx.front());
            _tx.pop_front();
        }
        return out;
    }

    bool available() override { return !_rx.empty(); }

    std::size_t read(std::uint8_t* buf, std::size_t maxLen) override
    {
        std::size_t n = 0;
        while (n < maxLen && !_rx.empty()) {
            buf[n++] = _rx.front();
            _rx.pop_front();
        }
        return n;
    }

    void write(const std::uint8_t* buf, std::size_t len) override
    {
        for (std::size_t i = 0; i < len; ++i) _tx.push_back(buf[i]);
    }

private:
    std::deque<std::uint8_t> _rx;
    std::deque<std::uint8_t> _tx;
};

inline bool seed_isolated_tree(fujinet::tests::MemoryFileSystem& fs)
{
    return fs.createDirectory("/beta") && fs.create_file("/alpha.txt", {'h', 'i'});
}

inline std::vector<std::uint8_t> make_list_request(std::string_view uri)
{
    std::vector<std::uint8_t> payload;
    payload.push_back(1);
    const auto len = static_cast<std::uint16_t>(uri.size());
    payload.push_back(static_cast<std::uint8_t>(len & 0xFF));
    payload.push_back(static_cast<std::uint8_t>(len >> 8));
    payload.insert(payload.end(), uri.begin(), uri.end());
    payload.push_back(0);
    payload.push_back(0);
    payload.push_back(0x00);
    payload.push_back(0x02); // maxPayloadBytes = 512
    return payload;
}

inline std::vector<std::uint8_t> make_set_time_payload(std::uint64_t unix_seconds)
{
    std::vector<std::uint8_t> payload;
    payload.push_back(1);
    for (int i = 0; i < 8; ++i) {
        payload.push_back(static_cast<std::uint8_t>((unix_seconds >> (8 * i)) & 0xFF));
    }
    return payload;
}

inline std::vector<std::uint8_t> make_get_time_format_utc()
{
    return {1, static_cast<std::uint8_t>(fujinet::io::TimeFormat::UtcIsoString)};
}

inline std::uint64_t read_u64le(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[offset + i]) << (8U * i);
    }
    return value;
}

struct DecodedReply {
    bool got{false};
    StatusCode status{StatusCode::InternalError};
    std::uint8_t device{0};
    std::uint8_t command{0};
    std::vector<std::uint8_t> payload;
};

inline DecodedReply decode_raw(const ByteBuffer& raw)
{
    DecodedReply out;
    auto packet = FujiBusPacket::fromRaw(raw);
    if (!packet) return out;
    out.got = true;
    out.device = static_cast<std::uint8_t>(packet->device());
    out.command = packet->command();
    std::uint8_t st = static_cast<std::uint8_t>(StatusCode::InternalError);
    if (packet->tryParamU8(0, st)) {
        out.status = static_cast<StatusCode>(st);
    }
    if (packet->data()) {
        out.payload.assign(packet->data()->begin(), packet->data()->end());
    }
    return out;
}

enum class Framing { Serial, Native };

class CoreParityStack {
public:
    CoreParityStack(Framing framing, std::string fs_name)
        : _framing(framing)
        , _fs_name(std::move(fs_name))
    {
        auto fs = std::make_unique<fujinet::tests::MemoryFileSystem>(_fs_name);
        _seeded = seed_isolated_tree(*fs);
        _registered_fs = _core.storageManager().registerFileSystem(std::move(fs));
        _registered_file = _core.deviceManager().registerDevice(
            to_device_id(WireDeviceId::FileService),
            std::make_unique<FileDevice>(_core.storageManager()));
        _registered_clock = _core.deviceManager().registerDevice(
            to_device_id(WireDeviceId::Clock),
            std::make_unique<ClockDevice>());

        if (_framing == Framing::Serial) {
            _bytes = std::make_unique<LoopbackChannel>();
            _slip = std::make_unique<SlipFramer>();
            _transport = std::make_unique<FujiBusTransport>(*_bytes, *_slip);
        } else {
            _packets = std::make_unique<PacketIODouble>(2048, 8, 8192);
            _packet_ch = std::make_unique<PacketChannel>(_packets.get());
            _native = std::make_unique<NativeFramer>(*_packets);
            _transport = std::make_unique<FujiBusTransport>(*_packet_ch, *_native);
        }
        _core.addTransport(_transport.get());
    }

    bool ready() const
    {
        return _seeded && _registered_fs && _registered_file && _registered_clock;
    }

    const std::string& fs_name() const { return _fs_name; }

    std::string root_uri() const { return _fs_name + ":/"; }

    DecodedReply exchange(WireDeviceId device, std::uint8_t command,
                          const std::vector<std::uint8_t>& payload)
    {
        FujiBusPacket pkt(device, command);
        if (!payload.empty()) {
            pkt.setData(ByteBuffer(payload.begin(), payload.end()));
        }

        if (_framing == Framing::Serial) {
            const auto framed = pkt.serialize();
            _bytes->push(std::vector<std::uint8_t>(framed.begin(), framed.end()));
            _core.tick();
            const auto tx = _bytes->takeTx();
            return decode_raw(decodeSLIP(ByteBuffer(tx.begin(), tx.end())));
        }

        const auto raw = pkt.serializeRaw();
        if (raw.empty() || !_packets->enqueue(raw)) {
            return {};
        }
        _core.tick();
        ByteBuffer sent;
        if (!_packets->takeSent(sent)) {
            return {};
        }
        return decode_raw(sent);
    }

private:
    Framing _framing;
    std::string _fs_name;
    bool _seeded{false};
    bool _registered_fs{false};
    bool _registered_file{false};
    bool _registered_clock{false};
    FujinetCore _core;
    std::unique_ptr<LoopbackChannel> _bytes;
    std::unique_ptr<SlipFramer> _slip;
    std::unique_ptr<PacketIODouble> _packets;
    std::unique_ptr<PacketChannel> _packet_ch;
    std::unique_ptr<NativeFramer> _native;
    std::unique_ptr<FujiBusTransport> _transport;
};

struct ListEntry {
    std::uint8_t flags{0};
    std::string name;
    std::uint64_t size_bytes{0};
};

inline std::vector<ListEntry> parse_list_entries(const std::vector<std::uint8_t>& payload)
{
    std::vector<ListEntry> entries;
    if (payload.size() < 10) return entries;
    const std::uint16_t entry_count =
        static_cast<std::uint16_t>(payload[6] | (static_cast<std::uint16_t>(payload[7]) << 8));
    std::size_t offset = 10;
    for (std::uint16_t i = 0; i < entry_count; ++i) {
        if (offset + 2 > payload.size()) break;
        ListEntry entry;
        entry.flags = payload[offset];
        const std::uint8_t name_len = payload[offset + 1];
        offset += 2;
        if (offset + name_len > payload.size()) break;
        entry.name.assign(reinterpret_cast<const char*>(payload.data() + offset), name_len);
        offset += name_len;
        if (offset + 16 > payload.size()) break;
        entry.size_bytes = read_u64le(payload, offset);
        offset += 16;
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace file_clock_core_parity
