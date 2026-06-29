#include "doctest.h"

#include "net_device_test_helpers.h"

// Pull in the TCP backend implementation (POSIX)
#include "fujinet/platform/posix/tcp_network_protocol_posix.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fujinet::tests {

using namespace fujinet::tests::netdev;

struct MemoryTcpSocketOps final : fujinet::net::ITcpSocketOps {
    std::vector<std::uint8_t> rx;
    std::size_t rx_pos = 0;
    int last_error = 0;

    int socket(int, int, int) override { return 1; }
    void close(int) override {}
    int connect(int, const struct sockaddr*, fujinet::net::SockLen) override { return 0; }
    int bind(int, const struct sockaddr*, fujinet::net::SockLen) override { return 0; }
    int listen(int, int) override { return 0; }
    int accept(int, struct sockaddr*, fujinet::net::SockLen*) override { return -1; }
    int set_nonblocking(int) override { return 0; }
    bool poll_connect_complete(int) override { return true; }
    fujinet::net::SSize send(int, const void*, std::size_t len) override
    {
        return static_cast<fujinet::net::SSize>(len);
    }
    fujinet::net::SSize recv(int, void* buf, std::size_t len) override
    {
        const std::size_t remain = rx.size() - rx_pos;
        if (remain == 0) {
            last_error = 11;
            return -1;
        }
        const std::size_t n = std::min(remain, len);
        std::memcpy(buf, rx.data() + rx_pos, n);
        rx_pos += n;
        return static_cast<fujinet::net::SSize>(n);
    }
    int shutdown_write(int) override { return 0; }
    int get_so_error(int) override { return 0; }
    int setsockopt(int, int, int, const void*, fujinet::net::SockLen) override { return 0; }
    void apply_stream_socket_options(int, bool, bool) override {}
    void apply_listen_socket_options(int) override {}
    int getaddrinfo(const char*, const char*, const void*, fujinet::net::AddrInfo**) override { return -1; }
    const void* tcp_stream_addrinfo_hints() const noexcept override { return nullptr; }
    const void* tcp_stream_passive_addrinfo_hints() const noexcept override { return nullptr; }
    void freeaddrinfo(fujinet::net::AddrInfo*) override {}
    fujinet::net::AddrInfo* addrinfo_next(fujinet::net::AddrInfo*) override { return nullptr; }
    int addrinfo_family(fujinet::net::AddrInfo*) override { return 0; }
    int addrinfo_socktype(fujinet::net::AddrInfo*) override { return 0; }
    int addrinfo_protocol(fujinet::net::AddrInfo*) override { return 0; }
    const struct sockaddr* addrinfo_addr(fujinet::net::AddrInfo*, fujinet::net::SockLen*) override { return nullptr; }
    std::uint64_t now_ms() override { return 0; }
    int last_errno() override { return last_error; }
    const char* err_string(int) override { return "memory socket"; }
    bool is_would_block(int errno_val) const noexcept override { return errno_val == 11; }
    bool is_in_progress(int) const noexcept override { return false; }
    bool is_peer_gone(int) const noexcept override { return false; }
    int err_timed_out() const noexcept override { return 110; }
    int err_conn_refused() const noexcept override { return 111; }
    int err_host_unreach() const noexcept override { return 113; }
};

// ------------------------
// Minimal local echo server
// ------------------------
struct LocalEchoServer {
    int listen_fd = -1;
    std::uint16_t port = 0;

    std::thread th;
    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};

    std::atomic<int> last_errno{0};

    LocalEchoServer() = default;

    ~LocalEchoServer() {
        shutdown();
    }

    bool start()
    {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            last_errno.store(errno);
            return false;
        }

        int opt = 1;
        (void)::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
        addr.sin_port = htons(0); // ephemeral

        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            last_errno.store(errno);
            ::close(listen_fd);
            listen_fd = -1;
            return false;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            last_errno.store(errno);
            ::close(listen_fd);
            listen_fd = -1;
            return false;
        }

        port = ntohs(addr.sin_port);

        if (::listen(listen_fd, 1) != 0) {
            last_errno.store(errno);
            ::close(listen_fd);
            listen_fd = -1;
            return false;
        }

        // Server is now "ready" (bound + listening) before thread starts => no race
        ready.store(true);

        th = std::thread([this]() {
            sockaddr_in caddr {};
            socklen_t clen = sizeof(caddr);

            int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&caddr), &clen);
            if (cfd < 0) {
                // accept can fail if shutdown() closed listen_fd
                last_errno.store(errno);
                return;
            }

            std::uint8_t buf[4096];
            while (!stop.load()) {
                const ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
                if (n > 0) {
                    std::size_t off = 0;
                    while (off < static_cast<std::size_t>(n)) {
                        const ssize_t w = ::send(cfd, buf + off,
                                                 static_cast<std::size_t>(n) - off,
                                                 0);
                        if (w > 0) {
                            off += static_cast<std::size_t>(w);
                        } else if (w == 0) {
                            break;
                        } else {
                            last_errno.store(errno);
                            break;
                        }
                    }
                    continue;
                }
                if (n == 0) {
                    break; // peer closed
                }
                last_errno.store(errno);
                break;
            }

            ::close(cfd);
        });

        return true;
    }

    void shutdown()
    {
        stop.store(true);
        if (listen_fd >= 0) {
            ::shutdown(listen_fd, SHUT_RDWR);
            ::close(listen_fd);
            listen_fd = -1;
        }
        if (th.joinable()) {
            th.join();
        }
    }
};


// ------------------------
// Registry helper for TCP
// ------------------------
static fujinet::io::ProtocolRegistry make_registry_tcp_only()
{
    fujinet::io::ProtocolRegistry reg;
    reg.register_scheme("tcp", [] {
        return std::make_unique<fujinet::platform::posix::TcpNetworkProtocolPosix>();
    });
    return reg;
}

// ------------------------
// Response parsing helpers
// ------------------------
struct ReadParsed {
    std::uint8_t  ver = 0;
    std::uint8_t  flags = 0;
    std::uint16_t reserved = 0;
    std::uint16_t handle = 0;
    std::uint32_t offset = 0;
    std::uint16_t len = 0;
    std::vector<std::uint8_t> bytes;
};

struct WriteParsed {
    std::uint8_t  ver = 0;
    std::uint8_t  flags = 0;
    std::uint16_t reserved = 0;
    std::uint16_t handle = 0;
    std::uint32_t offset = 0;
    std::uint16_t written = 0;
};

static bool parse_read_response(const IOResponse& resp, ReadParsed& out)
{
    netproto::Reader r(resp.payload.data(), resp.payload.size());

    if (!r.read_u8(out.ver)) return false;
    if (!r.read_u8(out.flags)) return false;
    if (!r.read_u16le(out.reserved)) return false;
    if (!r.read_u16le(out.handle)) return false;
    if (!r.read_u32le(out.offset)) return false;
    if (!r.read_u16le(out.len)) return false;

    const std::uint8_t* p = nullptr;
    if (!r.read_bytes(p, out.len)) return false;

    out.bytes.assign(p, p + out.len);
    return r.remaining() == 0;
}

static bool parse_write_response(const IOResponse& resp, WriteParsed& out)
{
    netproto::Reader r(resp.payload.data(), resp.payload.size());

    if (!r.read_u8(out.ver)) return false;
    if (!r.read_u8(out.flags)) return false;
    if (!r.read_u16le(out.reserved)) return false;
    if (!r.read_u16le(out.handle)) return false;
    if (!r.read_u32le(out.offset)) return false;
    if (!r.read_u16le(out.written)) return false;
    return r.remaining() == 0;
}

struct InfoParsed {
    std::uint8_t  ver = 0;
    std::uint8_t  flags = 0;
    std::uint16_t reserved = 0;
    std::uint16_t handle = 0;
    std::uint16_t httpStatus = 0;
    std::uint64_t contentLength = 0;
    std::uint32_t hdrLen = 0;
};

static bool parse_info_response(const IOResponse& resp, InfoParsed& out)
{
    netproto::Reader r(resp.payload.data(), resp.payload.size());

    if (!r.read_u8(out.ver)) return false;
    if (!r.read_u8(out.flags)) return false;
    if (!r.read_u16le(out.reserved)) return false;
    if (!r.read_u16le(out.handle)) return false;
    if (!r.read_u16le(out.httpStatus)) return false;
    if (!r.read_u64le(out.contentLength)) return false;
    if (!r.read_u32le(out.hdrLen)) return false;
    return r.remaining() == 0;
}

static bool fetch_info_headers(NetworkDevice& dev, std::uint16_t deviceId, std::uint16_t handle, std::uint32_t hdrLen, std::string& out)
{
    out.clear();
    std::uint32_t offset = 0;
    while (offset < hdrLen) {
        IOResponse rr = info_read_req(dev, deviceId, handle, offset, 128);
        if (rr.status != StatusCode::Ok) return false;

        netproto::Reader r(rr.payload.data(), rr.payload.size());
        std::uint8_t ver = 0, flags = 0;
        std::uint16_t reserved = 0, h = 0, len = 0;
        std::uint32_t off = 0;
        if (!r.read_u8(ver)) return false;
        if (!r.read_u8(flags)) return false;
        if (!r.read_u16le(reserved)) return false;
        if (!r.read_u16le(h)) return false;
        if (!r.read_u32le(off)) return false;
        if (!r.read_u16le(len)) return false;
        if (ver != V || h != handle || off != offset) return false;

        const std::uint8_t* p = nullptr;
        if (!r.read_bytes(p, len)) return false;
        out.append(reinterpret_cast<const char*>(p), reinterpret_cast<const char*>(p + len));
        if (r.remaining() != 0) return false;

        offset += len;
        if ((flags & 0x01) != 0) break;
        if (len == 0) return false;
    }
    return out.size() == hdrLen;
}

template <typename Pred>
static bool spin_poll_until(NetworkDevice& dev, Pred pred, int timeout_ms = 500)
{
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        dev.poll();
        if (pred()) return true;

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (elapsed > timeout_ms) return false;

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ------------------------
// Tests
// ------------------------

TEST_CASE("TCP common: ring read across 64K boundary preserves bytes")
{
    std::vector<std::uint8_t> marker = {
        0x0e, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x11, 0x40, 0x01, 0x00, 0x00
    };

    MemoryTcpSocketOps ops;
    ops.rx.assign(0xffffu, 0x55);
    ops.rx.insert(ops.rx.end(), marker.begin(), marker.end());

    fujinet::net::TcpNetworkProtocolCommon proto(ops);
    fujinet::net::TcpNetworkProtocolCommon::Options opt{};
    opt.rx_buf = 65536;
    REQUIRE(proto.adopt_connected_socket(1, opt, "memory", 1) == StatusCode::Ok);

    std::uint32_t offset = 0;
    std::uint8_t buf[4096];
    while (offset < 0xffffu) {
        const std::size_t want = std::min<std::uint32_t>(sizeof(buf), 0xffffu - offset);
        std::uint16_t read = 0;
        bool eof = false;
        bool more = false;
        REQUIRE(proto.read_body(offset, buf, want, read, eof, more) == StatusCode::Ok);
        REQUIRE(read > 0);
        bool allFiller = true;
        for (std::uint16_t i = 0; i < read; ++i) {
            allFiller = allFiller && buf[i] == 0x55;
        }
        CHECK(allFiller);
        offset += read;
    }

    std::uint8_t got[16] = {};
    std::uint16_t read = 0;
    bool eof = false;
    bool more = false;
    REQUIRE(proto.read_body(offset, got, marker.size(), read, eof, more) == StatusCode::Ok);
    REQUIRE(read == marker.size());
    for (std::size_t i = 0; i < marker.size(); ++i) {
        CAPTURE(i);
        CHECK(got[i] == marker[i]);
    }
}

TEST_CASE("TCP: Open + Write + Read echoes bytes (sequential offsets)")
{
    LocalEchoServer srv;
    REQUIRE(srv.start());

    auto reg = make_registry_tcp_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::string url = "tcp://127.0.0.1:" + std::to_string(srv.port);
    const std::uint16_t handle = open_handle_stub(dev, deviceId, url, /*method=*/1 /*GET*/, /*flags=*/0, /*bodyLenHint=*/0);

    // Allow connect to complete
    REQUIRE(spin_poll_until(dev, [&] {
        auto ir = info_req(dev, deviceId, handle);
        if (ir.status != StatusCode::Ok) return false;
        InfoParsed ip;
        if (!parse_info_response(ir, ip)) return false;
        std::string headers;
        if (!fetch_info_headers(dev, deviceId, handle, ip.hdrLen, headers)) return false;
        return headers.find("X-FujiNet-Connected: 1") != std::string::npos;
    }, 1500));

    // Write "hello"
    const std::string msg = "hello";
    {
        IOResponse w = write_req(dev, deviceId, handle, /*offset=*/0, msg);
        CHECK(w.status == StatusCode::Ok);
    }

    // Read it back
    std::vector<std::uint8_t> got;
    got.reserve(msg.size());

    std::uint32_t off = 0;
    while (got.size() < msg.size()) {
        IOResponse rr = read_req(dev, deviceId, handle, off, /*maxBytes=*/64);

        if (rr.status == StatusCode::NotReady) {
            dev.poll();
            continue;
        }

        REQUIRE(rr.status == StatusCode::Ok);

        ReadParsed rp;
        REQUIRE(parse_read_response(rr, rp));
        CHECK(rp.ver == V);
        CHECK(rp.handle == handle);
        CHECK(rp.offset == off);

        // eof must not be set yet
        CHECK((rp.flags & 0x01) == 0);

        got.insert(got.end(), rp.bytes.begin(), rp.bytes.end());
        off += rp.len;

        dev.poll();
    }

    const std::string gotStr(got.begin(), got.end());
    CHECK(gotStr == msg);

    CHECK(close_req(dev, deviceId, handle).status == StatusCode::Ok);
}

TEST_CASE("TCP: Read returns NotReady when no data buffered yet")
{
    LocalEchoServer srv;
    REQUIRE(srv.start());

    auto reg = make_registry_tcp_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::string url = "tcp://127.0.0.1:" + std::to_string(srv.port);
    const std::uint16_t handle = open_handle_stub(dev, deviceId, url, 1, 0, 0);

    // Immediately attempt a read (no writes yet) => NotReady (stream empty)
    {
        IOResponse rr = read_req(dev, deviceId, handle, /*offset=*/0, /*maxBytes=*/16);
        CHECK(rr.status == StatusCode::NotReady);
    }

    CHECK(close_req(dev, deviceId, handle).status == StatusCode::Ok);
}

TEST_CASE("TCP: Info exposes pseudo-headers without breaking v1 flags")
{
    LocalEchoServer srv;
    REQUIRE(srv.start());

    auto reg = make_registry_tcp_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::string url = "tcp://127.0.0.1:" + std::to_string(srv.port);
    const std::uint16_t handle = open_handle_stub(dev, deviceId, url, 1, 0, 0);

    // poll a bit, then ask for info headers
    dev.poll();

    IOResponse ir = info_req(dev, deviceId, handle);
    REQUIRE(ir.status == StatusCode::Ok);

    InfoParsed ip;
    REQUIRE(parse_info_response(ir, ip));
    CHECK(ip.ver == V);
    CHECK(ip.handle == handle);

    // v1 Info flags: bit0=headersIncluded, bit1=hasContentLength, bit2=hasHttpStatus
    CHECK((ip.flags & 0x01) != 0); // headers included
    CHECK((ip.flags & 0x02) == 0); // no content-length for tcp
    CHECK((ip.flags & 0x04) == 0); // no http status for tcp

    std::string headers;
    REQUIRE(fetch_info_headers(dev, deviceId, handle, ip.hdrLen, headers));
    CHECK(headers.find("X-FujiNet-Scheme: tcp") != std::string::npos);
    CHECK(headers.find("X-FujiNet-RxAvailable:") != std::string::npos);
    CHECK(headers.find("X-FujiNet-LastError:") != std::string::npos);

    CHECK(close_req(dev, deviceId, handle).status == StatusCode::Ok);
}

TEST_CASE("TCP: Write enforces sequential offset (mismatch => InvalidRequest)")
{
    LocalEchoServer srv;
    REQUIRE(srv.start());

    auto reg = make_registry_tcp_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::string url = "tcp://127.0.0.1:" + std::to_string(srv.port);
    const std::uint16_t handle = open_handle_stub(dev, deviceId, url, 1, 0, 0);

    // Wait until the async connect is complete; otherwise NetworkDevice will return NotReady on Write.
    REQUIRE(spin_poll_until(dev, [&] {
        auto ir = info_req(dev, deviceId, handle);
        if (ir.status != StatusCode::Ok) return false;

        InfoParsed ip;
        if (!parse_info_response(ir, ip)) return false;
        std::string headers;
        if (!fetch_info_headers(dev, deviceId, handle, ip.hdrLen, headers)) return false;
        return headers.find("X-FujiNet-Connected: 1") != std::string::npos;
    }, 1500));

    // First write at offset 0 should succeed (or transiently DeviceBusy).
    {
        IOResponse w = write_req(dev, deviceId, handle, 0, "abc");

        // If you ever see DeviceBusy here (rare locally), just poll once and retry.
        if (w.status == StatusCode::DeviceBusy) {
            dev.poll();
            w = write_req(dev, deviceId, handle, 0, "abc");
        }

        CHECK(w.status == StatusCode::Ok);
    }

    // Now try to write again at offset 0 (should be 3) => InvalidRequest.
    // We still tolerate transient DeviceBusy, but offset mismatch must not become Ok.
    {
        IOResponse w = write_req(dev, deviceId, handle, 0, "x");

        if (w.status == StatusCode::DeviceBusy) {
            dev.poll();
            w = write_req(dev, deviceId, handle, 0, "x");
        }

        CHECK(w.status == StatusCode::InvalidRequest);
    }

    CHECK(close_req(dev, deviceId, handle).status == StatusCode::Ok);
}

TEST_CASE("TCP: Read preserves bytes across 64K stream and ring boundary")
{
    LocalEchoServer srv;
    REQUIRE(srv.start());

    auto reg = make_registry_tcp_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::string url = "tcp://127.0.0.1:" + std::to_string(srv.port) + "?rx_buf=65536";
    const std::uint16_t handle = open_handle_stub(dev, deviceId, url, 1, 0, 0);

    REQUIRE(spin_poll_until(dev, [&] {
        auto ir = info_req(dev, deviceId, handle);
        if (ir.status != StatusCode::Ok) return false;

        InfoParsed ip;
        if (!parse_info_response(ir, ip)) return false;
        std::string headers;
        if (!fetch_info_headers(dev, deviceId, handle, ip.hdrLen, headers)) return false;
        return headers.find("X-FujiNet-Connected: 1") != std::string::npos;
    }, 1500));

    std::string marker;
    marker.push_back('\x0e');
    marker.push_back('\x00');
    marker.push_back('\x20');
    marker.push_back('\x00');
    marker.push_back('\x00');
    marker.push_back('\x00');
    marker.push_back('\x00');
    marker.push_back('\x00');
    marker.push_back('\x00');
    marker.push_back('\x11');
    marker.push_back('\x40');
    marker.push_back('\x01');
    marker.push_back('\x00');
    marker.push_back('\x00');

    std::uint32_t writeOff = 0;
    std::string chunk(4096, '\x55');
    while (writeOff < 0xffffu) {
        const std::uint32_t remain = 0xffffu - writeOff;
        const std::size_t n = std::min<std::size_t>(chunk.size(), remain);
        std::size_t chunkOff = 0;
        while (chunkOff < n) {
            IOResponse w = write_req(dev,
                                     deviceId,
                                     handle,
                                     writeOff,
                                     std::string_view(chunk.data() + chunkOff, n - chunkOff));
            if (w.status == StatusCode::DeviceBusy) {
                dev.poll();
                continue;
            }
            REQUIRE(w.status == StatusCode::Ok);
            WriteParsed wp;
            REQUIRE(parse_write_response(w, wp));
            REQUIRE(wp.written > 0);
            CHECK(wp.offset == writeOff);
            chunkOff += wp.written;
            writeOff += wp.written;
            dev.poll();
        }
    }

    std::size_t markerOff = 0;
    while (markerOff < marker.size()) {
        IOResponse w = write_req(dev,
                                 deviceId,
                                 handle,
                                 writeOff,
                                 std::string_view(marker.data() + markerOff, marker.size() - markerOff));
        if (w.status == StatusCode::DeviceBusy) {
            dev.poll();
            continue;
        }
        REQUIRE(w.status == StatusCode::Ok);
        WriteParsed wp;
        REQUIRE(parse_write_response(w, wp));
        REQUIRE(wp.written > 0);
        CHECK(wp.offset == writeOff);
        markerOff += wp.written;
        writeOff += wp.written;
        dev.poll();
    }

    std::uint32_t readOff = 0;
    while (readOff < 0xffffu) {
        const std::uint16_t want = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(4096u, 0xffffu - readOff));
        IOResponse rr = read_req(dev, deviceId, handle, readOff, want);

        if (rr.status == StatusCode::NotReady) {
            dev.poll();
            continue;
        }

        REQUIRE(rr.status == StatusCode::Ok);
        ReadParsed rp;
        REQUIRE(parse_read_response(rr, rp));
        REQUIRE(rp.len > 0);
        CHECK(rp.offset == readOff);
        CHECK(rp.bytes.size() == rp.len);
        bool allFiller = true;
        for (std::uint8_t b : rp.bytes) {
            allFiller = allFiller && b == 0x55;
        }
        CHECK(allFiller);
        readOff += rp.len;
        dev.poll();
    }

    ReadParsed rp;
    REQUIRE(spin_poll_until(dev, [&] {
        IOResponse rr = read_req(dev, deviceId, handle, readOff, static_cast<std::uint16_t>(marker.size()));
        if (rr.status == StatusCode::NotReady) {
            return false;
        }
        REQUIRE(rr.status == StatusCode::Ok);
        REQUIRE(parse_read_response(rr, rp));
        return rp.len == marker.size();
    }, 1500));

    CHECK(rp.offset == 0xffffu);
    CHECK(rp.bytes == std::vector<std::uint8_t>(marker.begin(), marker.end()));
    REQUIRE(rp.bytes.size() == marker.size());
    for (std::size_t i = 0; i < marker.size(); ++i) {
        CAPTURE(i);
        CHECK(rp.bytes[i] == static_cast<std::uint8_t>(marker[i]));
    }

    CHECK(close_req(dev, deviceId, handle).status == StatusCode::Ok);
}

} // namespace fujinet::tests

#else

TEST_CASE("TCP tests are skipped on Windows builds")
{
    CHECK(true);
}

#endif
