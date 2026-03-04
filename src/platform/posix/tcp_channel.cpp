#include "fujinet/platform/posix/tcp_channel.h"
#include "fujinet/core/logging.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <memory>

namespace fujinet::platform {

static constexpr const char* TAG = "tcp";

static void format_hex_prefix(const std::uint8_t* buffer, std::size_t len, char* out, std::size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }

    std::size_t pos = 0;
    out[0] = '\0';

    const std::size_t n = (len < 16) ? len : 16;
    for (std::size_t i = 0; i < n; ++i) {
        int wrote = std::snprintf(out + pos, out_len - pos, "%02X%s", buffer[i], (i + 1 == n) ? "" : " ");
        if (wrote <= 0) {
            break;
        }
        pos += static_cast<std::size_t>(wrote);
        if (pos >= out_len) {
            out[out_len - 1] = '\0';
            break;
        }
    }
}

class TcpChannel final : public fujinet::io::Channel {
 public:
  explicit TcpChannel(const std::string& host, uint16_t port) : host_(host), port_(port), socket_fd_(-1), connected_(false) {
    socket_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd_ < 0) {
      FN_LOGE(TAG, "Failed to create TCP socket: %s", std::strerror(errno));
      return;
    }

    // Set socket to non-blocking
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (flags < 0) {
      FN_LOGE(TAG, "Failed to get socket flags: %s", std::strerror(errno));
      close(socket_fd_);
      socket_fd_ = -1;
      return;
    }
    if (fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
      FN_LOGE(TAG, "Failed to set socket to non-blocking: %s", std::strerror(errno));
      close(socket_fd_);
      socket_fd_ = -1;
      return;
    }

    // Resolve hostname
    struct hostent* he = gethostbyname(host_.c_str());
    if (!he) {
      FN_LOGE(TAG, "Failed to resolve hostname: %s", host_.c_str());
      close(socket_fd_);
      socket_fd_ = -1;
      return;
    }

    memset(&server_addr_, 0, sizeof(server_addr_));
    server_addr_.sin_family = AF_INET;
    server_addr_.sin_port = htons(port_);
    std::memcpy(&server_addr_.sin_addr, he->h_addr_list[0], he->h_length);

    // Connect to the server
    if (connect(socket_fd_, reinterpret_cast<const sockaddr*>(&server_addr_), sizeof(server_addr_)) < 0) {
      if (errno != EINPROGRESS) {
        FN_LOGE(TAG, "Failed to connect TCP socket: %s", std::strerror(errno));
        close(socket_fd_);
        socket_fd_ = -1;
        return;
      }

      // Wait for connection to complete
      fd_set write_fds;
      FD_ZERO(&write_fds);
      FD_SET(socket_fd_, &write_fds);

      struct timeval tv;
      tv.tv_sec = 5;
      tv.tv_usec = 0;

      int ret = select(socket_fd_ + 1, nullptr, &write_fds, nullptr, &tv);
      if (ret <= 0) {
        FN_LOGE(TAG, "Connection timeout or error: %d", errno);
        close(socket_fd_);
        socket_fd_ = -1;
        return;
      }

      // Check if connection was successful
      int sock_error;
      socklen_t len = sizeof(sock_error);
      if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &sock_error, &len) < 0 || sock_error != 0) {
        FN_LOGE(TAG, "Connection failed: %s", std::strerror(sock_error));
        close(socket_fd_);
        socket_fd_ = -1;
        return;
      }
    }

    connected_ = true;
    FN_LOGI(TAG, "Connected to %s:%u", host_.c_str(), static_cast<unsigned>(port_));
  }

  ~TcpChannel() override {
    if (socket_fd_ >= 0) {
      close(socket_fd_);
    }
  }

  bool available() override {
    if (!connected_) {
      return false;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd_, &read_fds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    int ret = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &tv);
    return ret > 0;
  }

  std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override {
    if (!connected_ || socket_fd_ < 0) {
      return 0;
    }

    ssize_t bytes_read = recv(socket_fd_, buffer, maxLen, 0);
    if (bytes_read < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        FN_LOGE(TAG, "Read failed: %s", std::strerror(errno));
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
    format_hex_prefix(buffer, bytes_read, hex_prefix, sizeof(hex_prefix));
    FN_LOGD(TAG, "Read %d bytes: %s", static_cast<int>(bytes_read), hex_prefix);

    return static_cast<std::size_t>(bytes_read);
  }

  void write(const std::uint8_t* buffer, std::size_t len) override {
    if (!connected_ || socket_fd_ < 0) {
      return;
    }

    ssize_t bytes_written = send(socket_fd_, buffer, len, 0);
    if (bytes_written < 0) {
      FN_LOGE(TAG, "Write failed: %s", std::strerror(errno));
      connected_ = false;
      return;
    }

    char hex_prefix[64];
    format_hex_prefix(buffer, bytes_written, hex_prefix, sizeof(hex_prefix));
    FN_LOGD(TAG, "Wrote %d bytes: %s", static_cast<int>(bytes_written), hex_prefix);
  }

 private:
  std::string host_;
  uint16_t port_;
  int socket_fd_;
  bool connected_;
  struct sockaddr_in server_addr_;
};

std::unique_ptr<fujinet::io::Channel> create_tcp_channel(const std::string& host, uint16_t port) {
  return std::make_unique<TcpChannel>(host, port);
}

}  // namespace fujinet::platform
