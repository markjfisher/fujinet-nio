#include "fujinet/platform/posix/http_network_protocol_curl.h"

#if FN_WITH_CURL == 1

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

// curl headers are only included in curl-specific files
#include <curl/curl.h>

namespace fujinet::platform::posix {

static void ensure_curl_global_init()
{
    static const bool inited = []{
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)inited;
}

static bool method_supported(std::uint8_t method)
{
    // v1: GET(1), POST(2), PUT(3), DELETE(4), HEAD(5)
    switch (method) {
        case 1: // GET
        case 2: // POST
        case 3: // PUT
        case 4: // DELETE
        case 5: // HEAD
            return true;
        default:
            return false;
    }
}

std::size_t HttpNetworkProtocolCurl::write_body_cb(
    char *ptr,
    std::size_t size,
    std::size_t nmemb,
    void *userdata)
{
    auto *self = static_cast<HttpNetworkProtocolCurl *>(userdata);

    // Abort if we have nowhere to write.
    if (!self)
        return 0;

    // Guard overflow: n = size * nmemb
    if (size != 0 && nmemb > (std::numeric_limits<std::size_t>::max() / size))
    {
        return 0; // abort transfer
    }

    const std::size_t n = size * nmemb;
    if (n == 0)
        return 0;

    // libcurl should not call with ptr==nullptr if n>0, but be defensive.
    if (!ptr)
        return 0;

    self->_body.insert(self->_body.end(),
                       reinterpret_cast<const std::uint8_t *>(ptr),
                       reinterpret_cast<const std::uint8_t *>(ptr) + n);
    return n;
}

std::size_t HttpNetworkProtocolCurl::write_header_cb(
    char *ptr, std::size_t size, std::size_t nmemb, void *userdata)
{
    auto *self = static_cast<HttpNetworkProtocolCurl*>(userdata);
    if (!self) return 0;

    if (size != 0 && nmemb > (std::numeric_limits<std::size_t>::max() / size)) {
        return 0; // abort transfer
    }
    const std::size_t n = size * nmemb;
    if (n == 0) return 0;
    if (!ptr) return 0;

    // NEW: store ONLY requested headers. If none requested, store nothing.
    if (self->_req.responseHeaderNamesLower.empty()) {
        return n;
    }

    const std::string_view line(ptr, n);

    // Keep only "Key: Value\r\n" lines (skip status lines and blanks).
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return n;
    }

    std::string_view key = line.substr(0, colon);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
        key.remove_suffix(1);
    }

    // Case-insensitive match against allowlist (stored lowercase).
    std::string keyLower;
    keyLower.reserve(key.size());
    for (char c : key) {
        keyLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    for (const auto& want : self->_req.responseHeaderNamesLower) {
        if (want == keyLower) {
            self->_headersBlock.append(line.data(), line.size());
            break;
        }
    }

    return n;
}


io::StatusCode HttpNetworkProtocolCurl::perform_now()
{
    if (!_curl) return io::StatusCode::InternalError;

    // Clear response buffers for this execution
    _httpStatus = 0;
    _headersBlock.clear();
    _body.clear();

    CURLcode res = curl_easy_perform(_curl);

    long httpCode = 0;
    curl_easy_getinfo(_curl, CURLINFO_RESPONSE_CODE, &httpCode);
    _httpStatus = static_cast<std::uint16_t>(httpCode < 0 ? 0 : httpCode);

    if (res != CURLE_OK) {
        return io::StatusCode::IOError;
    }

    _performed = true;
    return io::StatusCode::Ok;
}

io::StatusCode HttpNetworkProtocolCurl::open(const io::NetworkOpenRequest& req)
{
    close();

    if (!method_supported(req.method)) {
        return io::StatusCode::Unsupported;
    }

    ensure_curl_global_init();
    _req = req;

    const bool isPost = (_req.method == 2);
    const bool isPut  = (_req.method == 3);
    
    // Detect whether caller explicitly provided Content-Type (case-insensitive).
    bool hasContentType = false;
    for (const auto& kv : _req.headers) {
        std::string k = kv.first;
        for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (k == "content-type") {
            hasContentType = true;
            break;
        }
    }

    _curl = curl_easy_init();
    if (!_curl) {
        return io::StatusCode::InternalError;
    }

    // Build request headers
    for (const auto& kv : _req.headers) {
        std::string line;
        line.reserve(kv.first.size() + 2 + kv.second.size());
        line.append(kv.first);
        line.append(": ");
        line.append(kv.second);
        _slist = curl_slist_append(_slist, line.c_str());
    }

    // IMPORTANT:
    // libcurl will default Content-Type to application/x-www-form-urlencoded when POSTFIELDS is used.
    // We want "no Content-Type unless explicitly requested by the client", so suppress it.
    if ((isPost || isPut) && !hasContentType) {
        _slist = curl_slist_append(_slist, "Content-Type:");
    }

    if (_slist) {
        curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, _slist);
    }

    // Common options
    curl_easy_setopt(_curl, CURLOPT_URL, _req.url.c_str());
    curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, &HttpNetworkProtocolCurl::write_body_cb);
    curl_easy_setopt(_curl, CURLOPT_WRITEDATA, this);

    // Always collect headers on POSIX so Info() can return them later if requested.
    curl_easy_setopt(_curl, CURLOPT_HEADERFUNCTION, &HttpNetworkProtocolCurl::write_header_cb);
    curl_easy_setopt(_curl, CURLOPT_HEADERDATA, this);

    const bool follow = (_req.flags & 0x02) != 0;
    curl_easy_setopt(_curl, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);

    // TLS verification defaults (fine for now)
    curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // Reset request-body state
    _requestBody.clear();
    _expectedRequestBodyLen = _req.bodyLenHint;
    _performed = false;

    const bool hasBody = (_req.bodyLenHint > 0);
    const bool bodyUnknown = ((_req.flags & 0x04) != 0) && (_req.bodyLenHint == 0) && (isPost || isPut);

    // Configure method
    if (_req.method == 5) {
        // HEAD
        curl_easy_setopt(_curl, CURLOPT_NOBODY, 1L);
    } else if (_req.method == 1) {
        // GET
        curl_easy_setopt(_curl, CURLOPT_HTTPGET, 1L);
    } else if (_req.method == 4) {
        // DELETE (no body for now)
        curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (isPost) {
        // POST
        curl_easy_setopt(_curl, CURLOPT_POST, 1L);
    } else if (isPut) {
        // PUT: easiest is CUSTOMREQUEST + POSTFIELDS (sends request body)
        curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, "PUT");
    }

    // If method expects/uses a body:
    // - If bodyLenHint==0, dispatch immediately with an empty body.
    // - If bodyLenHint>0, we defer until write_body() completes.
    if (isPost || isPut) {
        if (!hasBody && !bodyUnknown) {
            curl_easy_setopt(_curl, CURLOPT_POSTFIELDS, "");
            curl_easy_setopt(_curl, CURLOPT_POSTFIELDSIZE, 0L);
            io::StatusCode st = perform_now();
            if (st != io::StatusCode::Ok) { close(); }
            return st;
        }
        // defer; write_body() will provide POSTFIELDS + size and then perform (known length)
        // or will commit on a zero-length write (unknown length).
        return io::StatusCode::Ok;
    }

    // No-body methods dispatch immediately
    io::StatusCode st = perform_now();
    if (st != io::StatusCode::Ok) { close(); }
    return st;
}


io::StatusCode HttpNetworkProtocolCurl::write_body(
    std::uint32_t offset,
    const std::uint8_t* data,
    std::size_t len,
    std::uint16_t& written
) {
    written = 0;

    const bool isPostOrPut = (_req.method == 2 || _req.method == 3);
    const bool bodyUnknown = ((_req.flags & 0x04) != 0) && (_expectedRequestBodyLen == 0) && isPostOrPut;

    // Only meaningful for POST/PUT requests that were opened for body upload:
    // - known-length: bodyLenHint > 0
    // - unknown-length: bodyLenHint == 0 and flag bit2 set
    if (!isPostOrPut || (!bodyUnknown && _expectedRequestBodyLen == 0)) {
        return io::StatusCode::Unsupported;
    }

    // NetworkDevice already enforces sequential offsets + overflow,
    // but keep a defensive check here too.
    if (offset != _requestBody.size()) {
        return io::StatusCode::InvalidRequest;
    }
    if (!bodyUnknown) {
        const std::uint64_t end = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(len);
        if (end > static_cast<std::uint64_t>(_expectedRequestBodyLen)) {
            return io::StatusCode::InvalidRequest;
        }
    }

    if (len > 0) {
        if (!data) return io::StatusCode::InvalidRequest;
        _requestBody.insert(_requestBody.end(), data, data + len);
    }

    written = static_cast<std::uint16_t>(len);

    // Dispatch rules:
    // - known-length: dispatch once we've received exactly bodyLenHint bytes
    // - unknown-length: dispatch on zero-length Write() (commit)
    const bool shouldDispatch =
        (!bodyUnknown && _requestBody.size() == _expectedRequestBodyLen) ||
        (bodyUnknown && len == 0);

    if (shouldDispatch) {
        if (!_curl) return io::StatusCode::InternalError;

        // Provide the final body to libcurl (valid until perform returns)
        curl_easy_setopt(_curl, CURLOPT_POSTFIELDS, _requestBody.data());
        curl_easy_setopt(_curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(_requestBody.size()));

        io::StatusCode st = perform_now();
        if (st != io::StatusCode::Ok) {
            close();
        }
        return st;
    }

    return io::StatusCode::Ok;
}

io::StatusCode HttpNetworkProtocolCurl::read_body(std::uint32_t offset,
                                                  std::uint8_t* out,
                                                  std::size_t outLen,
                                                  std::uint16_t& read,
                                                  bool& eof)
{
    if (!_performed) {
        read = 0;
        eof = false;
        return io::StatusCode::NotReady;
    }

    read = 0;
    eof = false;

    const std::size_t total = _body.size();
    const std::size_t off = (offset <= total) ? static_cast<std::size_t>(offset) : total;
    const std::size_t n = std::min<std::size_t>(outLen, total - off);

    if (n > 0 && out) {
        std::memcpy(out, _body.data() + off, n);
        read = static_cast<std::uint16_t>(n);
    }

    eof = (off + n) >= total;
    return io::StatusCode::Ok;
}

io::StatusCode HttpNetworkProtocolCurl::info(io::NetworkInfo& out)
{
    if (!_performed) {
        return io::StatusCode::NotReady;
    }

    out = io::NetworkInfo{};
    out.hasHttpStatus = true;
    out.httpStatus = _httpStatus;

    out.hasContentLength = true;
    out.contentLength = static_cast<std::uint64_t>(_body.size());

    // headers already filtered.
    out.headersBlock = _headersBlock;

    return io::StatusCode::Ok;
}


void HttpNetworkProtocolCurl::close()
{
    _req = io::NetworkOpenRequest{};
    _httpStatus = 0;
    _headersBlock.clear();
    _body.clear();

    _requestBody.clear();
    _expectedRequestBodyLen = 0;
    _performed = false;

    if (_curl) {
        curl_easy_cleanup(_curl);
        _curl = nullptr;
    }
    if (_slist) {
        curl_slist_free_all(_slist);
        _slist = nullptr;
    }
}

} // namespace fujinet::platform::posix

#endif // FN_WITH_CURL


