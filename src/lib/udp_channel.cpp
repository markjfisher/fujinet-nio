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
    const void* hints = socket_ops_.udp_addrinfo_hints();
    AddrInfo* res = nullptr;
    const std::string port_str = std::to_string(port_);
    const int gai = socket_ops_.getaddrinfo(host_.c_str(), port_str.c_str(), hints, &res);
    if (gai != 0 || !res) {
        FN_LOGE(TAG, "Failed to resolve hostname: %s", host_.c_str());
        return;
    }

    for (AddrInfo* ai = res; ai; ai = socket_ops_.addrinfo_next(ai)) {
        socket_fd_ = socket_ops_.socket(socket_ops_.addrinfo_family(ai),
                                       socket_ops_.addrinfo_socktype(ai),
                                       socket_ops_.addrinfo_protocol(ai));
        if (socket_fd_ < 0) {
            continue;
        }

        if (socket_ops_.set_nonblocking(socket_fd_) < 0) {
            socket_ops_.close(socket_fd_);
            socket_fd_ = -1;
            continue;
        }

        SockLen addrlen = 0;
        const struct sockaddr* addr = socket_ops_.addrinfo_addr(ai, &addrlen);
        if (socket_ops_.connect(socket_fd_, addr, addrlen) == 0) {
            connected_ = true;
            break;
        }

        socket_ops_.close(socket_fd_);
        socket_fd_ = -1;
    }

    socket_ops_.freeaddrinfo(res);

    if (connected_) {
        FN_LOGI(TAG, "Connected to %s:%u", host_.c_str(), static_cast<unsigned>(port_));
    } else {
        FN_LOGE(TAG, "Failed to connect UDP socket: %s", socket_ops_.err_string(socket_ops_.last_errno()));
    }
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

    const SSize bytes_read = socket_ops_.recv(socket_fd_, buffer, max_len);
    if (bytes_read < 0) {
        const int err = socket_ops_.last_errno();
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

    const SSize bytes_sent = socket_ops_.send(socket_fd_, buffer, len);
    if (bytes_sent < 0) {
        FN_LOGE(TAG, "Write failed: %s", socket_ops_.err_string(socket_ops_.last_errno()));
        connected_ = false;
        return;
    }
}

} // namespace fujinet::net
