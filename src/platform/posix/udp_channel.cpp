
#include "fujinet/platform/posix/udp_channel.h"
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

static constexpr const char* TAG = "udp";

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

class UdpChannel final : public fujinet::io::Channel {
 public:
  explicit UdpChannel(const std::string& host, uint16_t port) : host_(host), port_(port), socket_fd_(-1), connected_(false) {
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
      FN_LOGE(TAG, "Failed to create UDP socket: %s", std::strerror(errno));
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

    // Connect to the server so we can use send/recv instead of sendto/recvfrom
    if (connect(socket_fd_, reinterpret_cast<const sockaddr*>(&server_addr_), sizeof(server_addr_)) < 0) {
      FN_LOGE(TAG, "Failed to connect UDP socket: %s", std::strerror(errno));
      close(socket_fd_);
      socket_fd_ = -1;
      return;
    }

    connected_ = true;
    FN_LOGI(TAG, "Connected to %s:%u", host_.c_str(), static_cast<unsigned>(port_));
  }

  ~UdpChannel() override {
    if (socket_fd_ >= 0) {
      close(socket_fd_);
    }
  }

  bool available() override {
    if (socket_fd_ < 0 || !connected_) {
      return false;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd_, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    int result = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
    if (result < 0) {
      FN_LOGE(TAG, "Select failed: %s", std::strerror(errno));
      return false;
    }

    return FD_ISSET(socket_fd_, &read_fds);
  }

  std::size_t read(std::uint8_t* buffer, std::size_t max_len) override {
    if (socket_fd_ < 0 || !connected_ || !buffer) {
      return 0;
    }

    ssize_t bytes_read = recv(socket_fd_, buffer, max_len, 0);
    if (bytes_read < 0) {
      // If we would block, return 0 bytes read instead of error
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      }
      // For other errors, treat as a closed channel
      FN_LOGE(TAG, "Read failed: %s", std::strerror(errno));
      close(socket_fd_);
      socket_fd_ = -1;
      connected_ = false;
      return 0;
    }

    // Debug: log received UDP packet (prefix only, to avoid spam)
    {
      char hex[3 * 16 + 1];
      format_hex_prefix(buffer, static_cast<std::size_t>(bytes_read), hex, sizeof(hex));
      if (bytes_read > 16) {
        FN_LOGD(TAG, "Received %zd bytes: %s ...", bytes_read, hex);
      } else {
        FN_LOGD(TAG, "Received %zd bytes: %s", bytes_read, hex);
      }
    }

    return static_cast<std::size_t>(bytes_read);
  }

  void write(const std::uint8_t* buffer, std::size_t len) override {
    if (socket_fd_ < 0 || !connected_ || !buffer) {
      return;
    }

    // Debug: log sent UDP packet (prefix only, to avoid spam)
    {
      char hex[3 * 16 + 1];
      format_hex_prefix(buffer, len, hex, sizeof(hex));
      if (len > 16) {
        FN_LOGD(TAG, "Sending %zu bytes: %s ...", len, hex);
      } else {
        FN_LOGD(TAG, "Sending %zu bytes: %s", len, hex);
      }
    }

    const std::uint8_t* ptr = buffer;
    std::size_t remaining = len;

    while (remaining > 0) {
      ssize_t bytes_sent = send(socket_fd_, ptr, remaining, 0);
      if (bytes_sent < 0) {
        // If we would block, treat as a closed channel
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          FN_LOGE(TAG, "Write would block: %s", std::strerror(errno));
          close(socket_fd_);
          socket_fd_ = -1;
          connected_ = false;
        } else {
          FN_LOGE(TAG, "Write failed: %s", std::strerror(errno));
        }
        break;
      }
      remaining -= static_cast<std::size_t>(bytes_sent);
      ptr += bytes_sent;
    }
  }

 private:
  std::string host_;
  uint16_t port_;
  int socket_fd_;
  bool connected_;
  struct sockaddr_in server_addr_;
};

std::unique_ptr<fujinet::io::Channel> create_udp_channel(const std::string& host, uint16_t port) {
  return std::make_unique<UdpChannel>(host, port);
}

}  // namespace fujinet::platform
