#include "fujinet/io/devices/app_store_service.h"

#include "fujinet/io/devices/app_store_commands.h"
#include "fujinet/io/devices/byte_codec.h"

#include <string>

namespace fujinet::io {

namespace {

using bytecodec::Reader;
constexpr auto kVersion = protocol::APPSTORE_PROTOCOL_VERSION;

struct Prefix {
    std::string ns;
    std::string key;
};

bool parse_prefix(Reader& r, Prefix& out, bool requireKey)
{
    std::uint8_t version = 0;
    std::uint16_t nsLen = 0;
    std::uint16_t keyLen = 0;
    const std::uint8_t* ns = nullptr;
    const std::uint8_t* key = nullptr;
    if (!r.read_u8(version) || version != kVersion ||
        !r.read_u16le(nsLen) || nsLen == 0 || !r.read_bytes(ns, nsLen) ||
        !r.read_u16le(keyLen) || !r.read_bytes(key, keyLen)) {
        return false;
    }
    out.ns.assign(reinterpret_cast<const char*>(ns), nsLen);
    out.key.assign(reinterpret_cast<const char*>(key), keyLen);
    return AppStore::valid_namespace(out.ns) &&
           (requireKey ? AppStore::valid_key(out.key) : out.key.empty());
}

} // namespace

AppStoreService::AppStoreService(std::shared_ptr<AppStore> store)
    : _store(std::move(store))
{}

IOResponse AppStoreService::handle(const IORequest& request)
{
    switch (protocol::to_app_store_command(request.command)) {
    case protocol::AppStoreCommand::Stat: return handle_stat(request);
    case protocol::AppStoreCommand::Read: return handle_read(request);
    case protocol::AppStoreCommand::Write: return handle_write(request);
    case protocol::AppStoreCommand::Delete: return handle_delete(request);
    case protocol::AppStoreCommand::List: return handle_list(request);
    default: return make_base_response(request, StatusCode::Unsupported);
    }
}

IOResponse AppStoreService::handle_stat(const IORequest& request)
{
    auto resp = make_success_response(request);
    Reader r(request.payload.data(), request.payload.size());
    Prefix p{};
    if (!parse_prefix(r, p, true) || r.remaining() != 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }
    if (!_store || !_store->available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    AppStore::Stat result{};
    if (!_store->stat(p.ns, p.key, result)) {
        resp.status = StatusCode::IOError;
        return resp;
    }
    std::string out;
    bytecodec::write_u8(out, kVersion);
    bytecodec::write_u8(out, result.exists ? 0x01U : 0x00U);
    bytecodec::write_u16le(out, 0);
    bytecodec::write_u64le(out, result.sizeBytes);
    bytecodec::write_u64le(out, result.modifiedUnixTime);
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

IOResponse AppStoreService::handle_read(const IORequest& request)
{
    auto resp = make_success_response(request);
    Reader r(request.payload.data(), request.payload.size());
    Prefix p{};
    std::uint32_t offset = 0;
    std::uint16_t maxBytes = 0;
    if (!parse_prefix(r, p, true) || !r.read_u32le(offset) ||
        !r.read_u16le(maxBytes) || maxBytes == 0 || r.remaining() != 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }
    if (!_store || !_store->available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    AppStore::ReadResult result{};
    if (!_store->read(p.ns, p.key, offset, maxBytes, result)) {
        resp.status = StatusCode::IOError;
        return resp;
    }
    std::uint8_t flags = 0;
    if (result.eof) flags |= 0x01U;
    if (result.exists) flags |= 0x02U;
    std::string out;
    bytecodec::write_u8(out, kVersion);
    bytecodec::write_u8(out, flags);
    bytecodec::write_u16le(out, 0);
    bytecodec::write_u32le(out, result.offset);
    bytecodec::write_u16le(
        out, static_cast<std::uint16_t>(result.data.size()));
    if (!result.data.empty()) {
        bytecodec::write_bytes(out, result.data.data(), result.data.size());
    }
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

IOResponse AppStoreService::handle_write(const IORequest& request)
{
    auto resp = make_success_response(request);
    Reader r(request.payload.data(), request.payload.size());
    Prefix p{};
    std::uint32_t offset = 0;
    std::uint16_t dataLen = 0;
    const std::uint8_t* data = nullptr;
    if (!parse_prefix(r, p, true) || !r.read_u32le(offset) ||
        !r.read_u16le(dataLen) || !r.read_bytes(data, dataLen) ||
        r.remaining() != 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }
    if (!_store || !_store->available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    AppStore::WriteResult result{};
    if (!_store->write(p.ns, p.key, offset, data, dataLen, result)) {
        resp.status = StatusCode::IOError;
        return resp;
    }
    std::string out;
    bytecodec::write_u8(out, kVersion);
    bytecodec::write_u8(out, 0);
    bytecodec::write_u16le(out, 0);
    bytecodec::write_u32le(out, result.offset);
    bytecodec::write_u16le(out, result.written);
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

IOResponse AppStoreService::handle_delete(const IORequest& request)
{
    auto resp = make_success_response(request);
    Reader r(request.payload.data(), request.payload.size());
    Prefix p{};
    if (!parse_prefix(r, p, true) || r.remaining() != 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }
    if (!_store || !_store->available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    AppStore::DeleteResult result{};
    if (!_store->remove(p.ns, p.key, result)) {
        resp.status = StatusCode::IOError;
        return resp;
    }
    std::string out;
    bytecodec::write_u8(out, kVersion);
    bytecodec::write_u8(out, result.deleted ? 0x01U : 0x00U);
    bytecodec::write_u16le(out, 0);
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

IOResponse AppStoreService::handle_list(const IORequest& request)
{
    auto resp = make_success_response(request);
    Reader r(request.payload.data(), request.payload.size());
    Prefix p{};
    std::uint16_t startIndex = 0;
    std::uint16_t maxPayloadBytes = 0;
    if (!parse_prefix(r, p, false) || !r.read_u16le(startIndex) ||
        !r.read_u16le(maxPayloadBytes) || maxPayloadBytes == 0 ||
        r.remaining() != 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }
    if (!_store || !_store->available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    AppStore::ListResult result{};
    if (!_store->list(p.ns, startIndex, maxPayloadBytes, result)) {
        resp.status = StatusCode::IOError;
        return resp;
    }
    std::string out;
    bytecodec::write_u8(out, kVersion);
    bytecodec::write_u8(out, result.more ? 0x01U : 0x00U);
    bytecodec::write_u16le(out, 0);
    bytecodec::write_u16le(out, result.startIndex);
    bytecodec::write_u16le(
        out, static_cast<std::uint16_t>(result.keys.size()));
    const auto keysLenPos = out.size();
    bytecodec::write_u16le(out, 0);
    const auto keysStart = out.size();
    for (const auto& key : result.keys) {
        bytecodec::write_u16le(out, static_cast<std::uint16_t>(key.size()));
        bytecodec::write_bytes(out, key.data(), key.size());
    }
    const auto keysLen = static_cast<std::uint16_t>(out.size() - keysStart);
    out[keysLenPos] = static_cast<char>(keysLen & 0xFFU);
    out[keysLenPos + 1] = static_cast<char>((keysLen >> 8U) & 0xFFU);
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

} // namespace fujinet::io
