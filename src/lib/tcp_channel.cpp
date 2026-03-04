#include "fujinet/net/tcp_channel.h"
#include "fujinet/core/logging.h"
#include "fujinet/core/utils.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace fujinet::net {

static constexpr const char* TAG = "tcp";

TcpChannel::TcpChannel(ITcpSocketOps& socket_ops, const std::string& host, uint16_t port)
    : socket_ops_(socket_ops)
    , host_(host)
    , port_(port)
    , socket_fd_(-1)
    , connected_(false)
{
    int last_connect_error = socket_ops_.err_timed_out();

    const void* hints = socket_ops_.tcp_stream_addrinfo_hints();
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
        const int connect_result = socket_ops_.connect(socket_fd_, addr, addrlen);
        
        if (connect_result == 0) {
            connected_ = true;
            break;
        }
        
        const int connect_err = socket_ops_.last_errno();
        last_connect_error = connect_err;
        if (connect_result < 0 && socket_ops_.is_in_progress(connect_err)) {
            // Wait for connection to complete
            bool connect_complete = false;
            for (int i = 0; i < 50; ++i) {
                if (socket_ops_.poll_connect_complete(socket_fd_)) {
                    connect_complete = true;
                    break;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            if (connect_complete) {
                const int so_error = socket_ops_.get_so_error(socket_fd_);
                last_connect_error = so_error;
                if (so_error == 0) {
                    connected_ = true;
                    break;
                }
            }
        }

        socket_ops_.close(socket_fd_);
        socket_fd_ = -1;
    }

    socket_ops_.freeaddrinfo(res);

    if (connected_) {
        socket_ops_.apply_stream_socket_options(socket_fd_, true, false);
        FN_LOGI(TAG, "Connected to %s:%u", host_.c_str(), static_cast<unsigned>(port_));
    } else {
        FN_LOGE(TAG, "Failed to connect TCP socket: %s", socket_ops_.err_string(last_connect_error));
    }
}

TcpChannel::~TcpChannel()
{
    if (socket_fd_ >= 0) {
        socket_ops_.close(socket_fd_);
    }
}

bool TcpChannel::available()
{
    if (!connected_ || socket_fd_ < 0) {
        return false;
    }

    // We need to implement this using socket_ops to maintain platform independence
    // For now, we'll use a simple implementation - in the future, we could add a
    // poll_readable method to ITcpSocketOps
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd_, &read_fds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    int ret = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &tv);
    return ret > 0;
}

std::size_t TcpChannel::read(std::uint8_t* buffer, std::size_t max_len)
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

    if (bytes_read == 0) {
        FN_LOGE(TAG, "Connection closed by peer");
        connected_ = false;
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

void TcpChannel::write(const std::uint8_t* buffer, std::size_t len)
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
