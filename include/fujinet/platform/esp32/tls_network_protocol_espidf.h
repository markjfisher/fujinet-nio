#pragma once

#include "fujinet/io/devices/network_protocol.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Forward declaration for ESP-IDF type
struct esp_tls;

namespace fujinet::platform::esp32 {

// TLS stream backend using ESP-IDF esp_tls component.
// Provides secure TCP connections (TLS/SSL) for protocols like IRC over TLS, SMTPS, etc.
// URL format: tls://host:port
class TlsNetworkProtocolEspIdf final : public fujinet::io::INetworkProtocol {
public:
    TlsNetworkProtocolEspIdf();
    ~TlsNetworkProtocolEspIdf() override;

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

    // Handle I/O errors
    void handle_error(int err);

    esp_tls* _tls{nullptr};
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

} // namespace fujinet::platform::esp32
