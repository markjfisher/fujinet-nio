#pragma once

#include "fujinet/io/devices/virtual_device.h"

#include "fujinet/net/tcp_network_protocol_common.h"
#include "fujinet/net/tcp_socket_ops.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::io {

// ModemDevice: stream-oriented modem endpoint (v1).
// - Host writes bytes to the modem (Write)
// - Host reads bytes emitted by the modem (Read)
// - The device optionally interprets AT commands in command mode.
// - Connected mode forwards bytes to/from a TCP (or Telnet) backend.
class ModemDevice : public VirtualDevice {
public:
    explicit ModemDevice(fujinet::net::ITcpSocketOps& socketOps);

    IOResponse handle(const IORequest& request) override;
    void poll() override;

private:
    // Allow out-of-band diagnostics (console) without polluting the on-wire API surface.
    friend struct ModemDeviceDiagnosticsAccessor;

    static constexpr std::uint8_t MODEM_VERSION = 1;

    // Keep memory bounded; this is an 8-bit-centric project.
    static constexpr std::size_t HOST_RX_BUF = 4096; // modem -> host
    static constexpr std::size_t NET_TX_BUF  = 1024; // host -> network (when backpressured)

    // Poll-based timers (assuming ~20-50ms tick, but exact rate is platform-defined)
    static constexpr std::uint64_t RING_INTERVAL_TICKS = 40; // ~2s at 50ms
    static constexpr std::uint64_t RING_TIMEOUT_TICKS  = 40 * 30; // ~60s
    static constexpr std::uint64_t ANSWER_DELAY_TICKS  = 20; // ~1s at 50ms

    struct ByteRing {
        std::vector<std::uint8_t> buf;
        std::size_t head{0};
        std::size_t tail{0};
        bool full{false};

        explicit ByteRing(std::size_t cap)
            : buf(cap, 0)
        {
        }

        std::size_t size() const noexcept
        {
            if (buf.empty()) return 0;
            if (full) return buf.size();
            if (tail >= head) return tail - head;
            return (buf.size() - head) + tail;
        }

        std::size_t capacity() const noexcept { return buf.size(); }
        std::size_t free_space() const noexcept { return buf.size() - size(); }

        void clear() noexcept
        {
            head = 0;
            tail = 0;
            full = false;
        }

        bool push(std::uint8_t b) noexcept
        {
            if (buf.empty()) return false;
            if (full) return false;
            buf[tail] = b;
            tail = (tail + 1) % buf.size();
            if (tail == head) full = true;
            return true;
        }

        std::size_t push_bytes(const std::uint8_t* p, std::size_t n) noexcept
        {
            if (!p || n == 0) return 0;
            std::size_t w = 0;
            for (; w < n; ++w) {
                if (!push(p[w])) break;
            }
            return w;
        }

        bool pop(std::uint8_t& out) noexcept
        {
            if (buf.empty()) return false;
            if (!full && head == tail) return false;
            out = buf[head];
            head = (head + 1) % buf.size();
            full = false;
            return true;
        }

        std::size_t pop_bytes(std::uint8_t* out, std::size_t max) noexcept
        {
            if (!out || max == 0) return 0;
            std::size_t r = 0;
            for (; r < max; ++r) {
                std::uint8_t b = 0;
                if (!pop(b)) break;
                out[r] = b;
            }
            return r;
        }
    };

    enum class LineMode : std::uint8_t {
        AsciiCRLF,
        AtariEOL, // placeholder (we only emit CRLF today)
    };

    // on-wire host stream cursors (sequential offsets)
    std::uint32_t _hostWriteCursor{0}; // bytes consumed from host Write()
    std::uint32_t _hostReadCursor{0};  // bytes returned to host via Read()

    // network stream cursors
    std::uint32_t _netWriteCursor{0};
    std::uint32_t _netReadCursor{0};

    // state
    bool _cmdMode{true};
    bool _useTelnet{true};
    bool _commandEcho{true};
    bool _numericResult{false};
    bool _autoAnswer{false};

    // Informational "modem baud" (used for CONNECT messages and numeric connect codes).
    // This does not reconfigure any physical UART in NIO; it's for host compatibility.
    std::uint32_t _modemBaud{9600};
    bool _baudLock{false};

    std::uint16_t _listenPort{0};
    int _listenFd{-1};
    int _pendingFd{-1}; // accepted but not yet answered

    std::uint64_t _tickNow{0};
    std::uint64_t _lastRingTick{0};
    std::uint64_t _pendingSinceTick{0};
    std::uint64_t _answerAtTick{0};
    bool _answered{false};

    // escape sequence tracking ("+++")
    int _plusCount{0};
    std::uint64_t _plusTick{0};

    // AT command buffer
    std::string _cmdBuf;
    std::string _termType{"DUMB"};

    // buffered data to host and to network
    ByteRing _toHost;
    ByteRing _toNet;

    // TCP stream backend (reused for both dialed and accepted sockets)
    fujinet::net::ITcpSocketOps& _sockOps;
    fujinet::net::TcpNetworkProtocolCommon _tcp;

    // --- helpers ---
    void reset_to_idle();
    void close_network();
    bool is_connected() const noexcept;

    // listen/answer
    fujinet::io::StatusCode start_listen(std::uint16_t port);
    void stop_listen();
    void poll_listen();
    void answer_pending();

    // host output helpers (kept small and allocation-free where possible)
    void emit_crlf();
    void emit_text(std::string_view s);
    void emit_line(std::string_view s);
    void emit_result_ok();
    void emit_result_error();
    void emit_result_no_carrier();
    void emit_result_ring();
    void emit_result_connect();

    // AT parser
    void process_host_byte(std::uint8_t b);
    void process_at_command(std::string_view cmdUpper);
    fujinet::io::StatusCode dial_hostport(std::string_view hostPort);

    // Telnet handling (minimal; enough for common servers)
    void telnet_on_connect();
    void telnet_filter_incoming(const std::uint8_t* in, std::size_t n);
    void telnet_escape_and_queue_outgoing(const std::uint8_t* in, std::size_t n);
    void poll_tcp_rx();
    void poll_tcp_tx();
};

} // namespace fujinet::io


