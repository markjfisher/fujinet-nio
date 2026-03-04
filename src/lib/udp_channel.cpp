#include "fujinet/net/udp_channel.h"
#include "fujinet/core/logging.h"
#include "fujinet/core/utils.h"

#include <cstring>

namespace fujinet::net {

static constexpr const char* TAG = "udp";

UdpChannel::UdpChannel(IUdpSocketOps& socket_ops, const std::string& host, uint16_t port)
    : socket_ops_(socket_ops)
    , host_(host)
    , port_(port)
    , socket_fd_(-1)
    , connected_(false)
{
    FN_LOGD(TAG, "UdpChannel constructor called with host: %s, port: %u", host.c_str(), static_cast<unsigned>(port));
    
    const void* hints = socket_ops_.udp_addrinfo_hints();
    AddrInfo* res = nullptr;
    const std::string port_str = std::to_string(port_);
    const int gai = socket_ops_.getaddrinfo(host_.c_str(), port_str.c_str(), hints, &res);
    if (gai != 0 || !res) {
        FN_LOGE(TAG, "Failed to resolve hostname: %s", host_.c_str());
        return;
    }

    // Prefer IPv4 first for localhost-style endpoints because many TNFS daemons
    // are bound only on IPv4. Fallback to the first usable address.
    AddrInfo* chosen = nullptr;
    for (AddrInfo* ai = res; ai; ai = socket_ops_.addrinfo_next(ai)) {
        if (socket_ops_.addrinfo_family(ai) == AF_INET) {
            chosen = ai;
            break;
        }
    }
    if (!chosen) {
        chosen = res;
    }

    sockaddr_storage temp_addr{};
    SockLen addrlen = 0;
    const struct sockaddr* addr = socket_ops_.addrinfo_addr(chosen, &addrlen);
    if (!addr || addrlen > sizeof(temp_addr)) {
        FN_LOGE(TAG, "Failed to get UDP peer address for %s", host_.c_str());
        socket_ops_.freeaddrinfo(res);
        return;
    }
    std::memcpy(&peer_addr_, addr, addrlen);
    peer_addr_len_ = addrlen;

    socket_fd_ = socket_ops_.socket(socket_ops_.addrinfo_family(chosen),
                                   socket_ops_.addrinfo_socktype(chosen),
                                   socket_ops_.addrinfo_protocol(chosen));
    if (socket_fd_ < 0) {
        FN_LOGE(TAG, "Failed to create socket: %s", socket_ops_.err_string(socket_ops_.last_errno()));
        socket_ops_.freeaddrinfo(res);
        return;
    }

    if (socket_ops_.set_nonblocking(socket_fd_) < 0) {
        FN_LOGE(TAG, "Failed to set socket non-blocking: %s", socket_ops_.err_string(socket_ops_.last_errno()));
        socket_ops_.close(socket_fd_);
        socket_fd_ = -1;
        socket_ops_.freeaddrinfo(res);
        return;
    }

    socket_ops_.freeaddrinfo(res);
    connected_ = true;
    FN_LOGI(TAG, "UDP channel created for %s:%u", host_.c_str(), static_cast<unsigned>(port_));
}

UdpChannel::~UdpChannel()
{
    if (socket_fd_ >= 0) {
        socket_ops_.close(socket_fd_);
    }
}

bool UdpChannel::available()
{
    if (!connected_ || socket_fd_ < 0) {
        return false;
    }

    return socket_ops_.poll_readable(socket_fd_);
}

std::size_t UdpChannel::read(std::uint8_t* buffer, std::size_t max_len)
{
    if (!connected_ || socket_fd_ < 0 || !buffer) {
        return 0;
    }

    sockaddr_storage src_addr{};
    socklen_t src_addr_len = sizeof(src_addr);
    const SSize bytes_read = socket_ops_.recvfrom(socket_fd_, buffer, max_len, reinterpret_cast<struct sockaddr*>(&src_addr), &src_addr_len);
    if (bytes_read < 0) {
        const int err = socket_ops_.last_errno();
        if (err == ECONNREFUSED) {
            // For UDP sockets, ECONNREFUSED means the packet was rejected, but we can continue
            FN_LOGD(TAG, "Read failed (ECONNREFUSED) - will retry");
            return 0;
        }
        if (!socket_ops_.is_would_block(err)) {
            FN_LOGE(TAG, "Read failed: %s", socket_ops_.err_string(err));
            connected_ = false;
        }
        return 0;
    }

    char hex_prefix[64];
    core::format_hex_prefix(buffer, static_cast<std::size_t>(bytes_read), hex_prefix, sizeof(hex_prefix));
    if (bytes_read > 16) {
        FN_LOGD(TAG, "Received %zd bytes: %s ...", bytes_read, hex_prefix);
    } else {
        FN_LOGD(TAG, "Received %zd bytes: %s", bytes_read, hex_prefix);
    }

    return static_cast<std::size_t>(bytes_read);
}

void UdpChannel::write(const std::uint8_t* buffer, std::size_t len)
{
    if (!connected_ || socket_fd_ < 0 || !buffer) {
        return;
    }

    char hex_prefix[64];
    core::format_hex_prefix(buffer, len, hex_prefix, sizeof(hex_prefix));
    if (len > 16) {
        FN_LOGD(TAG, "Sending %zu bytes: %s ...", len, hex_prefix);
    } else {
        FN_LOGD(TAG, "Sending %zu bytes: %s", len, hex_prefix);
    }

    const SSize bytes_sent = socket_ops_.sendto(socket_fd_, buffer, len, reinterpret_cast<const struct sockaddr*>(&peer_addr_), peer_addr_len_);
    if (bytes_sent < 0) {
        FN_LOGE(TAG, "Write failed: %s", socket_ops_.err_string(socket_ops_.last_errno()));
        connected_ = false;
        return;
    }
}

} // namespace fujinet::net
