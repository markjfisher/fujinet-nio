#include "fujinet/io/devices/slot_catalog_service.h"

#include "fujinet/io/devices/byte_codec.h"
#include "fujinet/io/devices/slot_catalog_commands.h"
#include "fujinet/io/host_state.h"

#include <string>

namespace fujinet::io {

namespace {

using bytecodec::Reader;
constexpr auto kVersion = protocol::SLOT_CATALOG_PROTOCOL_VERSION;

void encode_entry(std::string& out, const SlotCatalog::Entry& entry)
{
    bytecodec::write_u8(out, kVersion);
    bytecodec::write_u8(out, entry.flags);
    bytecodec::write_u8(out, entry.index);
    bytecodec::write_u16le(out, static_cast<std::uint16_t>(entry.uri.size()));
    bytecodec::write_bytes(out, entry.uri.data(), entry.uri.size());
}

} // namespace

SlotCatalogService::SlotCatalogService(
    fs::StorageManager& storage, std::shared_ptr<AppStore> store)
    : _storage(storage), _store(std::move(store)), _catalog(*_store)
{}

IOResponse SlotCatalogService::handle(const IORequest& request)
{
    switch (protocol::to_slot_catalog_command(request.command)) {
    case protocol::SlotCatalogCommand::Get: return handle_get(request);
    case protocol::SlotCatalogCommand::Put: return handle_put(request);
    case protocol::SlotCatalogCommand::Delete: return handle_delete(request);
    case protocol::SlotCatalogCommand::Range: return handle_range(request);
    default: return make_base_response(request, StatusCode::Unsupported);
    }
}

IOResponse SlotCatalogService::handle_get(const IORequest& request)
{
    auto resp = make_success_response(request);
    Reader r(request.payload.data(), request.payload.size());
    std::uint8_t version = 0;
    std::uint8_t index = 0;
    if (!r.read_u8(version) || version != kVersion || !r.read_u8(index) ||
        r.remaining() != 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }
    if (!_store->available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    SlotCatalog::Entry entry{};
    if (!_catalog.get(index, entry)) {
        resp.status = StatusCode::IOError;
        return resp;
    }
    if ((entry.flags & protocol::slot_catalog::kEntryValid) == 0) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }
    std::string out;
    encode_entry(out, entry);
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

IOResponse SlotCatalogService::handle_put(const IORequest& request)
{
    auto resp = make_success_response(request);
    Reader r(request.payload.data(), request.payload.size());
    std::uint8_t version = 0;
    std::uint8_t index = 0;
    std::uint8_t flags = 0;
    std::uint16_t targetLen = 0;
    const std::uint8_t* targetData = nullptr;
    if (!r.read_u8(version) || version != kVersion || !r.read_u8(index) ||
        !r.read_u8(flags) ||
        (flags & ~protocol::slot_catalog::kEntryReadOnly) != 0 ||
        !r.read_u16le(targetLen) || targetLen == 0 ||
        !r.read_bytes(targetData, targetLen) || r.remaining() != 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }
    if (!_store->available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    const std::string target(
        reinterpret_cast<const char*>(targetData), targetLen);
    std::string canonicalUri;
    HostState hostState(_storage);
    if (!hostState.resolve_target(target, canonicalUri, nullptr)) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    SlotCatalog::Entry entry{};
    if (!_catalog.put(index, flags, canonicalUri, entry)) {
        resp.status = StatusCode::IOError;
        return resp;
    }
    std::string out;
    encode_entry(out, entry);
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

IOResponse SlotCatalogService::handle_delete(const IORequest& request)
{
    auto resp = make_success_response(request);
    Reader r(request.payload.data(), request.payload.size());
    std::uint8_t version = 0;
    std::uint8_t index = 0;
    if (!r.read_u8(version) || version != kVersion || !r.read_u8(index) ||
        r.remaining() != 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }
    if (!_store->available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }
    bool deleted = false;
    if (!_catalog.remove(index, deleted)) {
        resp.status = StatusCode::IOError;
        return resp;
    }
    std::string out;
    bytecodec::write_u8(out, kVersion);
    bytecodec::write_u8(
        out, deleted ? protocol::slot_catalog::kDeleteRemoved : 0);
    bytecodec::write_u8(out, index);
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

IOResponse SlotCatalogService::handle_range(const IORequest& request)
{
    auto resp = make_success_response(request);
    Reader r(request.payload.data(), request.payload.size());
    std::uint8_t version = 0;
    std::uint8_t lower = 0;
    std::uint8_t upper = 0;
    std::uint8_t cursor = 0;
    std::uint8_t flags = 0;
    std::uint8_t maxUriBytes = 0;
    std::uint16_t maxPayloadBytes = 0;
    if (!r.read_u8(version) || version != kVersion ||
        !r.read_u8(lower) || !r.read_u8(upper) || !r.read_u8(cursor) ||
        !r.read_u8(flags) || !r.read_u8(maxUriBytes) ||
        !r.read_u16le(maxPayloadBytes) || r.remaining() != 0 ||
        (flags & ~(protocol::slot_catalog::kRequestTailUri |
                   protocol::slot_catalog::kRequestFormatted)) != 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }
    if (!_store->available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    SlotCatalog::RangeResult result{};
    if (!_catalog.range(lower, upper, cursor, flags, maxUriBytes,
                        maxPayloadBytes, result)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::string out;
    bytecodec::write_u8(out, kVersion);
    std::uint8_t responseFlags =
        result.more ? protocol::slot_catalog::kResponseMore : 0;
    if ((flags & protocol::slot_catalog::kRequestFormatted) != 0) {
        responseFlags |= protocol::slot_catalog::kResponseFormatted;
    }
    bytecodec::write_u8(out, responseFlags);
    bytecodec::write_u8(out, result.nextIndex);
    bytecodec::write_u8(
        out, static_cast<std::uint8_t>(result.presence.size()));
    bytecodec::write_u8(
        out, static_cast<std::uint8_t>(result.entries.size()));
    const auto entriesLenPos = out.size();
    bytecodec::write_u16le(out, 0);
    bytecodec::write_bytes(
        out, result.presence.data(), result.presence.size());
    const auto entriesStart = out.size();
    for (const auto& entry : result.entries) {
        if ((flags & protocol::slot_catalog::kRequestFormatted) != 0) {
            out += std::to_string(entry.index);
            out += ": ";
            out += (entry.flags & protocol::slot_catalog::kEntryValid) != 0
                ? entry.uri : "<invalid>";
            out.push_back('\n');
        } else {
            bytecodec::write_u8(out, entry.index);
            bytecodec::write_u8(out, entry.flags);
            bytecodec::write_u8(
                out, static_cast<std::uint8_t>(entry.uri.size()));
            bytecodec::write_bytes(out, entry.uri.data(), entry.uri.size());
        }
    }
    const auto entriesLen =
        static_cast<std::uint16_t>(out.size() - entriesStart);
    out[entriesLenPos] = static_cast<char>(entriesLen & 0xFFU);
    out[entriesLenPos + 1] =
        static_cast<char>((entriesLen >> 8U) & 0xFFU);
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

} // namespace fujinet::io
