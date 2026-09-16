#pragma once

#include "fake_fs.h"
#include "packet_io_double.h"

#include "fujinet/disk/disk_types.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/disk_codec.h"
#include "fujinet/io/devices/disk_commands.h"
#include "fujinet/io/devices/disk_device.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/io/transport/native_framer.h"
#include "fujinet/io/transport/slip_framer.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace disk_parity {

namespace diskproto = fujinet::io::diskproto;
using fujinet::io::DiskDevice;
using fujinet::io::IORequest;
using fujinet::io::IOResponse;
using fujinet::io::StatusCode;
using fujinet::io::protocol::ByteBuffer;
using fujinet::io::protocol::FujiBusPacket;
using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;
using packet_io_test::PacketChannel;
using packet_io_test::PacketIODouble;

inline constexpr std::uint8_t kProto = 1;
inline constexpr std::uint8_t kSlot = 1;
inline constexpr std::uint16_t kSectorSize = 256;
inline constexpr std::uint32_t kSectorCount = 4;
inline constexpr const char* kPath = "/disks/parity.img";
inline constexpr const char* kUri = "mem:///disks/parity.img";

inline std::vector<std::uint8_t> make_seed_image()
{
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(kSectorCount) * kSectorSize);
    for (std::uint32_t sector = 0; sector < kSectorCount; ++sector) {
        std::fill(bytes.begin() + sector * kSectorSize,
                  bytes.begin() + (sector + 1) * kSectorSize,
                  static_cast<std::uint8_t>(0xA0 + sector));
    }
    return bytes;
}

inline std::vector<std::uint8_t> expected_after_write(
    const std::vector<std::uint8_t>& seed,
    std::uint32_t lba,
    const std::vector<std::uint8_t>& sector)
{
    auto out = seed;
    const std::size_t off = static_cast<std::size_t>(lba) * kSectorSize;
    if (lba >= kSectorCount || sector.size() != kSectorSize ||
        off + kSectorSize > out.size()) {
        return {};
    }
    std::copy(sector.begin(), sector.end(), out.begin() + off);
    return out;
}

inline bool neighbors_unchanged(
    const std::vector<std::uint8_t>& seed,
    const std::vector<std::uint8_t>& actual,
    std::uint32_t writtenLba)
{
    if (seed.size() != actual.size()) return false;
    for (std::uint32_t sector = 0; sector < kSectorCount; ++sector) {
        if (sector == writtenLba) continue;
        const std::size_t off = static_cast<std::size_t>(sector) * kSectorSize;
        if (!std::equal(seed.begin() + off, seed.begin() + off + kSectorSize,
                        actual.begin() + off)) {
            return false;
        }
    }
    return true;
}

class EffectCountingFile final : public fujinet::fs::IFile {
public:
    EffectCountingFile(std::unique_ptr<fujinet::fs::IFile> inner, std::size_t* writeEffects)
        : _inner(std::move(inner)), _writeEffects(writeEffects) {}

    std::size_t read(void* dst, std::size_t maxBytes) override
    {
        return _inner->read(dst, maxBytes);
    }

    std::size_t write(const void* src, std::size_t bytes) override
    {
        const std::size_t n = _inner->write(src, bytes);
        if (n > 0 && _writeEffects) ++*_writeEffects;
        return n;
    }

    bool seek(std::uint64_t offset) override { return _inner->seek(offset); }
    std::uint64_t tell() const override { return _inner->tell(); }
    bool flush() override { return _inner->flush(); }

private:
    std::unique_ptr<fujinet::fs::IFile> _inner;
    std::size_t* _writeEffects;
};

class EffectCountingFS final : public fujinet::fs::IFileSystem {
public:
    explicit EffectCountingFS(std::string name) : _inner(std::move(name)) {}

    fujinet::fs::FileSystemKind kind() const override { return _inner.kind(); }
    std::string name() const override { return _inner.name(); }
    bool exists(const std::string& path) override { return _inner.exists(path); }
    bool isDirectory(const std::string& path) override { return _inner.isDirectory(path); }
    bool createDirectory(const std::string& path) override { return _inner.createDirectory(path); }
    bool removeFile(const std::string& path) override { return _inner.removeFile(path); }
    bool removeDirectory(const std::string& path) override { return _inner.removeDirectory(path); }
    bool rename(const std::string& from, const std::string& to) override
    {
        return _inner.rename(from, to);
    }

    std::unique_ptr<fujinet::fs::IFile> open(const std::string& path, const char* mode) override
    {
        auto file = _inner.open(path, mode);
        if (!file) return nullptr;
        return std::make_unique<EffectCountingFile>(std::move(file), &_writeEffects);
    }

    bool stat(const std::string& path, fujinet::fs::FileInfo& outInfo) override
    {
        return _inner.stat(path, outInfo);
    }

    bool listDirectory(const std::string& path, std::vector<fujinet::fs::FileInfo>& out) override
    {
        return _inner.listDirectory(path, out);
    }

    bool create_file(const std::string& path, const std::vector<std::uint8_t>& bytes)
    {
        return _inner.create_file(path, bytes);
    }

    std::vector<std::uint8_t>& file_bytes(const std::string& path)
    {
        return _inner.file_bytes(path);
    }

    std::size_t flush_count() const { return _inner.flush_count(); }
    std::size_t write_effects() const { return _writeEffects; }

private:
    fujinet::tests::MemoryFileSystem _inner;
    std::size_t _writeEffects{0};
};

class DiskParityWorld {
public:
    explicit DiskParityWorld(const std::vector<std::uint8_t>& seed)
        : _fs(install(seed))
        , device(_storage)
    {
    }

    EffectCountingFS& fs() { return *_fs; }
    std::vector<std::uint8_t>& image() { return _fs->file_bytes(kPath); }
    std::size_t write_effects() const { return _fs->write_effects(); }
    std::size_t flush_count() const { return _fs->flush_count(); }

private:
    EffectCountingFS* install(const std::vector<std::uint8_t>& seed)
    {
        auto owned = std::make_unique<EffectCountingFS>("mem");
        owned->createDirectory("/disks");
        owned->create_file(kPath, seed);
        auto* ptr = owned.get();
        _storage.registerFileSystem(std::move(owned));
        return ptr;
    }

    fujinet::fs::StorageManager _storage;
    EffectCountingFS* _fs;

public:
    DiskDevice device;
};

class BytePipeChannel final : public fujinet::io::Channel {
public:
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
        _tx.insert(_tx.end(), buf, buf + len);
        ++writeCalls;
    }

    void push(const ByteBuffer& data)
    {
        _rx.insert(_rx.end(), data.begin(), data.end());
    }

    ByteBuffer take_tx()
    {
        ByteBuffer out(_tx.begin(), _tx.end());
        _tx.clear();
        return out;
    }

    std::size_t writeCalls{0};

private:
    std::deque<std::uint8_t> _rx;
    std::deque<std::uint8_t> _tx;
};

inline ByteBuffer encode_request(std::uint8_t command, const std::vector<std::uint8_t>& payload)
{
    FujiBusPacket packet(WireDeviceId::DiskService, command);
    packet.setData(ByteBuffer(payload.begin(), payload.end()));
    return packet.serializeRaw();
}

struct ExchangeResult {
    bool received_request{false};
    bool handled{false};
    IOResponse service{};
    bool framed_ok{false};
    IOResponse framed{};
    std::size_t transmissions{0};
    std::size_t accepted{0};
};

inline ExchangeResult exchange_serial(DiskDevice& device, std::uint8_t command,
                                      const std::vector<std::uint8_t>& payload)
{
    ExchangeResult out;
    BytePipeChannel ch;
    fujinet::io::SlipFramer framer;
    fujinet::io::FujiBusTransport transport(ch, framer);

    FujiBusPacket packet(WireDeviceId::DiskService, command);
    packet.setData(ByteBuffer(payload.begin(), payload.end()));
    ch.push(packet.serialize());
    transport.poll();

    IORequest req{};
    out.received_request = transport.receive(req);
    if (!out.received_request) return out;

    out.service = device.handle(req);
    out.handled = true;
    transport.send(out.service);
    out.transmissions = ch.writeCalls;
    out.accepted = ch.writeCalls > 0 ? 1 : 0;

    const auto tx = ch.take_tx();
    ch.push(tx);
    transport.poll();
    out.framed_ok = transport.receiveResponse(out.framed);
    return out;
}

enum class NativeRequestFate {
    Deliver,
    Unavailable,
};

enum class NativeReplyFate {
    Deliver,
    SendFailed,
    UnknownCompletion,
};

inline ExchangeResult exchange_native(DiskDevice& device, std::uint8_t command,
                                      const std::vector<std::uint8_t>& payload,
                                      NativeRequestFate requestFate = NativeRequestFate::Deliver,
                                      NativeReplyFate replyFate = NativeReplyFate::Deliver)
{
    ExchangeResult out;
    PacketIODouble io(4096, 4, 8192);
    PacketChannel ch(&io);
    fujinet::io::NativeFramer framer;
    fujinet::io::FujiBusTransport transport(ch, framer);

    const ByteBuffer raw = encode_request(command, payload);
    if (requestFate == NativeRequestFate::Unavailable) {
        io.peerAvailable = false;
    }
    io.enqueue(raw);
    transport.poll();

    IORequest req{};
    out.received_request = transport.receive(req);
    if (!out.received_request) {
        out.transmissions = io.sendCalls;
        out.accepted = io.acceptedCount;
        return out;
    }

    out.service = device.handle(req);
    out.handled = true;

    if (replyFate == NativeReplyFate::SendFailed) {
        io.setNextSendResult(fujinet::io::PacketIOStatus::SendFailed);
    } else if (replyFate == NativeReplyFate::UnknownCompletion) {
        io.setNextSendResult(fujinet::io::PacketIOStatus::UnknownCompletion, true);
    }

    transport.send(out.service);
    out.transmissions = io.sendCalls;
    out.accepted = io.acceptedCount;

    ByteBuffer sent;
    if (replyFate == NativeReplyFate::Deliver && io.takeSent(sent)) {
        io.enqueue(sent);
        transport.poll();
        out.framed_ok = transport.receiveResponse(out.framed);
    }
    return out;
}

inline std::vector<std::uint8_t> mount_payload(bool readOnly)
{
    std::vector<std::uint8_t> p;
    diskproto::write_u8(p, kProto);
    diskproto::write_u8(p, kSlot);
    diskproto::write_u8(p, readOnly ? 0x01 : 0x00);
    diskproto::write_u8(p, static_cast<std::uint8_t>(fujinet::disk::ImageType::Raw));
    diskproto::write_u16le(p, kSectorSize);
    diskproto::write_lp_u16_string(p, kUri);
    return p;
}

inline std::vector<std::uint8_t> read_payload(std::uint32_t lba)
{
    std::vector<std::uint8_t> p;
    diskproto::write_u8(p, kProto);
    diskproto::write_u8(p, kSlot);
    diskproto::write_u32le(p, lba);
    diskproto::write_u16le(p, kSectorSize);
    return p;
}

inline std::vector<std::uint8_t> write_payload(std::uint32_t lba, const std::vector<std::uint8_t>& sector)
{
    std::vector<std::uint8_t> p;
    diskproto::write_u8(p, kProto);
    diskproto::write_u8(p, kSlot);
    diskproto::write_u32le(p, lba);
    diskproto::write_u16le(p, static_cast<std::uint16_t>(sector.size()));
    p.insert(p.end(), sector.begin(), sector.end());
    return p;
}

inline std::vector<std::uint8_t> flush_payload()
{
    std::vector<std::uint8_t> p;
    diskproto::write_u8(p, kProto);
    diskproto::write_u8(p, kSlot);
    return p;
}

inline std::vector<std::uint8_t> unique_sector(std::uint8_t marker)
{
    std::vector<std::uint8_t> sector(kSectorSize, 0);
    sector[0] = 0x5A;
    sector[1] = marker;
    sector[2] = static_cast<std::uint8_t>(marker ^ 0xFF);
    return sector;
}

inline bool read_ok_contains(const IOResponse& resp, std::uint8_t marker)
{
    if (resp.status != StatusCode::Ok || resp.payload.size() < 11 + kSectorSize) return false;
    return resp.payload[11] == 0x5A && resp.payload[12] == marker;
}

} // namespace disk_parity
