#include "fujinet/io/devices/network_device.h"

#include "fujinet/core/logging.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/json_content_translator.h"
#include "fujinet/io/devices/network_content_profile.h"

#include "fujinet/io/devices/net_codec.h"
#include "fujinet/io/devices/net_commands.h"
#include "fujinet/io/devices/network_protocol.h"
#include "fujinet/io/devices/network_protocol_registry.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fujinet::io {

using fujinet::io::netproto::Reader;
using fujinet::io::protocol::NetworkCommand;

static constexpr std::uint8_t NET_OPEN_FLAG_STREAM_NO_PROBE = 0x10;
static constexpr std::uint8_t NET_READ_FLAG_EOF = 0x01;
static constexpr std::uint8_t NET_READ_FLAG_TRUNCATED = 0x02;
static constexpr std::uint8_t NET_READ_FLAG_MORE_AVAILABLE = 0x04;

static std::vector<std::uint8_t> to_vec(const std::string& s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

static std::string to_lower_ascii(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

static bool extract_scheme_lower(std::string_view url, std::string& outSchemeLower)
{
    const auto pos = url.find("://");
    if (pos == std::string_view::npos || pos == 0) {
        return false;
    }
    outSchemeLower = to_lower_ascii(url.substr(0, pos));
    return !outSchemeLower.empty();
}

NetworkDevice::NetworkDevice(ProtocolRegistry registry)
    : _registry(std::move(registry))
{
}

void NetworkDevice::poll()
{
    ++_tickNow;

    // Shorter timeout for "body upload in progress" sessions so we don't
    // wedge the session table if the host dies mid-upload.
    // With a 50ms tick, 20 ticks = 1s.
    static constexpr std::uint64_t BODY_UPLOAD_TIMEOUT_TICKS = 20ull * 10ull; // ~10s

    for (auto& s : _sessions) {
        if (!s.active || !s.proto) continue;

        // Allow backend to progress (future async backends)
        s.proto->poll();

        // If backend can signal progress/completion later, we can update s.completed
        // and/or touch(s) here when progress is made.
        // const std::uint64_t age  = _tickNow - s.createdTick;
        const std::uint64_t idle = _tickNow - s.lastActivityTick;

        // If we're waiting for request body data and the client goes away,
        // reap quickly to free the session slot.
        if (s.awaitingBody && idle > BODY_UPLOAD_TIMEOUT_TICKS) {
            close_and_free(s);
            continue;
        }

        // Reap dead/leaked handles:
        // - Enforce idle timeout
        if (idle > IDLE_TIMEOUT_TICKS) {
            close_and_free(s);
        }
    }
}

static void write_common_prefix(std::string& out, std::uint8_t version, std::uint8_t flags)
{
    netproto::write_u8(out, version);
    netproto::write_u8(out, flags);
    netproto::write_u16le(out, 0); // reserved
}

static bool check_version(Reader& r, std::uint8_t expected)
{
    std::uint8_t ver = 0;
    return r.read_u8(ver) && ver == expected;
}

static bool read_translation_type(Reader& r, ContentTranslationType& type)
{
    std::uint8_t raw = 0;
    if (!r.read_u8(raw)) {
        return false;
    }

    type = static_cast<ContentTranslationType>(raw);
    return true;
}

static bool read_translation_config(Reader& r, TranslationConfig& config)
{
    config = TranslationConfig{};

    if (!read_translation_type(r, config.type)) {
        return false;
    }

    if (!r.read_u8(config.flags)) {
        return false;
    }

    std::string_view selector;
    if (!r.read_lp_u16_string(selector)) {
        return false;
    }

    config.selector.assign(selector.data(), selector.size());
    return true;
}

struct OpenExtensions {
    TranslationConfig translation;
    RequestContentProfile contentProfile{RequestContentProfile::None};
};

static bool read_optional_open_extensions(Reader& r, OpenExtensions& extensions)
{
    extensions = OpenExtensions{};

    if (r.remaining() == 0) {
        return true;
    }

    std::uint32_t extFlags = 0;
    if (!r.read_u32le(extFlags)) {
        return false;
    }

    if ((extFlags & NETWORK_OPEN_EXT_TRANSLATION) != 0) {
        if (!read_translation_config(r, extensions.translation)) {
            return false;
        }
    }

    if ((extFlags & NETWORK_OPEN_EXT_CONTENT_PROFILE) != 0) {
        std::uint8_t raw = 0;
        if (!r.read_u8(raw)) {
            return false;
        }
        extensions.contentProfile = static_cast<RequestContentProfile>(raw);
    }

    const std::uint32_t knownFlags =
        NETWORK_OPEN_EXT_TRANSLATION | NETWORK_OPEN_EXT_CONTENT_PROFILE;
    return (extFlags & ~knownFlags) == 0;
}

static bool header_name_equals_lower(std::string_view name, std::string_view nameLower)
{
    if (name.size() != nameLower.size()) {
        return false;
    }

    for (std::size_t i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i]))
            != static_cast<unsigned char>(nameLower[i])) {
            return false;
        }
    }

    return true;
}

static bool has_request_header_lower(
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::string_view nameLower)
{
    for (const auto& kv : headers) {
        if (header_name_equals_lower(kv.first, nameLower)) {
            return true;
        }
    }

    return false;
}

static void apply_content_profile_headers(
    std::vector<std::pair<std::string, std::string>>& headers,
    RequestContentProfile profile)
{
    if (profile == RequestContentProfile::None) {
        return;
    }

    if (!has_request_header_lower(headers, "content-type")) {
        switch (profile) {
            case RequestContentProfile::JsonBody:
                headers.emplace_back("Content-Type", "application/json");
                break;
            case RequestContentProfile::FormBody:
                headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
                break;
            case RequestContentProfile::TextBody:
                headers.emplace_back("Content-Type", "text/plain");
                break;
            case RequestContentProfile::None:
                break;
        }
    }
}

bool NetworkDevice::translation_enabled(const Session& s) noexcept
{
    return s.translation.enabled() && static_cast<bool>(s.translator);
}

std::unique_ptr<IContentTranslator> NetworkDevice::make_translator(ContentTranslationType type)
{
    switch (type) {
        case ContentTranslationType::None:
            return nullptr;
        case ContentTranslationType::Json:
            return std::make_unique<JsonContentTranslator>();
        case ContentTranslationType::Xml:
        case ContentTranslationType::Rss:
            return nullptr;
    }

    return nullptr;
}

void NetworkDevice::reset_translation(Session& s) noexcept
{
    s.translation = TranslationConfig{};
    if (s.translator) {
        s.translator->reset();
    }
    s.translator.reset();
    s.responseBodyCache.clear();
    s.responseBodyCached = false;
    s.responseBodyBuffering = false;
    s.translationReady = false;
    s.translatedResultSize = 0;
}

StatusCode NetworkDevice::configure_translation(Session& s, const TranslationConfig& config)
{
    if (!is_known_translation_type(config.type)) {
        return StatusCode::InvalidRequest;
    }

    std::string cachedBody;
    const bool hadCachedBody = s.responseBodyCached;
    if (hadCachedBody) {
        cachedBody = std::move(s.responseBodyCache);
    }

    reset_translation(s);

    if (hadCachedBody) {
        s.responseBodyCache = std::move(cachedBody);
        s.responseBodyCached = true;
    }

    if (!config.enabled()) {
        return StatusCode::Ok;
    }

    auto translator = make_translator(config.type);
    if (!translator) {
        return StatusCode::Unsupported;
    }

    const StatusCode st = translator->configure(config);
    if (st != StatusCode::Ok) {
        return st;
    }

    s.translation = config;
    s.translator = std::move(translator);
    return StatusCode::Ok;
}

StatusCode NetworkDevice::buffer_translation_body(Session& s)
{
    if (!translation_enabled(s)) {
        return StatusCode::InvalidRequest;
    }

    if (s.responseBodyCached) {
        return StatusCode::Ok;
    }

    s.responseBodyBuffering = true;
    std::uint8_t tmp[4096];

    while (true) {
        std::uint16_t chunk = 0;
        bool chunkEof = false;
        bool chunkMoreAvailable = false;
        const StatusCode st = s.proto->read_body(
            static_cast<std::uint32_t>(s.responseBodyCache.size()),
            tmp,
            sizeof(tmp),
            chunk,
            chunkEof,
            chunkMoreAvailable);
        if (st != StatusCode::Ok) {
            s.responseBodyBuffering = false;
            return st;
        }

        if (chunk > 0) {
            s.responseBodyCache.append(reinterpret_cast<const char*>(tmp), chunk);
        }

        if (chunkEof) {
            s.responseBodyCached = true;
            s.responseBodyBuffering = false;
            return StatusCode::Ok;
        }

        if (chunk == 0) {
            s.responseBodyBuffering = false;
            return StatusCode::NotReady;
        }
    }
}

StatusCode NetworkDevice::finalize_translation(Session& s)
{
    if (!translation_enabled(s)) {
        return StatusCode::InvalidRequest;
    }

    s.translator->reset();
    const StatusCode appendSt = s.translator->append_body(
        reinterpret_cast<const std::uint8_t*>(s.responseBodyCache.data()),
        s.responseBodyCache.size());
    if (appendSt != StatusCode::Ok) {
        s.translationReady = false;
        return appendSt;
    }

    const StatusCode finalizeSt = s.translator->finalize();
    if (finalizeSt != StatusCode::Ok) {
        s.translationReady = false;
        return finalizeSt;
    }

    s.translatedResultSize = s.translator->translated_size();
    s.translationReady = true;
    return StatusCode::Ok;
}

StatusCode NetworkDevice::ensure_translation_ready(Session& s)
{
    if (!translation_enabled(s)) {
        return StatusCode::Ok;
    }

    if (s.translationReady) {
        return StatusCode::Ok;
    }

    const StatusCode bufferSt = buffer_translation_body(s);
    if (bufferSt != StatusCode::Ok) {
        return bufferSt;
    }

    return finalize_translation(s);
}

IOResponse NetworkDevice::handle(const IORequest& request)
{
    auto cmd = protocol::to_network_command(request.command);

    auto session_for_handle = [this](std::uint16_t handle) -> Session* {
        const auto idx = handle_index(handle);
        const auto gen = handle_generation(handle);
        if (idx >= MAX_SESSIONS) return nullptr;
        auto& s = _sessions[idx];
        if (!s.active) return nullptr;
        if (s.generation != gen) return nullptr;
        return &s;
    };

    switch (cmd) {
        case NetworkCommand::Open: {
            auto resp = make_success_response(request);
        
            Reader r(request.payload.data(), request.payload.size());
            if (!check_version(r, NETPROTO_VERSION)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
        
            std::uint8_t method = 0;
            std::uint8_t flags = 0;
            if (!r.read_u8(method) || !r.read_u8(flags)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
        
            // url: u16 len + bytes
            std::string_view urlView;
            if (!r.read_lp_u16_string(urlView)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
        
            std::uint16_t headerCount = 0;
            if (!r.read_u16le(headerCount)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
        
            std::vector<std::pair<std::string, std::string>> headers;
            headers.reserve(headerCount);
            for (std::uint16_t i = 0; i < headerCount; ++i) {
                std::string_view k, v;
                if (!r.read_lp_u16_string(k) || !r.read_lp_u16_string(v)) {
                    resp.status = StatusCode::InvalidRequest;
                    return resp;
                }
                headers.emplace_back(std::string(k), std::string(v));
            }

            std::uint32_t bodyLenHint = 0;
            if (!r.read_u32le(bodyLenHint)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            
            // NEW: response header allowlist (u16 count, then count * lp_u16 string)
            std::uint16_t respHeaderCount = 0;
            if (!r.read_u16le(respHeaderCount)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            
            std::vector<std::string> respHeaderNamesLower;
            respHeaderNamesLower.reserve(respHeaderCount);
            
            for (std::uint16_t i = 0; i < respHeaderCount; ++i) {
                std::string_view name;
                if (!r.read_lp_u16_string(name)) {
                    resp.status = StatusCode::InvalidRequest;
                    return resp;
                }
                if (name.empty()) {
                    resp.status = StatusCode::InvalidRequest;
                    return resp;
                }
                respHeaderNamesLower.emplace_back(to_lower_ascii(name));
            }

            OpenExtensions extensions;
            if (!read_optional_open_extensions(r, extensions)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            if (r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            if (!is_known_translation_type(extensions.translation.type)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            if (!is_known_content_profile(extensions.contentProfile)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
                    
            // Determine URL scheme -> protocol backend
            std::string schemeLower;
            if (!extract_scheme_lower(urlView, schemeLower)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
        
            auto proto = _registry.create(schemeLower);
            if (!proto) {
                resp.status = StatusCode::Unsupported;
                return resp;
            }
        
            apply_content_profile_headers(headers, extensions.contentProfile);

            NetworkOpenRequest openReq{};
            openReq.method = method;
            openReq.flags = flags;
            openReq.url.assign(urlView.data(), urlView.size());
            openReq.headers = std::move(headers);
            openReq.bodyLenHint = bodyLenHint;
            openReq.responseHeaderNamesLower = std::move(respHeaderNamesLower);
            
            const bool methodAllowsBody = (method == 2 /*POST*/ || method == 3 /*PUT*/);
            // Keep v1 simple + deterministic: only POST/PUT may declare a body.
            if (bodyLenHint > 0 && !methodAllowsBody) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            
            // Optional extension: allow POST/PUT with unknown-length bodies.
            // This is enabled explicitly via an Open flag (bit2).
            const bool bodyLenUnknown = methodAllowsBody && (bodyLenHint == 0) && ((flags & 0x04) != 0);
            if ((flags & 0x04) != 0 && !methodAllowsBody) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            // ---- (C) Reserve slot BEFORE proto->open() ----
            auto reserve_slot = [this]() -> Session* {
                for (auto& s : _sessions) {
                    if (s.active) continue;
        
                    s.active = true; // reserve
                    s.generation = static_cast<std::uint8_t>(s.generation + 1);
                    if (s.generation == 0) s.generation = 1;
        
                    s.createdTick = _tickNow;
                    s.lastActivityTick = _tickNow;
                    s.completed = false;
        
                    // Clear any stale fields just in case
                    s.method = 0;
                    s.flags = 0;
                    s.url.clear();
                    s.expectedBodyLen = 0;
                    s.receivedBodyLen = 0;
                    s.nextBodyOffset  = 0;
                    s.awaitingBody    = false;
                    s.bodyLenUnknown  = false;
                    reset_translation(s);
                    if (s.proto) {
                        s.proto->close();
                        s.proto.reset();
                    }
        
                    return &s;
                }
                return nullptr;
            };
        
            Session* slot = reserve_slot();
        
            // ---- (D) Optional eviction (allow_evict flag): if busy, evict LRU and retry once ----
            const bool allowEvict = (flags & 0x08) != 0;

            if (!slot && allowEvict) {
                if (auto* victim = pick_lru_victim()) {
                    // Potential future tightening:
                    // only evict completed or sufficiently idle sessions.
                    close_and_free(*victim);
                    slot = reserve_slot();
                }
            }

            if (!slot) {
                // No free slots.
                // In strict mode (allow_evict=0) we MUST return DeviceBusy.
                // In eviction mode we only reach here if eviction was not possible.
                proto->close();
                resp.status = StatusCode::DeviceBusy;
                return resp;
            }

            // Actually open the protocol now that we own a slot.
            const StatusCode st = proto->open(openReq);
            if (st != StatusCode::Ok) {
                // Release slot on failure.
                close_and_free(*slot);
                resp.status = st;
                return resp;
            }
        
            // Fill the reserved session now that open succeeded.
            slot->method = method;
            slot->flags = flags;
            slot->url = openReq.url;
            slot->proto = std::move(proto);

            // Body streaming state is enforced at NetworkDevice layer (cheap bookkeeping).
            const bool needsBodyWrite = methodAllowsBody && (bodyLenHint > 0 || bodyLenUnknown);
            slot->expectedBodyLen = (methodAllowsBody && bodyLenHint > 0) ? bodyLenHint : 0;
            slot->receivedBodyLen = 0;
            slot->nextBodyOffset  = 0;
            slot->awaitingBody    = needsBodyWrite;
            slot->bodyLenUnknown  = bodyLenUnknown;

            const StatusCode translationSt = configure_translation(*slot, extensions.translation);
            if (translationSt != StatusCode::Ok) {
                close_and_free(*slot);
                resp.status = translationSt;
                return resp;
            }

            touch(*slot);
        
            auto session_index = [this](const Session* s) -> std::uint8_t {
                // std::array is contiguous; pointer arithmetic is valid.
                return static_cast<std::uint8_t>(s - _sessions.data());
            };

            const std::uint16_t handle = make_handle(session_index(slot), slot->generation);
        
            // Determine protocol capability flags
            std::uint8_t protoFlags = 0;
            if (slot->proto) {
                if (slot->proto->is_streaming()) {
                    protoFlags |= 0x04;
                }
                if (slot->proto->requires_sequential_read()) {
                    protoFlags |= 0x01;
                }
                if (slot->proto->requires_sequential_write()) {
                    protoFlags |= 0x02;
                }
            }
        
            // Response: version, flags(bit0 accepted, bit1 needs_body_write), reserved, handle, proto_flags
            std::string out;
            out.reserve(1 + 1 + 2 + 2 + 1);
        
            std::uint8_t oflags = 0x01; // accepted
            if (needsBodyWrite) oflags |= 0x02;
        
            write_common_prefix(out, NETPROTO_VERSION, oflags);
            netproto::write_u16le(out, handle);
            netproto::write_u8(out, protoFlags);
        
            resp.payload = to_vec(out);
            return resp;
        }

        case NetworkCommand::Close: {
            auto resp = make_success_response(request);
            Reader r(request.payload.data(), request.payload.size());
            if (!check_version(r, NETPROTO_VERSION)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::uint16_t handle = 0;
            if (!r.read_u16le(handle) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            auto* s = session_for_handle(handle);
            if (!s) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            touch(*s);
            close_and_free(*s);

            // Optional minimal response payload: version + reserved prefix
            std::string out;
            out.reserve(1 + 1 + 2);
            write_common_prefix(out, NETPROTO_VERSION, 0);
            resp.payload = to_vec(out);
            return resp;
        }

        case NetworkCommand::Info: {
            auto resp = make_success_response(request);
            Reader r(request.payload.data(), request.payload.size());
            if (!check_version(r, NETPROTO_VERSION)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::uint16_t handle = 0;
            if (!r.read_u16le(handle) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            auto* s = session_for_handle(handle);
            if (!s || !s->proto) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            touch(*s);

            // If request body hasn't been fully uploaded, response is not available yet.
            if (s->awaitingBody) {
                resp.status = StatusCode::NotReady;
                return resp;
            }

            if (translation_enabled(*s)) {
                const StatusCode translationSt = ensure_translation_ready(*s);
                if (translationSt != StatusCode::Ok) {
                    resp.status = translationSt;
                    return resp;
                }
            }

            NetworkInfo info{};
            const StatusCode st = s->proto->info(info);
            if (st != StatusCode::Ok) {
                resp.status = st;
                return resp;
            }

            std::uint8_t flags = 0;
            // bit0=headersAvailable, bit1=hasContentLength, bit2=hasHttpStatus
            const std::size_t hdrLen = info.headersBlock.size();
            if (hdrLen > 0) flags |= 0x01;
            if (info.hasHttpStatus) flags |= 0x04;

            // Translation view overrides body length while preserving transport metadata.
            if (translation_enabled(*s) && s->translationReady) {
                flags |= 0x02; // hasContentLength
                info.contentLength = s->translatedResultSize;
                info.hasContentLength = true;
            } else if (info.hasContentLength) {
                flags |= 0x02;
            }

            std::string out;
            out.reserve(1 + 1 + 2 + 2 + 2 + 8 + 4);

            write_common_prefix(out, NETPROTO_VERSION, flags);
            netproto::write_u16le(out, handle);
            netproto::write_u16le(out, info.hasHttpStatus ? info.httpStatus : 0);
            netproto::write_u64le(out, info.hasContentLength ? info.contentLength : 0);
            netproto::write_u32le(out, static_cast<std::uint32_t>(hdrLen));

            resp.payload = to_vec(out);
            return resp;
        }

        case NetworkCommand::InfoRead: {
            auto resp = make_success_response(request);
            Reader r(request.payload.data(), request.payload.size());
            if (!check_version(r, NETPROTO_VERSION)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::uint16_t handle = 0;
            std::uint32_t offset = 0;
            std::uint16_t maxBytes = 0;
            if (!r.read_u16le(handle) || !r.read_u32le(offset) || !r.read_u16le(maxBytes) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            auto* s = session_for_handle(handle);
            if (!s || !s->proto) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            touch(*s);

            if (s->awaitingBody) {
                resp.status = StatusCode::NotReady;
                return resp;
            }

            NetworkInfo info{};
            const StatusCode st = s->proto->info(info);
            if (st != StatusCode::Ok) {
                resp.status = st;
                return resp;
            }

            const std::size_t total = info.headersBlock.size();
            if (offset > total) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            const std::size_t remaining = total - offset;
            const std::size_t n = std::min<std::size_t>(remaining, maxBytes);

            std::uint8_t flags = 0;
            if (offset + n >= total) {
                flags |= 0x01;
            }
            if (n == maxBytes && offset + n < total) {
                flags |= 0x02;
            }

            std::string out;
            out.reserve(1 + 1 + 2 + 2 + 4 + 2 + n);
            write_common_prefix(out, NETPROTO_VERSION, flags);
            netproto::write_u16le(out, handle);
            netproto::write_u32le(out, offset);
            netproto::write_u16le(out, static_cast<std::uint16_t>(n));
            if (n > 0) {
                out.append(info.headersBlock.data() + offset, n);
            }

            resp.payload = to_vec(out);
            return resp;
        }

        case NetworkCommand::Read: {
            auto resp = make_success_response(request);
            Reader r(request.payload.data(), request.payload.size());
            if (!check_version(r, NETPROTO_VERSION)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::uint16_t handle = 0;
            std::uint32_t offset = 0;
            std::uint16_t maxBytes = 0;
            if (!r.read_u16le(handle) || !r.read_u32le(offset) || !r.read_u16le(maxBytes) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            auto* s = session_for_handle(handle);
            if (!s || !s->proto) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            touch(*s);

            // If request body hasn't been fully uploaded, response is not available yet.
            if (s->awaitingBody) {
                resp.status = StatusCode::NotReady;
                return resp;
            }

            std::uint16_t n = 0;
            bool eof = false;
            bool moreAvailable = false;
            std::string out;
            std::uint8_t flags = 0;

            if (translation_enabled(*s)) {
                const StatusCode translationSt = ensure_translation_ready(*s);
                if (translationSt != StatusCode::Ok) {
                    resp.status = translationSt;
                    return resp;
                }

                std::vector<std::uint8_t> translatedBuf(maxBytes);
                const StatusCode st = s->translator->read(offset,
                                                          translatedBuf.data(),
                                                          translatedBuf.size(),
                                                          n,
                                                          eof);
                if (st != StatusCode::Ok) {
                    resp.status = st;
                    return resp;
                }

                out.reserve(1 + 1 + 2 + 2 + 4 + 2 + n);
                if (eof) {
                    flags |= NET_READ_FLAG_EOF;
                    s->completed = true;
                }
                if (n == maxBytes && !eof) {
                    flags |= NET_READ_FLAG_TRUNCATED;
                }
                if (!eof && (offset + n) < s->translatedResultSize) {
                    flags |= NET_READ_FLAG_MORE_AVAILABLE;
                }
                write_common_prefix(out, NETPROTO_VERSION, flags);
                netproto::write_u16le(out, handle);
                netproto::write_u32le(out, offset);
                netproto::write_u16le(out, n);
                if (n > 0) {
                    netproto::write_bytes(out, translatedBuf.data(), n);
                }
                resp.payload = to_vec(out);
                return resp;
            }

            // Normal (non-translated) read path
            {
                std::vector<std::uint8_t> buf;
                buf.resize(maxBytes);

                const StatusCode st = s->proto->read_body(offset, buf.data(), buf.size(), n, eof, moreAvailable);
                if (st != StatusCode::Ok) {
                    resp.status = st;
                    return resp;
                }

                if (n > buf.size()) {
                    n = static_cast<std::uint16_t>(buf.size());
                }

                out.reserve(1 + 1 + 2 + 2 + 4 + 2 + n);

                if (eof) {
                    flags |= NET_READ_FLAG_EOF;
                    s->completed = true;
                }
                if (n == maxBytes && !eof) {
                    flags |= NET_READ_FLAG_TRUNCATED;
                }
                if (moreAvailable) {
                    flags |= NET_READ_FLAG_MORE_AVAILABLE;
                }

                write_common_prefix(out, NETPROTO_VERSION, flags);
                netproto::write_u16le(out, handle);
                netproto::write_u32le(out, offset);
                netproto::write_u16le(out, n);
                if (n > 0 && !buf.empty()) {
                    netproto::write_bytes(out, buf.data(), n);
                }

                resp.payload = to_vec(out);
                return resp;
            }
        }

        case NetworkCommand::Write: {
            auto resp = make_success_response(request);
            Reader r(request.payload.data(), request.payload.size());
            if (!check_version(r, NETPROTO_VERSION)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::uint16_t handle = 0;
            std::uint32_t offset = 0;
            std::uint16_t dataLen = 0;
            if (!r.read_u16le(handle) || !r.read_u32le(offset) || !r.read_u16le(dataLen)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            const std::uint8_t* dataPtr = nullptr;
            if (!r.read_bytes(dataPtr, dataLen) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            auto* s = session_for_handle(handle);
            if (!s || !s->proto) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            touch(*s);

            // Enforce sequential streaming semantics for HTTP request bodies.
            // (Keeps device simple; avoids random-access uploads.)
            if (s->awaitingBody) {
                // Must match the next required offset.
                if (offset != s->nextBodyOffset) {
                    resp.status = StatusCode::InvalidRequest;
                    return resp;
                }

                if (!s->bodyLenUnknown) {
                    // Known-length body: must not exceed expected length.
                    const std::uint64_t end = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(dataLen);
                    if (end > static_cast<std::uint64_t>(s->expectedBodyLen)) {
                        resp.status = StatusCode::InvalidRequest;
                        return resp;
                    }
                } else {
                    // Unknown-length body: allow any length up to a sanity cap.
                    static constexpr std::uint32_t MAX_UNKNOWN_BODY_BYTES = 4u * 1024u * 1024u; // 4 MiB
                    const std::uint64_t end = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(dataLen);
                    if (end > MAX_UNKNOWN_BODY_BYTES) {
                        resp.status = StatusCode::InvalidRequest;
                        return resp;
                    }
                }
            }

            std::uint16_t written = 0;
            const StatusCode st = s->proto->write_body(offset,
                                                       dataPtr, dataLen,
                                                       written);
            if (st != StatusCode::Ok) {
                resp.status = st;
                return resp;
            }

            // Backend must either accept the whole chunk (Ok) or apply backpressure (DeviceBusy).
            // Partial Ok writes create ambiguous host semantics, so treat as internal contract violation.
            if (st == StatusCode::Ok && written != dataLen) {
                resp.status = StatusCode::InternalError;
                return resp;
            }

            if (s->awaitingBody) {
                // Unknown-length body: commit is signaled by a zero-length Write.
                if (s->bodyLenUnknown && dataLen == 0) {
                    s->awaitingBody = false;
                } else {
                    s->receivedBodyLen += written;
                    s->nextBodyOffset  += written;

                    if (!s->bodyLenUnknown && s->receivedBodyLen == s->expectedBodyLen) {
                        // Body complete; request is now considered dispatched.
                        s->awaitingBody = false;
                    }
                }
            }

            std::string out;
            out.reserve(1 + 1 + 2 + 2 + 4 + 2);
            write_common_prefix(out, NETPROTO_VERSION, 0);
            netproto::write_u16le(out, handle);
            netproto::write_u32le(out, offset);
            netproto::write_u16le(out, written);
            resp.payload = to_vec(out);
            return resp;
        }

        case NetworkCommand::TranslateConfigure: {
            auto resp = make_success_response(request);
            Reader r(request.payload.data(), request.payload.size());
            if (!check_version(r, NETPROTO_VERSION)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::uint16_t handle = 0;
            if (!r.read_u16le(handle)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            auto* s = session_for_handle(handle);
            if (!s || !s->proto) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            touch(*s);

            if (s->awaitingBody) {
                resp.status = StatusCode::NotReady;
                return resp;
            }

            TranslationConfig translation;
            if (!read_translation_config(r, translation) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            FN_LOGD("net", "TranslateConfigure handle=%u type=%u selector=\"%s\" bodyCached=%d bodySize=%zu",
                    handle,
                    static_cast<unsigned>(translation.type),
                    translation.selector.c_str(),
                    static_cast<int>(s->responseBodyCached),
                    s->responseBodyCache.size());

            const StatusCode configureSt = configure_translation(*s, translation);
            if (configureSt != StatusCode::Ok) {
                resp.status = configureSt;
                return resp;
            }

            if (translation_enabled(*s)) {
                const StatusCode translationSt = ensure_translation_ready(*s);
                if (translationSt != StatusCode::Ok) {
                    resp.status = translationSt;
                    return resp;
                }
            }

            std::string out;
            std::uint8_t jflags = s->translationReady ? 0x01 : 0x00;
            write_common_prefix(out, NETPROTO_VERSION, jflags);
            netproto::write_u16le(out, handle);
            netproto::write_u32le(out, static_cast<std::uint32_t>(s->translatedResultSize));
            resp.payload = to_vec(out);
            return resp;
        }

        default:
            return make_base_response(request, StatusCode::Unsupported);
    }
}

} // namespace fujinet::io
