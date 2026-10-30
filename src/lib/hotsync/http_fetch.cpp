// HotSync builds only (FN_WITH_HOTSYNC): the ESP32 build compiles every
// source, so elsewhere this file compiles to nothing.
#if FN_WITH_HOTSYNC == 1

#include "fujinet/hotsync/http_fetch.h"

#include "fujinet/hotsync/tcp_link.h"
#include "fujinet/net/tcp_socket_ops.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace fujinet::hotsync {

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(5);
// Status line and headers; a server sending more is not the one we want.
constexpr std::size_t kMaxHeaderBytes = 8192;

// A connected non-blocking socket, or -1 with err set.
int connect_to(net::ITcpSocketOps& ops, const HttpUrl& url, std::uint32_t timeoutMs, std::string& err)
{
    net::AddrInfo* res = nullptr;
    if (ops.getaddrinfo(url.host.c_str(), url.port.c_str(), ops.tcp_stream_addrinfo_hints(), &res) != 0 || !res) {
        if (res) {
            ops.freeaddrinfo(res);
        }
        err = "cannot resolve " + url.host;
        return -1;
    }

    int fd = -1;
    err = "cannot connect to " + url.host + ":" + url.port;
    for (auto* ai = res; ai && fd < 0; ai = ops.addrinfo_next(ai)) {
        fd = ops.socket(ops.addrinfo_family(ai), ops.addrinfo_socktype(ai), ops.addrinfo_protocol(ai));
        if (fd < 0) {
            continue;
        }
        (void)ops.set_nonblocking(fd);
        net::SockLen addrlen = 0;
        const struct sockaddr* addr = ops.addrinfo_addr(ai, &addrlen);
        bool connected = ops.connect(fd, addr, addrlen) == 0;
        if (!connected && (ops.is_in_progress(ops.last_errno()) || ops.is_would_block(ops.last_errno()))) {
            const std::uint64_t deadline = ops.now_ms() + timeoutMs;
            while (!connected && ops.now_ms() < deadline) {
                if (ops.poll_connect_complete(fd)) {
                    connected = ops.get_so_error(fd) == 0;
                    break;
                }
                const int e = ops.last_errno();
                if (e != 0 && !ops.is_in_progress(e) && !ops.is_would_block(e)) {
                    break;
                }
                std::this_thread::sleep_for(kPollInterval);
            }
        }
        if (!connected) {
            ops.close(fd);
            fd = -1;
        }
    }
    ops.freeaddrinfo(res);
    if (fd >= 0) {
        ops.apply_stream_socket_options(fd, /*nodelay=*/true, /*keepalive=*/false);
        err.clear();
    }
    return fd;
}

} // namespace

bool parse_http_url(const std::string& url, HttpUrl& out)
{
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0) {
        return false;
    }
    const std::size_t hostStart = scheme.size();
    const std::size_t pathStart = url.find('/', hostStart);
    const std::string authority = url.substr(hostStart, pathStart - hostStart);
    if (authority.empty() || authority.find('@') != std::string::npos) {
        return false;
    }
    HttpUrl parsed;
    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        parsed.host = authority;
    } else {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
        if (parsed.port.empty() ||
            !std::all_of(parsed.port.begin(), parsed.port.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
            std::atoi(parsed.port.c_str()) < 1 || std::atoi(parsed.port.c_str()) > 65535) {
            return false;
        }
    }
    if (parsed.host.empty()) {
        return false;
    }
    if (pathStart != std::string::npos) {
        parsed.path = url.substr(pathStart);
    }
    out = parsed;
    return true;
}

std::string http_fetch(net::ITcpSocketOps& ops, const HttpUrl& url, std::size_t maxBytes, Bytes& body,
                       std::uint32_t timeoutMs)
{
    body.clear();
    std::string err;
    const int fd = connect_to(ops, url, timeoutMs, err);
    if (fd < 0) {
        return err;
    }
    TcpLink link(ops, fd);

    const std::string request = "GET " + url.path + " HTTP/1.0\r\nHost: " + url.host +
                                "\r\nUser-Agent: FujiNet-NIO\r\nConnection: close\r\n\r\n";
    if (link.write(reinterpret_cast<const std::uint8_t*>(request.data()), request.size()) !=
        static_cast<int>(request.size())) {
        return "cannot send the request";
    }

    // HTTP/1.0 with Connection: close, so the body runs to the end of the
    // connection. A Content-Length, when sent, must match.
    Bytes reply;
    std::size_t bodyStart = std::string::npos;
    std::uint8_t chunk[1024];
    for (;;) {
        const int got = link.read(chunk, sizeof(chunk), timeoutMs);
        if (got < 0) {
            break; // closed: the reply is complete
        }
        if (got == 0) {
            return "timed out waiting for " + url.host;
        }
        reply.insert(reply.end(), chunk, chunk + got);
        if (bodyStart == std::string::npos) {
            static const std::uint8_t kEnd[] = {'\r', '\n', '\r', '\n'};
            const auto end = std::search(reply.begin(), reply.end(), std::begin(kEnd), std::end(kEnd));
            if (end != reply.end()) {
                bodyStart = static_cast<std::size_t>(end - reply.begin()) + sizeof(kEnd);
            } else if (reply.size() > kMaxHeaderBytes) {
                return "reply headers too long";
            }
        }
        if (bodyStart != std::string::npos && reply.size() - bodyStart > maxBytes) {
            return "file larger than " + std::to_string(maxBytes) + " bytes";
        }
    }
    if (bodyStart == std::string::npos) {
        return "no HTTP reply";
    }

    const std::string head(reply.begin(), reply.begin() + static_cast<std::ptrdiff_t>(bodyStart));
    const std::size_t space = head.find(' ');
    const int status = space == std::string::npos ? 0 : std::atoi(head.c_str() + space + 1);
    if (head.compare(0, 5, "HTTP/") != 0 || status == 0) {
        return "no HTTP reply";
    }
    if (status != 200) {
        return "HTTP " + std::to_string(status);
    }
    body.assign(reply.begin() + static_cast<std::ptrdiff_t>(bodyStart), reply.end());

    std::string lower(head);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::size_t length = lower.find("\r\ncontent-length:");
    if (length != std::string::npos &&
        std::strtoul(head.c_str() + length + 17, nullptr, 10) != body.size()) {
        body.clear();
        return "reply cut short";
    }
    return {};
}

} // namespace fujinet::hotsync

#endif // FN_WITH_HOTSYNC
