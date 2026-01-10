#include "doctest.h"

#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/byte_codec.h"
#include "fujinet/io/devices/modem_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"

// POSIX TCP ops (for unit tests)
#include "fujinet/platform/posix/tcp_socket_ops_posix.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fujinet::tests {

using fujinet::io::IORequest;
using fujinet::io::IOResponse;
using fujinet::io::ModemDevice;
using fujinet::io::StatusCode;
using fujinet::io::bytecodec::Reader;
using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;

static constexpr std::uint8_t V = 1;

static std::vector<std::uint8_t> to_vec(const std::string& s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

// ------------------------
// Minimal local echo server
// ------------------------
struct LocalEchoServer {
    int listen_fd = -1;
    std::uint16_t port = 0;

    std::thread th;
    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};

    LocalEchoServer() = default;
    ~LocalEchoServer() { shutdown(); }

    bool start()
    {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) return false;

        int opt = 1;
        (void)::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);

        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_fd);
            listen_fd = -1;
            return false;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ::close(listen_fd);
            listen_fd = -1;
            return false;
        }

        port = ntohs(addr.sin_port);

        if (::listen(listen_fd, 1) != 0) {
            ::close(listen_fd);
            listen_fd = -1;
            return false;
        }

        ready.store(true);

        th = std::thread([this]() {
            sockaddr_in caddr {};
            socklen_t clen = sizeof(caddr);
            int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&caddr), &clen);
            if (cfd < 0) return;

            std::uint8_t buf[4096];
            while (!stop.load()) {
                const ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
                if (n > 0) {
                    std::size_t off = 0;
                    while (off < static_cast<std::size_t>(n)) {
                        const ssize_t w = ::send(cfd, buf + off,
                                                 static_cast<std::size_t>(n) - off,
                                                 0);
                        if (w > 0) off += static_cast<std::size_t>(w);
                        else break;
                    }
                    continue;
                }
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
        if (th.joinable()) th.join();
    }
};

static std::uint16_t pick_free_port()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);

    const std::uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

static IOResponse modem_write(ModemDevice& dev, std::uint16_t deviceId, std::uint32_t offset, std::string_view bytes)
{
    std::string p;
    fujinet::io::bytecodec::write_u8(p, V);
    fujinet::io::bytecodec::write_u32le(p, offset);
    fujinet::io::bytecodec::write_u16le(p, static_cast<std::uint16_t>(bytes.size()));
    p.append(bytes.data(), bytes.size());

    IORequest req{};
    req.id = 100;
    req.deviceId = deviceId;
    req.command = 0x01; // Write
    req.payload = to_vec(p);

    return dev.handle(req);
}

static std::string modem_read_available(ModemDevice& dev, std::uint16_t deviceId, std::uint32_t offset, std::uint16_t maxBytes = 256)
{
    std::string p;
    fujinet::io::bytecodec::write_u8(p, V);
    fujinet::io::bytecodec::write_u32le(p, offset);
    fujinet::io::bytecodec::write_u16le(p, maxBytes);

    IORequest req{};
    req.id = 200;
    req.deviceId = deviceId;
    req.command = 0x02; // Read
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    if (resp.status != StatusCode::Ok) return {};

    Reader r(resp.payload.data(), resp.payload.size());
    std::uint8_t ver = 0, flags = 0;
    std::uint16_t reserved = 0;
    std::uint32_t roff = 0;
    std::uint16_t len = 0;
    if (!r.read_u8(ver) || !r.read_u8(flags) || !r.read_u16le(reserved) || !r.read_u32le(roff) || !r.read_u16le(len)) {
        return {};
    }

    const std::uint8_t* ptr = nullptr;
    if (!r.read_bytes(ptr, len)) return {};

    return std::string(reinterpret_cast<const char*>(ptr), reinterpret_cast<const char*>(ptr + len));
}

template <typename Pred>
static bool spin_poll_until(ModemDevice& dev, Pred pred, int timeout_ms = 1500)
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

TEST_CASE("ModemDevice: ATDT connects and stream echoes bytes")
{
    LocalEchoServer srv;
    REQUIRE(srv.start());

    auto& ops = fujinet::net::get_posix_socket_ops();
    ModemDevice dev(ops);

    const auto deviceId = to_device_id(WireDeviceId::ModemService);
    std::uint32_t woff = 0;
    std::uint32_t roff = 0;

    // Dial local echo server
    {
        const std::string cmd = "ATDT127.0.0.1:" + std::to_string(srv.port) + "\r";
        IOResponse wr = modem_write(dev, deviceId, woff, cmd);
        REQUIRE(wr.status == StatusCode::Ok);
        woff += static_cast<std::uint32_t>(cmd.size());
    }

    std::string out;
    REQUIRE(spin_poll_until(dev, [&] {
        const std::string chunk = modem_read_available(dev, deviceId, roff, 256);
        roff += static_cast<std::uint32_t>(chunk.size());
        out += chunk;
        return out.find("CONNECT") != std::string::npos;
    }, 2000));

    // Send "hello" and expect to read it back.
    {
        IOResponse wr = modem_write(dev, deviceId, woff, "hello");
        REQUIRE(wr.status == StatusCode::Ok);
        woff += 5;
    }

    // Poll until the echo arrives.
    std::string got;
    REQUIRE(spin_poll_until(dev, [&] {
        const std::string chunk = modem_read_available(dev, deviceId, roff, 256);
        roff += static_cast<std::uint32_t>(chunk.size());
        got += chunk;
        return got.find("hello") != std::string::npos;
    }, 2000));
}

TEST_CASE("ModemDevice: listen emits RING and ATA answers")
{
    const std::uint16_t port = pick_free_port();

    auto& ops = fujinet::net::get_posix_socket_ops();
    ModemDevice dev(ops);
    const auto deviceId = to_device_id(WireDeviceId::ModemService);

    std::uint32_t woff = 0;
    std::uint32_t roff = 0;

    // Start listening.
    {
        const std::string cmd = "ATPORT" + std::to_string(port) + "\r";
        IOResponse wr = modem_write(dev, deviceId, woff, cmd);
        REQUIRE(wr.status == StatusCode::Ok);
        woff += static_cast<std::uint32_t>(cmd.size());
    }

    // Connect a client.
    int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(cfd >= 0);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    REQUIRE(::connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // Expect RING.
    std::string out;
    REQUIRE(spin_poll_until(dev, [&] {
        const std::string chunk = modem_read_available(dev, deviceId, roff, 256);
        roff += static_cast<std::uint32_t>(chunk.size());
        out += chunk;
        return out.find("RING") != std::string::npos;
    }, 2500));

    // Answer.
    {
        IOResponse wr = modem_write(dev, deviceId, woff, "ATA\r");
        REQUIRE(wr.status == StatusCode::Ok);
        woff += 4;
    }

    // Expect CONNECT.
    REQUIRE(spin_poll_until(dev, [&] {
        const std::string chunk = modem_read_available(dev, deviceId, roff, 256);
        roff += static_cast<std::uint32_t>(chunk.size());
        out += chunk;
        return out.find("CONNECT") != std::string::npos;
    }, 2500));

    // Send bytes from client -> modem -> host.
    REQUIRE(::send(cfd, "z", 1, 0) == 1);

    std::string rx;
    REQUIRE(spin_poll_until(dev, [&] {
        const std::string chunk = modem_read_available(dev, deviceId, roff, 256);
        roff += static_cast<std::uint32_t>(chunk.size());
        rx += chunk;
        return rx.find('z') != std::string::npos;
    }, 2500));

    ::close(cfd);
}

} // namespace fujinet::tests

#else

TEST_CASE("ModemDevice tests are skipped on Windows builds")
{
    CHECK(true);
}

#endif


