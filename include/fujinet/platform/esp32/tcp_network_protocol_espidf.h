#pragma once

#include "fujinet/io/devices/network_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fujinet::platform::esp32 {

// TCP stream backend using ESP-IDF / lwIP BSD sockets.
// Same contract as POSIX implementation.
class TcpNetworkProtocolEspIdf final : public fujinet::io::INetworkProtocol {
public:
    TcpNetworkProtocolEspIdf();
    ~TcpNetworkProtocolEspIdf() override;

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
    fujinet::io::StatusCode info(std::size_t maxHeaderBytes,
                                 fujinet::io::NetworkInfo& out) override;
    void poll() override;
    void close() override;

private:
    struct Options {
        int connect_timeout_ms = 5000;
        bool nodelay = true;
        bool keepalive = false;
        std::size_t rx_buf = 8192;
        bool halfclose = true;
    };

    enum class State {
        Idle,
        Connecting,
        Connected,
        PeerClosed,
        Error
    };

    static bool parse_tcp_url(const std::string& url,
                              std::string& outHost,
                              std::uint16_t& outPort,
                              Options& outOpt);

    void reset_state();
    void set_error(int e);
    void apply_socket_options();
    void step_connect();
    void pump_recv();
    std::size_t rx_available() const noexcept;
    std::string build_info_headers() const;

private:
    int _fd = -1;

    State _state = State::Idle;
    bool _peer_closed = false;

    std::string _host;
    std::uint16_t _port = 0;
    Options _opt{};

    std::uint32_t _read_cursor = 0;
    std::uint32_t _write_cursor = 0;

    std::vector<std::uint8_t> _rx;
    std::size_t _rx_head = 0;
    std::size_t _rx_tail = 0;
    bool _rx_full = false;

    std::uint32_t _connect_start_ms = 0;
    int _last_errno = 0;
};

} // namespace fujinet::platform::esp32
