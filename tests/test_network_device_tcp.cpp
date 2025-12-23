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

struct InfoParsed {
    std::uint8_t  ver = 0;
    std::uint8_t  flags = 0;
    std::uint16_t reserved = 0;
    std::uint16_t handle = 0;
    std::uint16_t httpStatus = 0;
    std::uint64_t contentLength = 0;
    std::uint16_t hdrLen = 0;
    std::string   headers;
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
    if (!r.read_u16le(out.hdrLen)) return false;

    const std::uint8_t* p = nullptr;
    if (!r.read_bytes(p, out.hdrLen)) return false;

    out.headers.assign(reinterpret_cast<const char*>(p), reinterpret_cast<const char*>(p + out.hdrLen));
    return r.remaining() == 0;
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
        return ip.headers.find("X-FujiNet-Connected: 1") != std::string::npos;
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

    CHECK(ip.headers.find("X-FujiNet-Scheme: tcp") != std::string::npos);
    CHECK(ip.headers.find("X-FujiNet-RxAvailable:") != std::string::npos);
    CHECK(ip.headers.find("X-FujiNet-LastError:") != std::string::npos);

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

        return ip.headers.find("X-FujiNet-Connected: 1") != std::string::npos;
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

} // namespace fujinet::tests

#else

TEST_CASE("TCP tests are skipped on Windows builds")
{
    CHECK(true);
}

#endif
