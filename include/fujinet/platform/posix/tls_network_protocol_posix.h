#pragma once

#include "fujinet/io/devices/network_protocol.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Forward declarations for OpenSSL types
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace fujinet::platform::posix {

// TLS stream backend using OpenSSL.
// Provides secure TCP connections (TLS/SSL) for protocols like IRC over TLS, SMTPS, etc.
// URL format: tls://host:port
class TlsNetworkProtocolPosix final : public fujinet::io::INetworkProtocol {
public:
    TlsNetworkProtocolPosix();
    ~TlsNetworkProtocolPosix() override;

    fujinet::io::StatusCode open(const fujinet::io::NetworkOpenRequest& req) override;
    fujinet::io::StatusCode write_body(std::uint32_t offset,
                                       const std::uint8_t* data,
                                       std::size_t len,
                                       std::uint16_t& written) override;
    fujinet::io::StatusCode read_body(std::uint32_t offset,
                                      std::uint8_t* out,
                                      std::size_t outLen,
                                      std::uint16_t& read,
                                      bool& eof) override;
    fujinet::io::StatusCode info(fujinet::io::NetworkInfo& out) override;
    void poll() override;
    void close() override;

    // Protocol capabilities - TLS is a streaming protocol
    bool is_streaming() const override { return true; }
    bool requires_sequential_read() const override { return true; }
    bool requires_sequential_write() const override { return true; }

private:
    // Parse tls://host:port URL
    static bool parse_tls_url(const std::string& url,
                              std::string& outHost,
                              std::uint16_t& outPort);

    // Reset internal state
    void reset_state();

    // Initialize OpenSSL (called once globally)
    static void ensure_ssl_init();

    // Handle I/O errors
    void handle_error(const char* context, int sslError);

    SSL* _ssl{nullptr};
    SSL_CTX* _ctx{nullptr};
    int _socket{-1};
    std::string _host;
    std::uint16_t _port{0};

    // Connection state
    enum class State {
        Idle,
        Connecting,
        Connected,
        PeerClosed,
        Error
    };
    State _state{State::Idle};

    // RX buffer for incoming data
    std::vector<std::uint8_t> _rxBuffer;
    std::size_t _rxAvailable{0};

    // Stream cursors
    std::uint32_t _readCursor{0};
    std::uint32_t _writeCursor{0};

    // Error tracking
    int _lastError{0};
    bool _peerClosed{false};
};

} // namespace fujinet::platform::posix
