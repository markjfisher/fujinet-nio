#include "fujinet/platform/esp32/tcp_network_protocol_espidf.h"

#include "fujinet/core/logging.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

extern "C" {
#include "lwip/errno.h"
#include "lwip/fcntl.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_timer.h"
#include "netinet/tcp.h"
}

namespace fujinet::platform::esp32 {

static constexpr const char* TAG = "platform";

static std::uint32_t now_ms()
{
    // esp_timer_get_time() returns microseconds
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
}

static bool starts_with(std::string_view s, std::string_view p)
{
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

static std::string_view trim_prefix(std::string_view s, std::string_view p)
{
    return starts_with(s, p) ? s.substr(p.size()) : s;
}

static bool parse_bool(std::string_view v, bool def)
{
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

static int parse_int(std::string_view v, int def)
{
    if (v.empty()) return def;
    int sign = 1;
    std::size_t i = 0;
    if (v[0] == '-') { sign = -1; i = 1; }
    int out = 0;
    for (; i < v.size(); ++i) {
        if (v[i] < '0' || v[i] > '9') return def;
        const int d = (v[i] - '0');
        if (out > (std::numeric_limits<int>::max() - d) / 10) return def;
        out = out * 10 + d;
    }
    return out * sign;
}

TcpNetworkProtocolEspIdf::TcpNetworkProtocolEspIdf() = default;

TcpNetworkProtocolEspIdf::~TcpNetworkProtocolEspIdf()
{
    close();
}

void TcpNetworkProtocolEspIdf::reset_state()
{
    _state = State::Idle;
    _peer_closed = false;
    _read_cursor = 0;
    _write_cursor = 0;
    _connect_start_ms = 0;
    _last_errno = 0;

    _rx_head = 0;
    _rx_tail = 0;
    _rx_full = false;
    _rx.clear();
}

void TcpNetworkProtocolEspIdf::set_error(int e)
{
    _last_errno = e;
    _state = State::Error;
}

bool TcpNetworkProtocolEspIdf::parse_tcp_url(const std::string& url,
                                             std::string& outHost,
                                             std::uint16_t& outPort,
                                             Options& outOpt)
{
    // Same parser as POSIX: tcp://host:port?...
    std::string_view s(url);
    if (!starts_with(s, "tcp://")) return false;
    s = trim_prefix(s, "tcp://");

    std::string_view authority = s;
    std::string_view query;
    if (auto qpos = s.find('?'); qpos != std::string_view::npos) {
        authority = s.substr(0, qpos);
        query = s.substr(qpos + 1);
    }

    std::string host;
    std::string_view portPart;

    if (authority.empty()) return false;

    if (authority[0] == '[') {
        auto rb = authority.find(']');
        if (rb == std::string_view::npos) return false;
        host.assign(authority.substr(1, rb - 1));
        if (rb + 1 >= authority.size() || authority[rb + 1] != ':') return false;
        portPart = authority.substr(rb + 2);
    } else {
        auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) return false;
        host.assign(authority.substr(0, colon));
        portPart = authority.substr(colon + 1);
    }

    if (host.empty() || portPart.empty()) return false;

    const int p = parse_int(portPart, -1);
    if (p <= 0 || p > 65535) return false;

    Options opt{};

    while (!query.empty()) {
        auto amp = query.find('&');
        std::string_view kv = (amp == std::string_view::npos) ? query : query.substr(0, amp);
        query = (amp == std::string_view::npos) ? std::string_view{} : query.substr(amp + 1);

        if (kv.empty()) continue;
        auto eq = kv.find('=');
        std::string_view k = (eq == std::string_view::npos) ? kv : kv.substr(0, eq);
        std::string_view v = (eq == std::string_view::npos) ? std::string_view{} : kv.substr(eq + 1);

        if (k == "connect_timeout_ms") opt.connect_timeout_ms = std::max(0, parse_int(v, opt.connect_timeout_ms));
        else if (k == "nodelay") opt.nodelay = parse_bool(v, opt.nodelay);
        else if (k == "keepalive") opt.keepalive = parse_bool(v, opt.keepalive);
        else if (k == "rx_buf") {
            int n = parse_int(v, static_cast<int>(opt.rx_buf));
            if (n < 256) n = 256;
            if (n > 1'048'576) n = 1'048'576;
            opt.rx_buf = static_cast<std::size_t>(n);
        } else if (k == "halfclose") opt.halfclose = parse_bool(v, opt.halfclose);
    }

    outHost = std::move(host);
    outPort = static_cast<std::uint16_t>(p);
    outOpt = opt;
    return true;
}

void TcpNetworkProtocolEspIdf::apply_socket_options()
{
    if (_fd < 0) return;

    if (_opt.nodelay) {
        int v = 1;
        (void)lwip_setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
    }
    if (_opt.keepalive) {
        int v = 1;
        (void)lwip_setsockopt(_fd, SOL_SOCKET, SO_KEEPALIVE, &v, sizeof(v));
    }

    const int flags = lwip_fcntl(_fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)lwip_fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void TcpNetworkProtocolEspIdf::step_connect()
{
    if (_state != State::Connecting) return;
    if (_fd < 0) return;

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(_fd, &wfds);

    timeval tv {};
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    const int sr = lwip_select(_fd + 1, nullptr, &wfds, nullptr, &tv);
    if (sr < 0) {
        set_error(errno);
        return;
    }
    if (sr == 0) {
        const std::uint32_t now = now_ms();
        if (_opt.connect_timeout_ms > 0 &&
            _connect_start_ms > 0 &&
            (now - _connect_start_ms) > static_cast<std::uint32_t>(_opt.connect_timeout_ms)) {
            set_error(ETIMEDOUT);
        }
        return;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    if (lwip_getsockopt(_fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        set_error(errno);
        return;
    }
    if (err != 0) {
        set_error(err);
        return;
    }

    _state = State::Connected;
}

std::size_t TcpNetworkProtocolEspIdf::rx_available() const noexcept
{
    if (_rx.empty()) return 0;
    if (_rx_full) return _rx.size();
    if (_rx_tail >= _rx_head) return _rx_tail - _rx_head;
    return (_rx.size() - _rx_head) + _rx_tail;
}

void TcpNetworkProtocolEspIdf::pump_recv()
{
    if (_fd < 0) return;
    if (!(_state == State::Connected || _state == State::PeerClosed)) return;
    if (_rx.empty()) return;

    while (rx_available() < _rx.size()) {
        std::size_t free_total = _rx.size() - rx_available();
        if (free_total == 0) break;

        std::size_t free_contig = 0;
        std::size_t write_pos = _rx_tail;

        if (_rx_full) break;

        if (_rx_tail >= _rx_head) {
            free_contig = _rx.size() - _rx_tail;
        } else {
            free_contig = _rx_head - _rx_tail;
        }

        free_contig = std::min(free_contig, free_total);
        if (free_contig == 0) break;

        const int n = lwip_recv(_fd, &_rx[write_pos], free_contig, MSG_DONTWAIT);
        if (n > 0) {
            _rx_tail = (_rx_tail + static_cast<std::size_t>(n)) % _rx.size();
            if (_rx_tail == _rx_head) _rx_full = true;
            continue;
        }
        if (n == 0) {
            _state = State::PeerClosed;
            _peer_closed = true;
            return;
        }
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return;
        }
        set_error(errno);
        return;
    }
}

std::string TcpNetworkProtocolEspIdf::build_info_headers() const
{
    std::string b;
    b.reserve(256);

    auto append_kv = [&](const char* k, const std::string& v) {
        b.append(k);
        b.append(": ");
        b.append(v);
        b.append("\r\n");
    };
    auto append_kv_u64 = [&](const char* k, std::uint64_t v) {
        b.append(k);
        b.append(": ");
        b.append(std::to_string(v));
        b.append("\r\n");
    };

    append_kv("X-FujiNet-Scheme", "tcp");
    append_kv("X-FujiNet-Remote", _host + ":" + std::to_string(_port));

    const bool connecting = (_state == State::Connecting);
    const bool connected = (_state == State::Connected || _state == State::PeerClosed);
    append_kv("X-FujiNet-Connecting", connecting ? "1" : "0");
    append_kv("X-FujiNet-Connected", connected ? "1" : "0");
    append_kv("X-FujiNet-PeerClosed", _peer_closed ? "1" : "0");

    append_kv_u64("X-FujiNet-RxAvailable", static_cast<std::uint64_t>(rx_available()));
    append_kv_u64("X-FujiNet-ReadCursor", static_cast<std::uint64_t>(_read_cursor));
    append_kv_u64("X-FujiNet-WriteCursor", static_cast<std::uint64_t>(_write_cursor));
    append_kv_u64("X-FujiNet-LastError", static_cast<std::uint64_t>(_last_errno));

    return b;
}

fujinet::io::StatusCode TcpNetworkProtocolEspIdf::open(const fujinet::io::NetworkOpenRequest& req)
{
    close();
    reset_state();

    if (!parse_tcp_url(req.url, _host, _port, _opt)) {
        return fujinet::io::StatusCode::InvalidRequest;
    }

    _rx.assign(_opt.rx_buf, 0);

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(_port);
    const int gai = lwip_getaddrinfo(_host.c_str(), portStr.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        set_error(EHOSTUNREACH);
        return fujinet::io::StatusCode::IOError;
    }

    int lastErr = 0;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        _fd = lwip_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (_fd < 0) {
            lastErr = errno;
            continue;
        }

        apply_socket_options();
        _connect_start_ms = now_ms();

        const int cr = lwip_connect(_fd, ai->ai_addr, ai->ai_addrlen);
        if (cr == 0) {
            _state = State::Connected;
            lastErr = 0;
            break;
        }
        if (cr < 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK)) {
            _state = State::Connecting;
            lastErr = 0;
            break;
        }

        lastErr = errno;
        lwip_close(_fd);
        _fd = -1;
    }

    lwip_freeaddrinfo(res);

    if (_fd < 0) {
        set_error(lastErr != 0 ? lastErr : ECONNREFUSED);
        return fujinet::io::StatusCode::IOError;
    }

    return fujinet::io::StatusCode::Ok;
}

fujinet::io::StatusCode TcpNetworkProtocolEspIdf::write_body(std::uint32_t offset,
                                                             const std::uint8_t* data,
                                                             std::size_t len,
                                                             std::uint16_t& written)
{
    written = 0;

    if (offset != _write_cursor) {
        return fujinet::io::StatusCode::InvalidRequest;
    }

    if (len == 0) {
        if (_opt.halfclose && _fd >= 0) {
            (void)lwip_shutdown(_fd, SHUT_WR);
        }
        return fujinet::io::StatusCode::Ok;
    }

    if (!data) return fujinet::io::StatusCode::InvalidRequest;

    if (_state == State::Connecting) return fujinet::io::StatusCode::NotReady;
    if (_state == State::Error) return fujinet::io::StatusCode::IOError;
    if (!(_state == State::Connected || _state == State::PeerClosed)) return fujinet::io::StatusCode::IOError;
    if (_fd < 0) return fujinet::io::StatusCode::IOError;

    const int n = lwip_send(_fd, data, len, MSG_DONTWAIT);
    if (n > 0) {
        const std::size_t nn = static_cast<std::size_t>(n);
        _write_cursor += static_cast<std::uint32_t>(nn);
        written = static_cast<std::uint16_t>(std::min<std::size_t>(nn, 0xFFFF));
        return fujinet::io::StatusCode::Ok;
    }
    if (n == 0) {
        return fujinet::io::StatusCode::DeviceBusy;
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
        return fujinet::io::StatusCode::DeviceBusy;
    }

    set_error(errno);
    return fujinet::io::StatusCode::IOError;
}

fujinet::io::StatusCode TcpNetworkProtocolEspIdf::read_body(std::uint32_t offset,
                                                            std::uint8_t* out,
                                                            std::size_t outLen,
                                                            std::uint16_t& read,
                                                            bool& eof)
{
    read = 0;
    eof = false;

    if (offset != _read_cursor) {
        return fujinet::io::StatusCode::InvalidRequest;
    }

    if (_state == State::Connecting) return fujinet::io::StatusCode::NotReady;
    if (_state == State::Error) return fujinet::io::StatusCode::IOError;

    pump_recv();

    const std::size_t avail = rx_available();
    if (avail == 0) {
        if (_state == State::PeerClosed) {
            eof = true;
            return fujinet::io::StatusCode::Ok;
        }
        return fujinet::io::StatusCode::NotReady;
    }

    const std::size_t n = std::min(avail, outLen);
    if (n == 0) {
        if (_state == State::PeerClosed) {
            eof = true;
            return fujinet::io::StatusCode::Ok;
        }
        return fujinet::io::StatusCode::NotReady;
    }

    std::size_t remaining = n;
    while (remaining > 0) {
        const std::size_t contig =
            (_rx_tail >= _rx_head || _rx_full)
                ? std::min(remaining, _rx.size() - _rx_head)
                : std::min(remaining, _rx_tail - _rx_head);

        if (contig == 0) break;

        if (out) {
            std::memcpy(out + (n - remaining), &_rx[_rx_head], contig);
        }
        _rx_head = (_rx_head + contig) % _rx.size();
        _rx_full = false;
        remaining -= contig;
    }

    const std::size_t actual = n - remaining;
    _read_cursor += static_cast<std::uint32_t>(actual);
    read = static_cast<std::uint16_t>(std::min<std::size_t>(actual, 0xFFFF));

    if (_state == State::PeerClosed && rx_available() == 0) {
        eof = true;
    }

    return fujinet::io::StatusCode::Ok;
}

fujinet::io::StatusCode TcpNetworkProtocolEspIdf::info(std::size_t maxHeaderBytes,
                                                       fujinet::io::NetworkInfo& out)
{
    out = fujinet::io::NetworkInfo{};
    out.hasHttpStatus = false;
    out.hasContentLength = false;
    out.httpStatus = 0;
    out.contentLength = 0;

    if (_state == State::Error) {
        return fujinet::io::StatusCode::IOError;
    }

    if (maxHeaderBytes > 0) {
        const std::string hdr = build_info_headers();
        out.headersBlock.assign(hdr.data(), std::min<std::size_t>(hdr.size(), maxHeaderBytes));
    }

    return fujinet::io::StatusCode::Ok;
}

void TcpNetworkProtocolEspIdf::poll()
{
    if (_state == State::Connecting) {
        step_connect();
    }
    if (_state == State::Connected || _state == State::PeerClosed) {
        pump_recv();
    }
}

void TcpNetworkProtocolEspIdf::close()
{
    if (_fd >= 0) {
        lwip_close(_fd);
        _fd = -1;
    }
    reset_state();
    _host.clear();
    _port = 0;
    _opt = Options{};
}

} // namespace fujinet::platform::esp32
