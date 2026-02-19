#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fujinet/io/core/io_message.h"

namespace fujinet::io {

struct NetworkOpenRequest {
    std::uint8_t method{0}; // 1=GET,2=POST,3=PUT,4=DELETE,5=HEAD

    // bit0=tls, bit1=follow_redirects
    std::uint8_t flags{0};

    std::string url;

    // Request headers (sent to server)
    std::vector<std::pair<std::string, std::string>> headers;

    // Request body hint (POST/PUT)
    std::uint32_t bodyLenHint{0};

    // Response header allowlist (lowercase ASCII header names).
    // If empty: store no response headers.
    std::vector<std::string> responseHeaderNamesLower;
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
    virtual StatusCode info(NetworkInfo& out) = 0;

    virtual void poll() = 0;
    virtual void close() = 0;

    // ========================================================================
    // Protocol Capabilities
    // ========================================================================
    // These methods describe protocol behavior so the server can inform
    // clients about how to interact with the session.

    /**
     * @brief Returns true if this is a streaming protocol (no content-length).
     * 
     * Streaming protocols (TCP, TLS) don't have a predetermined content length.
     * The client reads until EOF.
     */
    virtual bool is_streaming() const { return false; }

    /**
     * @brief Returns true if read operations must be sequential.
     * 
     * For streaming protocols like TCP, reads must be sequential (offset is
     * ignored and data is consumed in order). HTTP allows random access reads.
     */
    virtual bool requires_sequential_read() const { return false; }

    /**
     * @brief Returns true if write operations must be sequential.
     * 
     * For streaming protocols like TCP, writes must be sequential. HTTP
     * typically allows random access for body uploads.
     */
    virtual bool requires_sequential_write() const { return false; }
};

} // namespace fujinet::io
