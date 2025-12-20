#include "fujinet/net/tcp_network_protocol_common.h"

#include "fujinet/core/logging.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string_view>

// Include platform socket/network headers
// Both POSIX and ESP32 (lwIP) provide netdb.h with addrinfo
#ifdef __linux__
#include <netdb.h>
#include <sys/socket.h>
#elif defined(ESP_PLATFORM)
extern "C" {
#include "lwip/netdb.h"
}
#else
// Generic POSIX
#include <netdb.h>
#include <sys/socket.h>
#endif

// Socket constants (platform-agnostic)
#ifndef AF_UNSPEC
#define AF_UNSPEC 0
#endif
#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#endif
#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif
#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif
#ifndef SO_ERROR
#define SO_ERROR 4
#endif
#ifndef SO_KEEPALIVE
#define SO_KEEPALIVE 9
#endif
#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif
#ifndef SHUT_WR
#define SHUT_WR 1
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40
#endif
#ifndef MSG_NOSIGNAL
// MSG_NOSIGNAL is Linux-specific, not available on all platforms
#define MSG_NOSIGNAL 0
#endif

// Error codes
#ifndef EINPROGRESS
#define EINPROGRESS 115
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK 11
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef ECONNRESET
#define ECONNRESET 104
#endif
#ifndef ENOTCONN
#define ENOTCONN 107
#endif
#ifndef EPIPE
#define EPIPE 32
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH 113
#endif

namespace fujinet::net {

static constexpr const char* TAG = "tcp";

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

TcpNetworkProtocolCommon::TcpNetworkProtocolCommon(ITcpSocketOps& socket_ops)
    : _socket_ops(socket_ops)
{
}

TcpNetworkProtocolCommon::~TcpNetworkProtocolCommon()
{
    close();
}

void TcpNetworkProtocolCommon::reset_state()
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

void TcpNetworkProtocolCommon::set_error_from_errno(int e)
{
    _last_errno = e;
    _state = State::Error;
}

bool TcpNetworkProtocolCommon::parse_tcp_url(const std::string& url,
                                             std::string& outHost,
                                             std::uint16_t& outPort,
                                             Options& outOpt)
{
    // Expect: tcp://host:port[?k=v&k=v]
    std::string_view s(url);

    if (!starts_with(s, "tcp://")) return false;
    s = trim_prefix(s, "tcp://");

    // split query
    std::string_view authority = s;
    std::string_view query;
    if (auto qpos = s.find('?'); qpos != std::string_view::npos) {
        authority = s.substr(0, qpos);
        query = s.substr(qpos + 1);
    }

    // authority: host:port or [ipv6]:port
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

    // defaults
    Options opt{};

    // parse query k=v&...
    while (!query.empty()) {
        auto amp = query.find('&');
        std::string_view kv = (amp == std::string_view::npos) ? query : query.substr(0, amp);
        query = (amp == std::string_view::npos) ? std::string_view{} : query.substr(amp + 1);

        if (kv.empty()) continue;
        auto eq = kv.find('=');
        std::string_view k = (eq == std::string_view::npos) ? kv : kv.substr(0, eq);
        std::string_view v = (eq == std::string_view::npos) ? std::string_view{} : kv.substr(eq + 1);

        // no URL decode for now (keep it 8-bit friendly)
        if (k == "connect_timeout_ms") opt.connect_timeout_ms = std::max(0, parse_int(v, opt.connect_timeout_ms));
        else if (k == "io_timeout_ms") opt.io_timeout_ms = std::max(0, parse_int(v, opt.io_timeout_ms));
        else if (k == "nodelay") opt.nodelay = parse_bool(v, opt.nodelay);
        else if (k == "keepalive") opt.keepalive = parse_bool(v, opt.keepalive);
        else if (k == "rx_buf") {
            int n = parse_int(v, static_cast<int>(opt.rx_buf));
            if (n < 256) n = 256;
            if (n > 1'048'576) n = 1'048'576; // sanity cap
            opt.rx_buf = static_cast<std::size_t>(n);
        } else if (k == "halfclose") opt.halfclose = parse_bool(v, opt.halfclose);
        // unknown keys are ignored (forward compatible)
    }

    outHost = std::move(host);
    outPort = static_cast<std::uint16_t>(p);
    outOpt = opt;
    return true;
}

void TcpNetworkProtocolCommon::apply_socket_options()
{
    if (_fd < 0) return;

    if (_opt.nodelay) {
        int v = 1;
        (void)_socket_ops.setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
    }
    if (_opt.keepalive) {
        int v = 1;
        (void)_socket_ops.setsockopt(_fd, SOL_SOCKET, SO_KEEPALIVE, &v, sizeof(v));
    }

    (void)_socket_ops.set_nonblocking(_fd);
}

void TcpNetworkProtocolCommon::handle_recv_error(int errno_val)
{
    if (errno_val == EWOULDBLOCK || errno_val == EAGAIN) {
        // No data yet, not an error
        return;
    }

    if (errno_val == ECONNRESET || errno_val == ENOTCONN || errno_val == EPIPE) {
        // Peer closed/reset connection - treat as EOF
        FN_LOGW(TAG, "TCP peer closed/reset connection (%s, errno=%d); treating as EOF",
                _socket_ops.err_string(errno_val), errno_val);
        _state = State::PeerClosed;
        _peer_closed = true;
        return;
    }

    // Other error
    set_error_from_errno(errno_val);
}

void TcpNetworkProtocolCommon::handle_send_error(int errno_val)
{
    if (errno_val == EWOULDBLOCK || errno_val == EAGAIN) {
        // Would block - not an error, caller should return DeviceBusy
        return;
    }

    if (errno_val == ECONNRESET || errno_val == ENOTCONN || errno_val == EPIPE) {
        // Peer closed/reset connection - treat as EOF
        FN_LOGW(TAG, "TCP peer closed/reset connection (%s, errno=%d); treating as EOF",
                _socket_ops.err_string(errno_val), errno_val);
        _state = State::PeerClosed;
        _peer_closed = true;
        return;
    }

    // Other error
    set_error_from_errno(errno_val);
}

void TcpNetworkProtocolCommon::step_connect()
{
    if (_state != State::Connecting) return;
    if (_fd < 0) return;

    if (!_socket_ops.poll_connect_complete(_fd)) {
        // Still connecting or error
        const int err = _socket_ops.last_errno();
        if (err != 0 && err != EINPROGRESS && err != EWOULDBLOCK && err != EAGAIN) {
            set_error_from_errno(err);
            return;
        }

        // Timeout check
        const std::uint64_t now = _socket_ops.now_ms();
        if (_opt.connect_timeout_ms > 0 &&
            _connect_start_ms > 0 &&
            (now - _connect_start_ms) > static_cast<std::uint64_t>(_opt.connect_timeout_ms)) {
            set_error_from_errno(ETIMEDOUT);
            return;
        }
        return; // still connecting
    }

    // Check for connection error
    const int err = _socket_ops.get_so_error(_fd);
    if (err != 0) {
        set_error_from_errno(err);
        return;
    }

    _state = State::Connected;
}

std::size_t TcpNetworkProtocolCommon::rx_available() const noexcept
{
    if (_rx.empty()) return 0;
    if (_rx_full) return _rx.size();
    if (_rx_tail >= _rx_head) return _rx_tail - _rx_head;
    return (_rx.size() - _rx_head) + _rx_tail;
}

void TcpNetworkProtocolCommon::pump_recv()
{
    if (_fd < 0) return;
    if (!(_state == State::Connected || _state == State::PeerClosed)) return;
    if (_rx.empty()) return;

    while (rx_available() < _rx.size()) {
        // compute contiguous free space
        std::size_t free_total = _rx.size() - rx_available();
        if (free_total == 0) break;

        std::size_t write_pos = _rx_tail;
        std::size_t free_contig = 0;

        if (_rx_full) {
            break;
        } else if (_rx_tail >= _rx_head) {
            free_contig = _rx.size() - _rx_tail;
            if (_rx_head == 0) {
                // cannot use the entire wrap region if it would collide
                // (free_total already accounts for this)
            }
        } else {
            free_contig = _rx_head - _rx_tail;
        }

        free_contig = std::min(free_contig, free_total);
        if (free_contig == 0) break;

        const ssize_t n = _socket_ops.recv(_fd,
                                          reinterpret_cast<void*>(&_rx[write_pos]),
                                          free_contig,
                                          MSG_DONTWAIT);
        if (n > 0) {
            _rx_tail = (_rx_tail + static_cast<std::size_t>(n)) % _rx.size();
            if (_rx_tail == _rx_head) _rx_full = true;
            continue;
        }
        if (n == 0) {
            // peer closed
            _state = State::PeerClosed;
            _peer_closed = true;
            return;
        }
        // n < 0
        const int err = _socket_ops.last_errno();
        handle_recv_error(err);
        return;
    }
}

std::string TcpNetworkProtocolCommon::build_info_headers() const
{
    auto b = std::string{};
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

fujinet::io::StatusCode TcpNetworkProtocolCommon::open(const fujinet::io::NetworkOpenRequest& req,
                                                       int (*resolve_addr)(const char* host, const char* port,
                                                                           const struct addrinfo* hints,
                                                                           struct addrinfo** res),
                                                       void (*free_addr)(struct addrinfo* res))
{
    close();
    reset_state();

    // Ignore HTTP method mapping here; TCP is method-agnostic.
    // Parse tcp://host:port?... from req.url
    if (!parse_tcp_url(req.url, _host, _port, _opt)) {
        return fujinet::io::StatusCode::InvalidRequest;
    }

    _rx.assign(_opt.rx_buf, 0);

    // resolve and connect nonblocking
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* res = nullptr;
    const std::string portStr = std::to_string(_port);
    const int gai = resolve_addr(_host.c_str(), portStr.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        // map DNS failure -> IOError per TODO matrix
        // (gai doesn't set errno reliably)
        set_error_from_errno(EHOSTUNREACH);
        if (res) free_addr(res);
        return fujinet::io::StatusCode::IOError;
    }

    int fd = -1;
    int lastErr = 0;

    for (auto* ai = res; ai; ai = ai->ai_next) {
        fd = _socket_ops.socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            lastErr = _socket_ops.last_errno();
            continue;
        }

        _fd = fd;
        apply_socket_options();

        _connect_start_ms = _socket_ops.now_ms();
        const int cr = _socket_ops.connect(_fd, ai->ai_addr, ai->ai_addrlen);
        if (cr == 0) {
            _state = State::Connected;
            lastErr = 0;
            break;
        }
        const int connect_err = _socket_ops.last_errno();
        if (cr < 0 && (connect_err == EINPROGRESS || connect_err == EWOULDBLOCK || connect_err == EAGAIN)) {
            _state = State::Connecting;
            lastErr = 0;
            break;
        }

        lastErr = connect_err;
        _socket_ops.close(_fd);
        _fd = -1;
    }

    free_addr(res);

    if (_fd < 0) {
        set_error_from_errno(lastErr != 0 ? lastErr : ECONNREFUSED);
        return fujinet::io::StatusCode::IOError;
    }

    return fujinet::io::StatusCode::Ok;
}

fujinet::io::StatusCode TcpNetworkProtocolCommon::write_body(std::uint32_t offset,
                                                             const std::uint8_t* data,
                                                             std::size_t len,
                                                             std::uint16_t& written)
{
    written = 0;

    // sequential stream cursor rule
    if (offset != _write_cursor) {
        return fujinet::io::StatusCode::InvalidRequest;
    }

    // Optional "halfclose" signal: Write with len=0 at current cursor
    if (len == 0) {
        if (_opt.halfclose && _fd >= 0) {
            (void)_socket_ops.shutdown_write(_fd);
        }
        return fujinet::io::StatusCode::Ok;
    }

    if (!data) return fujinet::io::StatusCode::InvalidRequest;

    if (_state == State::Connecting) {
        return fujinet::io::StatusCode::NotReady;
    }
    if (!(_state == State::Connected || _state == State::PeerClosed)) {
        return fujinet::io::StatusCode::IOError;
    }
    if (_fd < 0) return fujinet::io::StatusCode::IOError;

    const ssize_t n = _socket_ops.send(_fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n > 0) {
        const std::size_t nn = static_cast<std::size_t>(n);
        _write_cursor += static_cast<std::uint32_t>(nn);
        written = static_cast<std::uint16_t>(std::min<std::size_t>(nn, 0xFFFF));
        return fujinet::io::StatusCode::Ok;
    }
    if (n == 0) {
        // no progress, treat as backpressure
        return fujinet::io::StatusCode::DeviceBusy;
    }

    const int err = _socket_ops.last_errno();
    if (err == EWOULDBLOCK || err == EAGAIN) {
        return fujinet::io::StatusCode::DeviceBusy;
    }

    handle_send_error(err);
    return fujinet::io::StatusCode::IOError;
}

fujinet::io::StatusCode TcpNetworkProtocolCommon::read_body(std::uint32_t offset,
                                                            std::uint8_t* out,
                                                            std::size_t outLen,
                                                            std::uint16_t& read,
                                                            bool& eof)
{
    read = 0;
    eof = false;

    // sequential stream cursor rule
    if (offset != _read_cursor) {
        return fujinet::io::StatusCode::InvalidRequest;
    }

    if (_state == State::Connecting) {
        return fujinet::io::StatusCode::NotReady;
    }
    if (_state == State::Error) {
        return fujinet::io::StatusCode::IOError;
    }

    // opportunistically pump recv (safe; nonblocking)
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
        // if caller asked for 0, behave "not ready unless EOF"
        if (_state == State::PeerClosed) {
            eof = true;
            return fujinet::io::StatusCode::Ok;
        }
        return fujinet::io::StatusCode::NotReady;
    }

    // read n bytes from ring (may wrap)
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

    // eof only when peer closed AND buffer empty after this read
    if (_state == State::PeerClosed && rx_available() == 0) {
        eof = true;
    }

    return fujinet::io::StatusCode::Ok;
}

fujinet::io::StatusCode TcpNetworkProtocolCommon::info(std::size_t maxHeaderBytes,
                                                       fujinet::io::NetworkInfo& out)
{
    // v1-compatible: no http status, no content-length for tcp
    out = fujinet::io::NetworkInfo{};
    out.hasHttpStatus = false;
    out.hasContentLength = false;
    out.httpStatus = 0;
    out.contentLength = 0;

    if (_state == State::Error) {
        // allow caller to still read headers to get last error, but match TODO contract:
        // use IOError for backend error after open.
        // (If you prefer Ok-with-error-headers, flip this.)
        return fujinet::io::StatusCode::IOError;
    }

    if (maxHeaderBytes > 0) {
        const std::string hdr = build_info_headers();
        out.headersBlock.assign(hdr.data(), std::min<std::size_t>(hdr.size(), maxHeaderBytes));
    }

    return fujinet::io::StatusCode::Ok;
}

void TcpNetworkProtocolCommon::poll()
{
    if (_state == State::Connecting) {
        step_connect();
    }
    if (_state == State::Connected || _state == State::PeerClosed) {
        pump_recv();
    }
}

void TcpNetworkProtocolCommon::close()
{
    if (_fd >= 0) {
        _socket_ops.close(_fd);
        _fd = -1;
    }
    reset_state();
    _host.clear();
    _port = 0;
    _opt = Options{};
}

} // namespace fujinet::net

