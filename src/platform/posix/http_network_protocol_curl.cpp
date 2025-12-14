#include "fujinet/platform/posix/http_network_protocol_curl.h"

#if FN_WITH_CURL == 1

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// curl headers are only included in curl-specific files
#include <curl/curl.h>

namespace fujinet::platform::posix {

static bool method_supported(std::uint8_t method)
{
    // v1: GET(1), HEAD(5)
    return method == 1 || method == 5;
}

std::size_t HttpNetworkProtocolCurl::write_body_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata)
{
    auto* self = static_cast<HttpNetworkProtocolCurl*>(userdata);
    const std::size_t n = size * nmemb;
    if (!self || !ptr || n == 0) return 0;
    self->_body.insert(self->_body.end(),
                       reinterpret_cast<std::uint8_t*>(ptr),
                       reinterpret_cast<std::uint8_t*>(ptr) + n);
    return n;
}

std::size_t HttpNetworkProtocolCurl::write_header_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata)
{
    auto* self = static_cast<HttpNetworkProtocolCurl*>(userdata);
    const std::size_t n = size * nmemb;
    if (!self || !ptr || n == 0) return 0;

    // libcurl passes header lines including trailing \r\n.
    // We only keep "Key: Value\r\n" lines (skip status lines and blanks).
    const std::string_view line(ptr, n);
    if (line.find(':') != std::string_view::npos) {
        self->_headersBlock.append(line.data(), line.size());
    }
    return n;
}

io::StatusCode HttpNetworkProtocolCurl::open(const io::NetworkOpenRequest& req)
{
    close();

    if (!method_supported(req.method)) {
        return io::StatusCode::Unsupported;
    }

    _req = req;

    CURL* curl = curl_easy_init();
    if (!curl) {
        return io::StatusCode::InternalError;
    }

    // Basic URL + callbacks
    curl_easy_setopt(curl, CURLOPT_URL, _req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &HttpNetworkProtocolCurl::write_body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);

    const bool wantHeaders = (_req.flags & 0x04) != 0;
    if (wantHeaders) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &HttpNetworkProtocolCurl::write_header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
    }

    const bool follow = (_req.flags & 0x02) != 0;
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);

    // HEAD: no body
    if (_req.method == 5) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        close();
        return io::StatusCode::IOError;
    }

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    _httpStatus = static_cast<std::uint16_t>(httpCode < 0 ? 0 : httpCode);

    curl_easy_cleanup(curl);

    // For v1, we publish contentLength as actual body size.
    return io::StatusCode::Ok;
}

io::StatusCode HttpNetworkProtocolCurl::write_body(std::uint32_t,
                                                   const std::uint8_t*,
                                                   std::size_t,
                                                   std::uint16_t& written)
{
    written = 0;
    return io::StatusCode::Unsupported;
}

io::StatusCode HttpNetworkProtocolCurl::read_body(std::uint32_t offset,
                                                  std::uint8_t* out,
                                                  std::size_t outLen,
                                                  std::uint16_t& read,
                                                  bool& eof)
{
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

io::StatusCode HttpNetworkProtocolCurl::info(std::size_t maxHeaderBytes, io::NetworkInfo& out)
{
    out = io::NetworkInfo{};
    out.hasHttpStatus = true;
    out.httpStatus = _httpStatus;
    out.hasContentLength = true;
    out.contentLength = static_cast<std::uint64_t>(_body.size());

    const std::size_t n = std::min<std::size_t>(_headersBlock.size(), maxHeaderBytes);
    out.headersBlock.assign(_headersBlock.data(), n);
    return io::StatusCode::Ok;
}

void HttpNetworkProtocolCurl::close()
{
    _req = io::NetworkOpenRequest{};
    _httpStatus = 0;
    _headersBlock.clear();
    _body.clear();
}

} // namespace fujinet::platform::posix

#endif // FN_WITH_CURL


