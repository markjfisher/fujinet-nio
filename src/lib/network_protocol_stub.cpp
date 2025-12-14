#include "fujinet/io/devices/network_protocol_stub.h"

#include <algorithm>
#include <string>

namespace fujinet::io {

static bool is_body_method(std::uint8_t method)
{
    // 2=POST, 3=PUT
    return method == 2 || method == 3;
}

StatusCode StubNetworkProtocol::open(const NetworkOpenRequest& req)
{
    _openReq = req;

    _httpStatus = 200;
    _headersBlock = "Content-Type: text/plain\r\nServer: fujinet-nio-stub\r\n";

    const std::string bodyStr = std::string("stub response for: ") + _openReq.url + "\n";
    _body.assign(bodyStr.begin(), bodyStr.end());
    _contentLength = static_cast<std::uint64_t>(_body.size());

    _requestBody.clear();
    if (is_body_method(_openReq.method) && _openReq.bodyLenHint > 0) {
        // Pre-size to hint (but keep it bounded by what host actually sends).
        _requestBody.reserve(_openReq.bodyLenHint);
    }

    return StatusCode::Ok;
}

StatusCode StubNetworkProtocol::write_body(std::uint32_t offset,
                                           const std::uint8_t* data,
                                           std::size_t dataLen,
                                           std::uint16_t& written)
{
    written = 0;

    if (!is_body_method(_openReq.method)) {
        return StatusCode::Unsupported;
    }

    const std::size_t off = static_cast<std::size_t>(offset);
    if (off > _requestBody.size()) {
        // Disallow sparse writes for now.
        return StatusCode::InvalidRequest;
    }

    // Expand to accommodate write
    if (off + dataLen > _requestBody.size()) {
        _requestBody.resize(off + dataLen);
    }

    if (dataLen > 0) {
        std::copy_n(data, static_cast<std::ptrdiff_t>(dataLen),
                    _requestBody.begin() + static_cast<std::ptrdiff_t>(off));
    }
    written = static_cast<std::uint16_t>(dataLen);
    return StatusCode::Ok;
}

StatusCode StubNetworkProtocol::read_body(std::uint32_t offset,
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

    if (n > 0) {
        std::copy_n(_body.begin() + static_cast<std::ptrdiff_t>(off),
                    static_cast<std::ptrdiff_t>(n),
                    out);
        read = static_cast<std::uint16_t>(n);
    }

    eof = (off + n) >= total;
    return StatusCode::Ok;
}

StatusCode StubNetworkProtocol::info(std::size_t maxHeaderBytes, NetworkInfo& out)
{
    out = NetworkInfo{};
    out.hasHttpStatus = true;
    out.httpStatus = _httpStatus;
    out.hasContentLength = true;
    out.contentLength = _contentLength;

    const std::size_t n = std::min<std::size_t>(_headersBlock.size(), maxHeaderBytes);
    out.headersBlock.assign(_headersBlock.data(), n);
    return StatusCode::Ok;
}

void StubNetworkProtocol::close()
{
    _body.clear();
    _headersBlock.clear();
    _requestBody.clear();
    _contentLength = 0;
    _httpStatus = 0;
    _openReq = NetworkOpenRequest{};
}

} // namespace fujinet::io


