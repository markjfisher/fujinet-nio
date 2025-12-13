#include "fujinet/io/devices/file_device.h"

#include "fujinet/core/logging.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/io/core/io_message.h"

// Commands + to_file_command helper
#include "fujinet/io/devices/file_commands.h"

// Binary codec
#include "fujinet/io/devices/file_codec.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <string_view>
#include <vector>

namespace fujinet::io {

using fujinet::fs::FileInfo;
using fujinet::fs::IFile;
using fujinet::fs::IFileSystem;
using fujinet::fs::StorageManager;
using fujinet::io::fileproto::Reader;

static const char* TAG = "file_device";

static constexpr std::uint8_t FILEPROTO_VERSION = 1;

static IOResponse make_base_response(const IORequest& req, StatusCode status)
{
    IOResponse resp;
    resp.id       = req.id;
    resp.deviceId = req.deviceId;
    resp.command  = req.command;
    resp.status   = status;
    return resp;
}

// Common request prefix:
// u8 version
// u8 fsNameLen
// u8[] fsName
// u16 pathLen (LE)
// u8[] path
struct CommonPrefix {
    std::string fs;
    std::string path;
};

static bool parse_common_prefix(Reader& r, CommonPrefix& out)
{
    std::uint8_t ver = 0;
    if (!r.read_u8(ver) || ver != FILEPROTO_VERSION) {
        return false;
    }

    std::uint8_t fsLen = 0;
    if (!r.read_u8(fsLen)) return false;

    const std::uint8_t* fsPtr = nullptr;
    if (!r.read_bytes(fsPtr, fsLen)) return false;
    out.fs.assign(reinterpret_cast<const char*>(fsPtr), fsLen);

    std::uint16_t pathLen = 0;
    if (!r.read_u16le(pathLen)) return false;

    const std::uint8_t* pathPtr = nullptr;
    if (!r.read_bytes(pathPtr, pathLen)) return false;
    out.path.assign(reinterpret_cast<const char*>(pathPtr), pathLen);

    if (out.fs.empty() || out.path.empty()) return false;
    return true;
}

static std::uint64_t to_unix_seconds(std::chrono::system_clock::time_point tp)
{
    if (tp == std::chrono::system_clock::time_point{}) return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count());
}

FileDevice::FileDevice(StorageManager& storage)
    : _storage(storage)
{}

IOResponse FileDevice::handle(const IORequest& request)
{
    auto cmd = protocol::to_file_command(request.command);

    switch (cmd) {
        case protocol::FileCommand::Stat:
            return handle_stat(request);
        case protocol::FileCommand::ListDirectory:
            return handle_list_directory(request);
        case protocol::FileCommand::ReadFile:
            return handle_read_file(request);
        case protocol::FileCommand::WriteFile:
            return handle_write_file(request);
        default:
            return make_base_response(request, StatusCode::Unsupported);
    }
}

// --------------------
// Stat (0x01)
// --------------------
IOResponse FileDevice::handle_stat(const IORequest& request)
{
    auto resp = make_base_response(request, StatusCode::Ok);

    Reader r(request.payload.data(), request.payload.size());
    CommonPrefix p{};
    if (!parse_common_prefix(r, p)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    auto* fs = _storage.get(p.fs);
    if (!fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    FileInfo info{};
    const bool exists = fs->stat(p.path, info);

    std::uint8_t flags = 0;
    if (exists) {
        flags |= 0x02; // exists
        if (info.isDirectory) flags |= 0x01; // isDir
    }

    const std::uint64_t size  = exists ? info.sizeBytes : 0;
    const std::uint64_t mtime = exists ? to_unix_seconds(info.modifiedTime) : 0;

    // Build response payload:
    // u8 version
    // u8 flags (bit0=isDir, bit1=exists)
    // u16 reserved=0
    // u64 sizeBytes
    // u64 modifiedUnixTime
    std::string out;
    out.reserve(1 + 1 + 2 + 8 + 8);

    out.push_back(static_cast<char>(FILEPROTO_VERSION));
    out.push_back(static_cast<char>(flags));
    fileproto::write_u16le(out, 0);
    fileproto::write_u64le(out, size);
    fileproto::write_u64le(out, mtime);

    resp.payload.assign(out.begin(), out.end());
    return resp;
}

// --------------------
// ListDirectory (0x02)
// --------------------
IOResponse FileDevice::handle_list_directory(const IORequest& request)
{
    auto resp = make_base_response(request, StatusCode::Ok);

    Reader r(request.payload.data(), request.payload.size());
    CommonPrefix p{};
    if (!parse_common_prefix(r, p)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint16_t startIndex = 0;
    std::uint16_t maxEntries = 0;
    if (!r.read_u16le(startIndex) || !r.read_u16le(maxEntries) || maxEntries == 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    auto* fs = _storage.get(p.fs);
    if (!fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    std::vector<FileInfo> entries;
    if (!fs->listDirectory(p.path, entries)) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    // Basename helper
    auto basename = [](const std::string& s) -> std::string_view {
        auto pos = s.find_last_of('/');
        if (pos == std::string::npos) return std::string_view{s};
        if (pos + 1 >= s.size()) return std::string_view{s};
        return std::string_view{s}.substr(pos + 1);
    };

    const std::size_t total = entries.size();
    const std::size_t start = (startIndex < total) ? startIndex : total;
    const std::size_t end = std::min<std::size_t>(start + maxEntries, total);
    const std::uint16_t returned = static_cast<std::uint16_t>(end - start);
    const bool more = (end < total);

    // Response:
    // u8 version
    // u8 flags bit0=more
    // u16 reserved
    // u16 returnedCount
    // entries...
    std::string out;
    out.reserve(64 + returned * 32);

    out.push_back(static_cast<char>(FILEPROTO_VERSION));
    out.push_back(static_cast<char>(more ? 0x01 : 0x00));
    fileproto::write_u16le(out, 0);
    fileproto::write_u16le(out, returned);

    for (std::size_t i = start; i < end; ++i) {
        const auto& e = entries[i];
        const auto name = basename(e.path);

        std::uint8_t eflags = e.isDirectory ? 0x01 : 0x00;
        out.push_back(static_cast<char>(eflags));

        const std::uint8_t nameLen =
            static_cast<std::uint8_t>(std::min<std::size_t>(name.size(), 255));
        out.push_back(static_cast<char>(nameLen));
        out.append(name.data(), nameLen);

        fileproto::write_u64le(out, e.sizeBytes);
        fileproto::write_u64le(out, to_unix_seconds(e.modifiedTime));
    }

    resp.payload.assign(out.begin(), out.end());
    return resp;
}

// --------------------
// ReadFile (0x03)
// --------------------
IOResponse FileDevice::handle_read_file(const IORequest& request)
{
    auto resp = make_base_response(request, StatusCode::Ok);

    Reader r(request.payload.data(), request.payload.size());
    CommonPrefix p{};
    if (!parse_common_prefix(r, p)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint32_t offset = 0;
    std::uint16_t maxBytes = 0;
    if (!r.read_u32le(offset) || !r.read_u16le(maxBytes) || maxBytes == 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    auto* fs = _storage.get(p.fs);
    if (!fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    auto file = fs->open(p.path, "rb");
    if (!file) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    if (!file->seek(static_cast<std::uint64_t>(offset))) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    // Response:
    // u8 version
    // u8 flags bit0=eof bit1=truncated
    // u16 reserved
    // u32 offset (echo)
    // u16 dataLen
    // data...
    std::string out;
    out.reserve(1 + 1 + 2 + 4 + 2 + maxBytes);

    out.push_back(static_cast<char>(FILEPROTO_VERSION));
    out.push_back(static_cast<char>(0)); // flags placeholder
    fileproto::write_u16le(out, 0);
    fileproto::write_u32le(out, offset);

    const std::size_t dataLenPos = out.size();
    fileproto::write_u16le(out, 0); // placeholder

    const std::size_t dataStart = out.size();
    out.resize(dataStart + maxBytes);

    const std::size_t n = file->read(out.data() + dataStart, maxBytes);

    out.resize(dataStart + n);

    // Fill dataLen
    out[dataLenPos + 0] = static_cast<char>(n & 0xFF);
    out[dataLenPos + 1] = static_cast<char>((n >> 8) & 0xFF);

    // Best-effort flags:
    // eof if we couldn't fill the request (n < maxBytes)
    // truncated if we filled exactly maxBytes (caller may need another read)
    std::uint8_t flags = 0;
    if (n < maxBytes) flags |= 0x01;       // eof-ish
    if (n == maxBytes) flags |= 0x02;      // truncated-ish (more may exist)
    out[1] = static_cast<char>(flags);

    resp.payload.assign(out.begin(), out.end());
    return resp;
}

// --------------------
// WriteFile (0x04)
// --------------------
IOResponse FileDevice::handle_write_file(const IORequest& request)
{
    auto resp = make_base_response(request, StatusCode::Ok);

    Reader r(request.payload.data(), request.payload.size());
    CommonPrefix p{};
    if (!parse_common_prefix(r, p)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint32_t offset = 0;
    std::uint16_t dataLen = 0;
    if (!r.read_u32le(offset) || !r.read_u16le(dataLen)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    const std::uint8_t* dataPtr = nullptr;
    if (!r.read_bytes(dataPtr, dataLen)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    auto* fs = _storage.get(p.fs);
    if (!fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    // v1 open mode convention:
    // offset==0 => create/truncate
    // offset>0  => open existing read/write (best effort)
    const char* mode = (offset == 0) ? "wb" : "r+b";
    auto file = fs->open(p.path, mode);
    if (!file && offset > 0) {
        // If file didn't exist, allow creation when offset>0 too.
        file = fs->open(p.path, "wb");
    }
    if (!file) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    if (offset > 0 && !file->seek(static_cast<std::uint64_t>(offset))) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    const std::size_t written = file->write(dataPtr, dataLen);
    (void)file->flush();

    // Response:
    // u8 version
    // u8 flags (0)
    // u16 reserved
    // u32 offset
    // u16 writtenLen
    std::string out;
    out.reserve(1 + 1 + 2 + 4 + 2);

    out.push_back(static_cast<char>(FILEPROTO_VERSION));
    out.push_back(static_cast<char>(0));
    fileproto::write_u16le(out, 0);
    fileproto::write_u32le(out, offset);
    fileproto::write_u16le(out, static_cast<std::uint16_t>(written));

    resp.payload.assign(out.begin(), out.end());
    return resp;
}

} // namespace fujinet::io
