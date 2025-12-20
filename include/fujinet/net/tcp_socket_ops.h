#pragma once

#include <cstdint>
#include <cstddef>

// Include platform socket headers to get socklen_t and ssize_t definitions.
// This is necessary for the function signatures in this header.
// Note: While we prefer to avoid platform-specific code in shared headers,
// including platform headers is necessary for type definitions and is acceptable.
#ifdef ESP_PLATFORM
extern "C" {
#include "lwip/sockets.h"
#include "lwip/netdb.h"
}
#include <unistd.h>
#include <sys/types.h>
#else
// POSIX
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#endif

// Forward declaration
struct sockaddr;

namespace fujinet::net {

// Platform-agnostic socket operations abstraction for TCP.
// Implementations provide the actual syscall glue (POSIX, lwIP, etc.).
class ITcpSocketOps {
public:
    virtual ~ITcpSocketOps() = default;

    // Socket lifecycle
    // Returns socket fd/handle on success, < 0 on error (errno set).
    virtual int socket(int domain, int type, int protocol) = 0;

    // Close socket. Safe to call on invalid fd.
    virtual void close(int fd) = 0;

    // Connect (nonblocking). Returns 0 on immediate success, -1 with errno=EINPROGRESS/EWOULDBLOCK if async.
    virtual int connect(int fd, const struct sockaddr* addr, socklen_t addrlen) = 0;

    // Set nonblocking mode. Returns 0 on success, -1 on error (errno set).
    virtual int set_nonblocking(int fd) = 0;

    // Check if connect completed. Returns:
    // - true if connected (check err via get_so_error)
    // - false if still connecting
    // On error, returns false and sets errno.
    virtual bool poll_connect_complete(int fd) = 0;

    // Send data (nonblocking). Returns bytes sent (>0), 0 if would block, -1 on error (errno set).
    virtual ssize_t send(int fd, const void* buf, size_t len, int flags) = 0;

    // Receive data (nonblocking). Returns bytes received (>0), 0 on EOF, -1 on error (errno set).
    virtual ssize_t recv(int fd, void* buf, size_t len, int flags) = 0;

    // Shutdown write side (half-close). Returns 0 on success, -1 on error (errno set).
    virtual int shutdown_write(int fd) = 0;

    // Get socket error status (SO_ERROR). Returns 0 if no error, errno value otherwise.
    virtual int get_so_error(int fd) = 0;

    // Set socket option. Returns 0 on success, -1 on error (errno set).
    virtual int setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen) = 0;

    // Monotonic time in milliseconds (for timeouts).
    virtual std::uint64_t now_ms() = 0;

    // Get last errno value from this platform.
    virtual int last_errno() = 0;

    // Get error string for errno (for logging).
    virtual const char* err_string(int errno_val) = 0;
};

} // namespace fujinet::net
