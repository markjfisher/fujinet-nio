#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fujinet/io/core/io_message.h"

namespace fujinet::io {

struct NetworkOpenRequest {
    std::uint8_t method{0};          // 1=GET,2=POST,3=PUT,4=DELETE,5=HEAD
    std::uint8_t flags{0};           // bit0=tls, bit1=follow_redirects, bit2=want_headers
    std::string  url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::uint32_t bodyLenHint{0};
};

struct NetworkInfo {
    bool hasHttpStatus{false};
    std::uint16_t httpStatus{0};

    bool hasContentLength{false};
    std::uint64_t contentLength{0};

    // Raw "Key: Value\r\n" bytes as a string (binary-safe for ASCII/UTF-8).
    std::string headersBlock;
};

class INetworkProtocol {
public:
    virtual ~INetworkProtocol() = default;

    virtual StatusCode open(const NetworkOpenRequest& req) = 0;

    // Write request body bytes at an explicit offset.
    virtual StatusCode write_body(std::uint32_t offset,
                                  const std::uint8_t* data,
                                  std::size_t dataLen,
                                  std::uint16_t& written) = 0;

    // Read response body bytes at an explicit offset into `out`.
    virtual StatusCode read_body(std::uint32_t offset,
                                 std::uint8_t* out,
                                 std::size_t outLen,
                                 std::uint16_t& read,
                                 bool& eof) = 0;

    // Fetch response metadata (may become available over time).
    virtual StatusCode info(std::size_t maxHeaderBytes, NetworkInfo& out) = 0;

    virtual void poll() = 0;
    virtual void close() = 0;
};

} // namespace fujinet::io


