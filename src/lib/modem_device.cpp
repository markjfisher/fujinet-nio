#include "fujinet/io/devices/modem_device.h"

#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/byte_codec.h"
#include "fujinet/io/devices/modem_commands.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fujinet::io {

using fujinet::io::bytecodec::Reader;
using fujinet::io::protocol::ModemCommand;

static std::vector<std::uint8_t> to_vec(const std::string& s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

static std::string to_upper_ascii(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

static std::string trim_ascii(std::string_view s)
{
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    std::size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return std::string(s.substr(b, e - b));
}

ModemDevice::ModemDevice(fujinet::net::ITcpSocketOps& socketOps)
    : _toHost(HOST_RX_BUF)
    , _toNet(NET_TX_BUF)
    , _sockOps(socketOps)
    , _tcp(socketOps)
{
    _cmdBuf.reserve(128);
    reset_to_idle();
}

void ModemDevice::reset_to_idle()
{
    close_network();
    stop_listen();

    _cmdMode = true;
    _plusCount = 0;
    _plusTick = 0;

    _hostWriteCursor = 0;
    _hostReadCursor = 0;
    _netWriteCursor = 0;
    _netReadCursor = 0;

    _answered = false;
    _answerAtTick = 0;

    _cmdBuf.clear();
    _toHost.clear();
    _toNet.clear();
}

bool ModemDevice::is_connected() const noexcept
{
    // If the TCP backend is usable, treat as "connected". We also treat PeerClosed as connected
    // until we drain buffered bytes and then emit disconnect semantics.
    const auto st = _tcp.state();
    return (st == fujinet::net::TcpNetworkProtocolCommon::State::Connected) ||
           (st == fujinet::net::TcpNetworkProtocolCommon::State::PeerClosed);
}

void ModemDevice::close_network()
{
    _tcp.close();
    _netWriteCursor = 0;
    _netReadCursor = 0;
}

// ----------------------------
// Host output helpers
// ----------------------------
void ModemDevice::emit_crlf()
{
    _toHost.push(static_cast<std::uint8_t>('\r'));
    _toHost.push(static_cast<std::uint8_t>('\n'));
}

void ModemDevice::emit_text(std::string_view s)
{
    _toHost.push_bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

void ModemDevice::emit_line(std::string_view s)
{
    emit_text(s);
    emit_crlf();
}

void ModemDevice::emit_result_ok()
{
    if (_numericResult) {
        emit_text("0");
        emit_crlf();
        return;
    }
    emit_line("OK");
}

void ModemDevice::emit_result_error()
{
    if (_numericResult) {
        emit_text("4");
        emit_crlf();
        return;
    }
    emit_line("ERROR");
}

void ModemDevice::emit_result_no_carrier()
{
    if (_numericResult) {
        emit_text("3");
        emit_crlf();
        return;
    }
    emit_line("NO CARRIER");
}

void ModemDevice::emit_result_ring()
{
    if (_numericResult) {
        emit_text("2");
        emit_crlf();
        return;
    }
    emit_line("RING");
}

void ModemDevice::emit_result_connect()
{
    if (_numericResult) {
        // Speed-specific result codes (matches old firmware mapping).
        // 300->1, 1200->5, 2400->10, 4800->18, 9600->13, 19200->85
        int rc = 1;
        switch (_modemBaud) {
            case 300:   rc = 1;  break;
            case 1200:  rc = 5;  break;
            case 2400:  rc = 10; break;
            case 4800:  rc = 18; break;
            case 9600:  rc = 13; break;
            case 19200: rc = 85; break;
            default:    rc = 1;  break;
        }
        emit_text(std::to_string(rc));
        emit_crlf();
        return;
    }

    emit_text("CONNECT ");
    emit_text(std::to_string(_modemBaud));
    emit_crlf();
}

// ----------------------------
// Listen/answer
// ----------------------------
StatusCode ModemDevice::start_listen(std::uint16_t port)
{
    stop_listen();

    if (port == 0) {
        return StatusCode::InvalidRequest;
    }

    // Resolve passive bind addresses for this port.
    const void* hints = _sockOps.tcp_stream_passive_addrinfo_hints();
    fujinet::net::AddrInfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    const int gai = _sockOps.getaddrinfo(nullptr, portStr.c_str(), hints, &res);
    if (gai != 0 || !res) {
        if (res) _sockOps.freeaddrinfo(res);
        return StatusCode::IOError;
    }

    int fd = -1;
    int lastErr = 0;

    for (auto* ai = res; ai; ai = _sockOps.addrinfo_next(ai)) {
        fd = _sockOps.socket(_sockOps.addrinfo_family(ai),
                             _sockOps.addrinfo_socktype(ai),
                             _sockOps.addrinfo_protocol(ai));
        if (fd < 0) {
            lastErr = _sockOps.last_errno();
            continue;
        }

        _sockOps.apply_listen_socket_options(fd);

        fujinet::net::SockLen addrlen = 0;
        const struct sockaddr* addr = _sockOps.addrinfo_addr(ai, &addrlen);
        if (_sockOps.bind(fd, addr, addrlen) != 0) {
            lastErr = _sockOps.last_errno();
            _sockOps.close(fd);
            fd = -1;
            continue;
        }
        if (_sockOps.listen(fd, 1) != 0) {
            lastErr = _sockOps.last_errno();
            _sockOps.close(fd);
            fd = -1;
            continue;
        }

        (void)_sockOps.set_nonblocking(fd);
        break;
    }

    _sockOps.freeaddrinfo(res);

    if (fd < 0) {
        (void)lastErr;
        return StatusCode::IOError;
    }

    _listenFd = fd;
    _listenPort = port;
    _lastRingTick = 0;
    _pendingSinceTick = 0;
    return StatusCode::Ok;
}

void ModemDevice::stop_listen()
{
    if (_pendingFd >= 0) {
        _sockOps.close(_pendingFd);
        _pendingFd = -1;
    }
    if (_listenFd >= 0) {
        _sockOps.close(_listenFd);
        _listenFd = -1;
    }
    _listenPort = 0;
    _pendingSinceTick = 0;
    _lastRingTick = 0;
}

void ModemDevice::answer_pending()
{
    if (_pendingFd < 0) return;

    // Adopt into the TCP backend.
    fujinet::net::TcpNetworkProtocolCommon::Options opt{};
    opt.nodelay = true;
    opt.keepalive = false;
    opt.rx_buf = 8192;
    opt.halfclose = true;

    if (_tcp.adopt_connected_socket(_pendingFd, opt, {}, 0) != StatusCode::Ok) {
        _sockOps.close(_pendingFd);
        _pendingFd = -1;
        emit_result_error();
        return;
    }

    _pendingFd = -1;
    _netWriteCursor = 0;
    _netReadCursor = 0;

    _cmdMode = false;
    _answered = false;
    _answerAtTick = _tickNow + ANSWER_DELAY_TICKS;

    if (_useTelnet) {
        telnet_on_connect();
    }
}

void ModemDevice::poll_listen()
{
    if (_listenFd < 0) return;

    // If we already have a pending client, ring/time it out.
    if (_pendingFd >= 0) {
        const std::uint64_t age = _tickNow - _pendingSinceTick;
        if (_autoAnswer) {
            answer_pending();
            return;
        }

        if (age > RING_TIMEOUT_TICKS) {
            // Drop the pending caller.
            _sockOps.close(_pendingFd);
            _pendingFd = -1;
            _pendingSinceTick = 0;
            return;
        }

        if ((_tickNow - _lastRingTick) >= RING_INTERVAL_TICKS) {
            emit_result_ring();
            _lastRingTick = _tickNow;
        }

        return;
    }

    // No pending client; try to accept.
    const int cfd = _sockOps.accept(_listenFd, nullptr, nullptr);
    if (cfd < 0) {
        const int err = _sockOps.last_errno();
        if (_sockOps.is_would_block(err)) {
            return;
        }
        // accept error: ignore for now (could log later)
        return;
    }

    (void)_sockOps.set_nonblocking(cfd);
    _sockOps.apply_stream_socket_options(cfd, /*nodelay=*/true, /*keepalive=*/false);

    _pendingFd = cfd;
    _pendingSinceTick = _tickNow;
    _lastRingTick = _tickNow; // first ring emitted after interval

    if (_autoAnswer) {
        answer_pending();
        return;
    }
}

// ----------------------------
// TCP polling
// ----------------------------
void ModemDevice::poll_tcp_rx()
{
    // Always poll the backend so nonblocking connect can complete.
    _tcp.poll();

    const auto st_now = _tcp.state();
    if (st_now == fujinet::net::TcpNetworkProtocolCommon::State::Connecting) {
        return;
    }
    if (!is_connected()) return;

    std::uint8_t tmp[512];
    std::uint16_t n = 0;
    bool eof = false;

    // Drain whatever is currently buffered by the TCP backend.
    while (true) {
        const StatusCode st = _tcp.read_body(_netReadCursor, tmp, sizeof(tmp), n, eof);
        if (st == StatusCode::NotReady) {
            break;
        }
        if (st != StatusCode::Ok) {
            // Treat as disconnect
            close_network();
            _cmdMode = true;
            emit_result_no_carrier();
            break;
        }
        if (n > 0) {
            _netReadCursor += n;
            if (_useTelnet) {
                telnet_filter_incoming(tmp, n);
            } else {
                _toHost.push_bytes(tmp, n);
            }
        }
        if (eof) {
            close_network();
            _cmdMode = true;
            emit_result_no_carrier();
            break;
        }
        if (n == 0) break;
    }
}

void ModemDevice::poll_tcp_tx()
{
    const auto st_now = _tcp.state();
    if (st_now == fujinet::net::TcpNetworkProtocolCommon::State::Connecting) {
        return;
    }
    if (!is_connected()) return;
    if (_toNet.size() == 0) return;

    std::uint8_t tmp[256];
    const std::size_t want = std::min<std::size_t>(sizeof(tmp), _toNet.size());
    const std::size_t got = _toNet.pop_bytes(tmp, want);
    if (got == 0) return;

    std::uint16_t written = 0;
    const StatusCode st = _tcp.write_body(_netWriteCursor, tmp, got, written);
    if (st == StatusCode::Ok) {
        _netWriteCursor += written;
        // If written < got (should not happen for this backend), we drop the remainder.
        return;
    }

    // Could not write now; push back what we popped (best-effort).
    // NOTE: order preservation is more important than perfection; if ring is full we drop.
    _toNet.push_bytes(tmp, got);
}

// ----------------------------
// Telnet minimal implementation
// ----------------------------
static constexpr std::uint8_t IAC  = 255;
static constexpr std::uint8_t DONT = 254;
static constexpr std::uint8_t DO   = 253;
static constexpr std::uint8_t WONT = 252;
static constexpr std::uint8_t WILL = 251;
static constexpr std::uint8_t SB   = 250;
static constexpr std::uint8_t SE   = 240;

static constexpr std::uint8_t TELOPT_ECHO   = 1;
static constexpr std::uint8_t TELOPT_TTYPE  = 24;
static constexpr std::uint8_t TELOPT_COMP2  = 86;
static constexpr std::uint8_t TELOPT_MSSP   = 70;

static constexpr std::uint8_t TTYPE_IS   = 0;
static constexpr std::uint8_t TTYPE_SEND = 1;

void ModemDevice::telnet_on_connect()
{
    // Send a small, conservative negotiation.
    std::uint8_t neg[] = {
        IAC, WONT, TELOPT_ECHO,
        IAC, WILL, TELOPT_TTYPE,
        IAC, WONT, TELOPT_COMP2,
        IAC, WONT, TELOPT_MSSP,
    };
    telnet_escape_and_queue_outgoing(neg, sizeof(neg));
}

void ModemDevice::telnet_escape_and_queue_outgoing(const std::uint8_t* in, std::size_t n)
{
    if (!in || n == 0) return;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t b = in[i];
        if (b == IAC) {
            _toNet.push(IAC);
            _toNet.push(IAC);
        } else {
            _toNet.push(b);
        }
    }
}

void ModemDevice::telnet_filter_incoming(const std::uint8_t* in, std::size_t n)
{
    if (!in || n == 0) return;

    std::size_t i = 0;
    while (i < n) {
        const std::uint8_t b = in[i++];
        if (b != IAC) {
            _toHost.push(b);
            continue;
        }

        if (i >= n) break;
        const std::uint8_t cmd = in[i++];

        if (cmd == IAC) {
            _toHost.push(IAC);
            continue;
        }

        // Negotiation: IAC {DO/DONT/WILL/WONT} opt
        if (cmd == DO || cmd == DONT || cmd == WILL || cmd == WONT) {
            if (i >= n) break;
            const std::uint8_t opt = in[i++];

            // Handle echo semantics similarly to old code:
            if (opt == TELOPT_ECHO) {
                if (cmd == WILL) _commandEcho = false;
                if (cmd == WONT) _commandEcho = true;
            }

            // Respond to a few options; otherwise refuse.
            std::uint8_t resp_cmd = 0;
            if (opt == TELOPT_TTYPE) {
                resp_cmd = (cmd == DO) ? WILL : (cmd == WILL ? DO : 0);
            } else {
                // Default: refuse
                resp_cmd = (cmd == DO) ? WONT : (cmd == WILL ? DONT : 0);
            }

            if (resp_cmd != 0) {
                std::uint8_t resp[] = {IAC, resp_cmd, opt};
                telnet_escape_and_queue_outgoing(resp, sizeof(resp));
            }
            continue;
        }

        // Subnegotiation: IAC SB ... IAC SE
        if (cmd == SB) {
            if (i >= n) break;
            const std::uint8_t opt = in[i++];

            // find IAC SE
            std::size_t sb_start = i;
            while (i + 1 < n) {
                if (in[i] == IAC && in[i + 1] == SE) break;
                ++i;
            }
            const bool has_end = (i + 1 < n && in[i] == IAC && in[i + 1] == SE);
            const std::size_t sb_len = has_end ? (i - sb_start) : 0;

            if (has_end) {
                // Handle TTYPE SEND
                if (opt == TELOPT_TTYPE && sb_len >= 1) {
                    const std::uint8_t tcmd = in[sb_start];
                    if (tcmd == TTYPE_SEND) {
                        // IAC SB TTYPE IS <term> IAC SE
                        std::vector<std::uint8_t> out;
                        out.reserve(6 + _termType.size());
                        out.push_back(IAC);
                        out.push_back(SB);
                        out.push_back(TELOPT_TTYPE);
                        out.push_back(TTYPE_IS);
                        for (char c : _termType) out.push_back(static_cast<std::uint8_t>(c));
                        out.push_back(IAC);
                        out.push_back(SE);
                        telnet_escape_and_queue_outgoing(out.data(), out.size());
                    }
                }

                // consume IAC SE
                i += 2;
            }
            continue;
        }

        // Unknown telnet command: ignore.
    }
}

// ----------------------------
// AT + host input processing
// ----------------------------
void ModemDevice::process_at_command(std::string_view cmdUpper)
{
    // cmdUpper is already trimmed + uppercased.
    if (cmdUpper == "AT") {
        emit_result_ok();
        return;
    }

    if (cmdUpper == "ATV0") {
        _numericResult = true;
        emit_result_ok();
        return;
    }
    if (cmdUpper == "ATV1") {
        _numericResult = false;
        emit_result_ok();
        return;
    }
    if (cmdUpper == "ATE0") {
        _commandEcho = false;
        emit_result_ok();
        return;
    }
    if (cmdUpper == "ATE1") {
        _commandEcho = true;
        emit_result_ok();
        return;
    }
    if (cmdUpper == "ATNET0") {
        _useTelnet = false;
        emit_result_ok();
        return;
    }
    if (cmdUpper == "ATNET1") {
        _useTelnet = true;
        emit_result_ok();
        return;
    }
    if (cmdUpper == "ATS0=0") {
        _autoAnswer = false;
        emit_result_ok();
        return;
    }
    if (cmdUpper == "ATS0=1") {
        _autoAnswer = true;
        emit_result_ok();
        return;
    }

    // Baud rate selection (common legacy-friendly shorthand).
    // NOTE: only affects CONNECT messaging and status reporting in NIO.
    if (cmdUpper == "ATB300")  { if (!_baudLock) _modemBaud = 300;  emit_result_ok(); return; }
    if (cmdUpper == "ATB600")  { if (!_baudLock) _modemBaud = 600;  emit_result_ok(); return; }
    if (cmdUpper == "ATB1200") { if (!_baudLock) _modemBaud = 1200; emit_result_ok(); return; }
    if (cmdUpper == "ATB1800") { if (!_baudLock) _modemBaud = 1800; emit_result_ok(); return; }
    if (cmdUpper == "ATB2400") { if (!_baudLock) _modemBaud = 2400; emit_result_ok(); return; }
    if (cmdUpper == "ATB4800") { if (!_baudLock) _modemBaud = 4800; emit_result_ok(); return; }
    if (cmdUpper == "ATB9600") { if (!_baudLock) _modemBaud = 9600; emit_result_ok(); return; }
    if (cmdUpper == "ATB19200"){ if (!_baudLock) _modemBaud = 19200;emit_result_ok(); return; }

    // Baud lock (legacy-ish): AT+BAUDLOCK=0/1
    if (cmdUpper == "AT+BAUDLOCK=0") { _baudLock = false; emit_result_ok(); return; }
    if (cmdUpper == "AT+BAUDLOCK=1") { _baudLock = true;  emit_result_ok(); return; }

    if (cmdUpper == "ATH" || cmdUpper == "+++ATH" || cmdUpper == "ATH1") {
        if (is_connected()) {
            close_network();
            _cmdMode = true;
            emit_result_no_carrier();
        } else {
            emit_result_ok();
        }
        return;
    }

    if (cmdUpper == "ATA") {
        if (_pendingFd >= 0) {
            answer_pending();
            emit_result_ok(); // connect result is emitted after delay
        } else {
            emit_result_error();
        }
        return;
    }

    if (cmdUpper.rfind("ATPORT", 0) == 0) {
        // ATPORT<port>
        std::string arg = trim_ascii(cmdUpper.substr(6));
        if (arg.empty()) {
            emit_result_error();
            return;
        }
        int port = 0;
        for (char c : arg) {
            if (c < '0' || c > '9') { port = -1; break; }
            port = port * 10 + (c - '0');
            if (port > 65535) { port = -1; break; }
        }
        if (port <= 0) {
            emit_result_error();
            return;
        }
        const StatusCode st = start_listen(static_cast<std::uint16_t>(port));
        if (st == StatusCode::Ok) emit_result_ok();
        else emit_result_error();
        return;
    }

    if (cmdUpper.rfind("ATDT", 0) == 0 || cmdUpper.rfind("ATDP", 0) == 0 || cmdUpper.rfind("ATDI", 0) == 0) {
        // ATDT<host[:port]> (default telnet port 23 if none)
        std::string arg = trim_ascii(cmdUpper.substr(4));
        if (arg.empty()) {
            emit_result_error();
            return;
        }
        const StatusCode st = dial_hostport(arg);
        if (st == StatusCode::Ok) {
            // connect result is emitted after delay (matches old behavior)
            _cmdMode = false;
            _answered = false;
            _answerAtTick = _tickNow + ANSWER_DELAY_TICKS;
        } else {
            emit_result_no_carrier();
        }
        return;
    }

    if (cmdUpper.rfind("AT+TERM=", 0) == 0) {
        std::string v = trim_ascii(cmdUpper.substr(8));
        if (v == "VT52" || v == "VT100" || v == "ANSI" || v == "DUMB") {
            _termType = v;
            emit_result_ok();
        } else {
            emit_result_error();
        }
        return;
    }

    if (cmdUpper == "ATO") {
        if (is_connected()) {
            emit_result_connect();
            _cmdMode = false;
            return;
        }
        emit_result_ok();
        return;
    }

    // Unsupported in NIO at the moment (wifi helpers etc)
    emit_result_error();
}

StatusCode ModemDevice::dial_hostport(std::string_view hostPort)
{
    // Basic parse: host[:port]
    std::string hp = trim_ascii(hostPort);
    if (hp.empty()) return StatusCode::InvalidRequest;

    std::string host;
    std::uint16_t port = 23;

    const std::size_t colon = hp.find(':');
    if (colon == std::string::npos) {
        host = hp;
    } else {
        host = hp.substr(0, colon);
        std::string p = hp.substr(colon + 1);
        int pi = 0;
        for (char c : p) {
            if (c < '0' || c > '9') return StatusCode::InvalidRequest;
            pi = pi * 10 + (c - '0');
            if (pi > 65535) return StatusCode::InvalidRequest;
        }
        if (pi <= 0) return StatusCode::InvalidRequest;
        port = static_cast<std::uint16_t>(pi);
    }

    if (host.empty()) return StatusCode::InvalidRequest;

    // Use the shared TCP backend by building a tcp:// URL.
    fujinet::io::NetworkOpenRequest req{};
    req.method = 1;
    req.flags = 0;
    req.url = "tcp://" + host + ":" + std::to_string(port);

    const StatusCode st = _tcp.open(req);
    if (st != StatusCode::Ok) {
        close_network();
        return st;
    }

    _netReadCursor = 0;
    _netWriteCursor = 0;

    if (_useTelnet) {
        telnet_on_connect();
    }

    return StatusCode::Ok;
}

void ModemDevice::process_host_byte(std::uint8_t b)
{
    // In command mode, implement an AT interpreter similar to old firmware.
    if (_cmdMode) {
        // Echo input back to host if enabled.
        if (_commandEcho) {
            _toHost.push(b);
        }

        if (b == '\r' || b == '\n') {
            // terminate command
            if (_commandEcho) {
                // ensure canonical newline after the command line
                emit_crlf();
            }

            const std::string trimmed = trim_ascii(_cmdBuf);
            _cmdBuf.clear();
            if (trimmed.empty()) return;

            const std::string up = to_upper_ascii(trimmed);
            process_at_command(up);
            return;
        }

        // Backspace handling (very small)
        if (b == 0x08 || b == 0x7F) {
            if (!_cmdBuf.empty()) {
                _cmdBuf.pop_back();
            }
            return;
        }

        if (_cmdBuf.size() < 256) {
            _cmdBuf.push_back(static_cast<char>(b));
        }
        return;
    }

    // Connected mode: detect "+++" escape sequence (with a simple guard time).
    if (b == '+') {
        _plusCount++;
        if (_plusCount >= 3) {
            _plusTick = _tickNow;
        }
    } else {
        _plusCount = 0;
    }

    if (_useTelnet) {
        telnet_escape_and_queue_outgoing(&b, 1);
    } else {
        _toNet.push(b);
    }
}

// ----------------------------
// Device poll + handle()
// ----------------------------
void ModemDevice::poll()
{
    ++_tickNow;

    poll_listen();

    // Allow connect progress even before we consider ourselves "connected".
    _tcp.poll();

    // If dial/answer failed during connect, collapse to command mode.
    if (_tcp.state() == fujinet::net::TcpNetworkProtocolCommon::State::Error) {
        if (!_cmdMode) {
            close_network();
            _cmdMode = true;
            _answered = false;
            _answerAtTick = 0;
            emit_result_no_carrier();
        }
    }

    // Emit CONNECT only once, and only after:
    // - our answer delay elapsed, AND
    // - the TCP socket is actually connected (or peer closed after connect).
    if (!_cmdMode && !_answered && _answerAtTick > 0 && _tickNow >= _answerAtTick) {
        if (is_connected()) {
            _answered = true;
            _answerAtTick = 0;
            emit_result_connect();
        }
    }

    // escape guard: if we saw "+++" and no other bytes for ~1s, return to command mode
    if (_plusCount >= 3) {
        if ((_tickNow - _plusTick) > ANSWER_DELAY_TICKS) {
            _plusCount = 0;
            _cmdMode = true;
            emit_result_ok();
        }
    }

    poll_tcp_tx();
    poll_tcp_rx();
}

IOResponse ModemDevice::handle(const IORequest& request)
{
    const auto cmd = protocol::to_modem_command(request.command);

    switch (cmd) {
        case ModemCommand::Write: {
            auto resp = make_success_response(request);

            Reader r(request.payload.data(), request.payload.size());
            std::uint8_t ver = 0;
            std::uint32_t offset = 0;
            std::uint16_t len = 0;

            if (!r.read_u8(ver) || ver != MODEM_VERSION) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            if (!r.read_u32le(offset) || !r.read_u16le(len)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            const std::uint8_t* p = nullptr;
            if (!r.read_bytes(p, len) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            // enforce sequential offsets
            if (offset != _hostWriteCursor) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            // consume bytes
            for (std::uint16_t i = 0; i < len; ++i) {
                process_host_byte(p[i]);
            }
            _hostWriteCursor += len;

            std::string out;
            out.reserve(1 + 1 + 2 + 4 + 2);
            bytecodec::write_u8(out, MODEM_VERSION);
            bytecodec::write_u8(out, 0);
            bytecodec::write_u16le(out, 0);
            bytecodec::write_u32le(out, offset);
            bytecodec::write_u16le(out, len);
            resp.payload = to_vec(out);
            return resp;
        }

        case ModemCommand::Read: {
            auto resp = make_success_response(request);

            Reader r(request.payload.data(), request.payload.size());
            std::uint8_t ver = 0;
            std::uint32_t offset = 0;
            std::uint16_t maxBytes = 0;

            if (!r.read_u8(ver) || ver != MODEM_VERSION) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            if (!r.read_u32le(offset) || !r.read_u16le(maxBytes) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            if (offset != _hostReadCursor) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::vector<std::uint8_t> data;
            data.resize(maxBytes);
            const std::size_t n = _toHost.pop_bytes(data.data(), data.size());
            data.resize(n);

            _hostReadCursor += static_cast<std::uint32_t>(n);

            std::string out;
            out.reserve(1 + 1 + 2 + 4 + 2 + n);
            bytecodec::write_u8(out, MODEM_VERSION);
            bytecodec::write_u8(out, 0);
            bytecodec::write_u16le(out, 0);
            bytecodec::write_u32le(out, offset);
            bytecodec::write_u16le(out, static_cast<std::uint16_t>(n));
            if (n > 0) {
                bytecodec::write_bytes(out, data.data(), n);
            }
            resp.payload = to_vec(out);
            return resp;
        }

        case ModemCommand::Status: {
            auto resp = make_success_response(request);

            Reader r(request.payload.data(), request.payload.size());
            std::uint8_t ver = 0;
            if (!r.read_u8(ver) || ver != MODEM_VERSION || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::uint8_t flags = 0;
            if (_cmdMode) flags |= 0x01;
            if (is_connected()) flags |= 0x02;
            if (_listenFd >= 0) flags |= 0x04;
            if (_pendingFd >= 0) flags |= 0x08;
            if (_autoAnswer) flags |= 0x10;
            if (_useTelnet) flags |= 0x20;
            if (_commandEcho) flags |= 0x40;
            if (_numericResult) flags |= 0x80;

            std::string out;
            // v1 response payload:
            //   u8  version
            //   u8  flags
            //   u16 reserved
            //   u16 listenPort
            //   u32 hostRxAvail
            //   u32 hostWriteCursor
            //   u32 hostReadCursor
            //   u32 netReadCursor
            //   u32 netWriteCursor
            out.reserve(1 + 1 + 2 + 2 + 4 + 4 + 4 + 4 + 4);
            bytecodec::write_u8(out, MODEM_VERSION);
            bytecodec::write_u8(out, flags);
            bytecodec::write_u16le(out, 0);
            bytecodec::write_u16le(out, _listenPort);
            bytecodec::write_u32le(out, static_cast<std::uint32_t>(_toHost.size()));
            // NB: hostTxCursor is the next required offset for host write.
            bytecodec::write_u32le(out, _hostWriteCursor);
            bytecodec::write_u32le(out, _hostReadCursor);
            bytecodec::write_u32le(out, _netReadCursor);
            bytecodec::write_u32le(out, _netWriteCursor);

            resp.payload = to_vec(out);
            return resp;
        }

        case ModemCommand::Control: {
            auto resp = make_success_response(request);

            Reader r(request.payload.data(), request.payload.size());
            std::uint8_t ver = 0;
            std::uint8_t op = 0;
            if (!r.read_u8(ver) || ver != MODEM_VERSION || !r.read_u8(op)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            switch (op) {
                case 0x01: { // hangup
                    const bool wasConnected = is_connected();
                    close_network();
                    _cmdMode = true;
                    if (wasConnected) {
                        emit_result_no_carrier();
                    } else {
                        emit_result_ok();
                    }
                    break;
                }
                case 0x02: { // dial: lp_u16 string host[:port]
                    std::string_view hp;
                    if (!r.read_lp_u16_string(hp) || r.remaining() != 0) {
                        resp.status = StatusCode::InvalidRequest;
                        return resp;
                    }
                    const StatusCode st = dial_hostport(hp);
                    if (st != StatusCode::Ok) {
                        resp.status = st;
                        return resp;
                    }
                    _cmdMode = false;
                    _answered = false;
                    _answerAtTick = _tickNow + ANSWER_DELAY_TICKS;
                    break;
                }
                case 0x03: { // listen: u16 port
                    std::uint16_t p = 0;
                    if (!r.read_u16le(p) || r.remaining() != 0) {
                        resp.status = StatusCode::InvalidRequest;
                        return resp;
                    }
                    resp.status = start_listen(p);
                    break;
                }
                case 0x04: { // unlisten
                    stop_listen();
                    break;
                }
                case 0x05: { // answer
                    answer_pending();
                    break;
                }
                case 0x06: { // set autoanswer (u8)
                    std::uint8_t v = 0;
                    if (!r.read_u8(v) || r.remaining() != 0) {
                        resp.status = StatusCode::InvalidRequest;
                        return resp;
                    }
                    _autoAnswer = (v != 0);
                    break;
                }
                case 0x0B: { // set baud (u32)
                    std::uint32_t b = 0;
                    if (!r.read_u32le(b) || r.remaining() != 0) {
                        resp.status = StatusCode::InvalidRequest;
                        return resp;
                    }
                    if (!_baudLock) {
                        if (b == 300 || b == 600 || b == 1200 || b == 1800 ||
                            b == 2400 || b == 4800 || b == 9600 || b == 19200) {
                            _modemBaud = b;
                        } else {
                            resp.status = StatusCode::InvalidRequest;
                            return resp;
                        }
                    }
                    break;
                }
                case 0x0C: { // baud lock (u8)
                    std::uint8_t v = 0;
                    if (!r.read_u8(v) || r.remaining() != 0) {
                        resp.status = StatusCode::InvalidRequest;
                        return resp;
                    }
                    _baudLock = (v != 0);
                    break;
                }
                case 0x07: { // set telnet (u8)
                    std::uint8_t v = 0;
                    if (!r.read_u8(v) || r.remaining() != 0) {
                        resp.status = StatusCode::InvalidRequest;
                        return resp;
                    }
                    _useTelnet = (v != 0);
                    break;
                }
                case 0x08: { // set echo (u8)
                    std::uint8_t v = 0;
                    if (!r.read_u8(v) || r.remaining() != 0) {
                        resp.status = StatusCode::InvalidRequest;
                        return resp;
                    }
                    _commandEcho = (v != 0);
                    break;
                }
                case 0x09: { // set numeric result (u8)
                    std::uint8_t v = 0;
                    if (!r.read_u8(v) || r.remaining() != 0) {
                        resp.status = StatusCode::InvalidRequest;
                        return resp;
                    }
                    _numericResult = (v != 0);
                    break;
                }
                case 0x0A: { // reset
                    reset_to_idle();
                    break;
                }
                default:
                    resp.status = StatusCode::Unsupported;
                    return resp;
            }

            std::string out;
            out.reserve(1 + 1 + 2);
            bytecodec::write_u8(out, MODEM_VERSION);
            bytecodec::write_u8(out, 0);
            bytecodec::write_u16le(out, 0);
            resp.payload = to_vec(out);
            return resp;
        }

        default:
            return make_base_response(request, StatusCode::Unsupported);
    }
}

} // namespace fujinet::io


