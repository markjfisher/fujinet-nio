#pragma once

#include <cstdint>
#include <cstddef>

// Forward declarations only - no platform headers
struct sockaddr;
struct addrinfo;

namespace fujinet::net {

// Portable type aliases to avoid platform-specific socklen_t/ssize_t
using SockLen = std::uint32_t;
using SSize = std::ptrdiff_t;

// Address resolution result (opaque handle)
struct AddrInfo;

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
    virtual int connect(int fd, const struct sockaddr* addr, SockLen addrlen) = 0;

    // Set nonblocking mode. Returns 0 on success, -1 on error (errno set).
    virtual int set_nonblocking(int fd) = 0;

    // Check if connect completed. Returns:
    // - true if connected (check err via get_so_error)
    // - false if still connecting
    // On error, returns false and sets errno.
    virtual bool poll_connect_complete(int fd) = 0;

    // Send data (nonblocking). Returns bytes sent (>0), 0 if would block, -1 on error (errno set).
    // Flags are platform-specific and handled by implementation.
    virtual SSize send(int fd, const void* buf, std::size_t len, int flags) = 0;

    // Receive data (nonblocking). Returns bytes received (>0), 0 on EOF, -1 on error (errno set).
    // Flags are platform-specific and handled by implementation.
    virtual SSize recv(int fd, void* buf, std::size_t len, int flags) = 0;

    // Shutdown write side (half-close). Returns 0 on success, -1 on error (errno set).
    virtual int shutdown_write(int fd) = 0;

    // Get socket error status (SO_ERROR). Returns 0 if no error, errno value otherwise.
    virtual int get_so_error(int fd) = 0;

    // Set socket option. Returns 0 on success, -1 on error (errno set).
    virtual int setsockopt(int fd, int level, int optname, const void* optval, SockLen optlen) = 0;

    // Address resolution
    // Resolve hostname and port. Returns 0 on success, non-zero error code on failure.
    // On success, *out is set to an opaque handle that must be freed with free_addrinfo.
    virtual int getaddrinfo(const char* host, const char* port, void* hints, AddrInfo** out) = 0;

    // Free address resolution result
    virtual void freeaddrinfo(AddrInfo* ai) = 0;

    // Iterate address list (returns next address or nullptr)
    virtual AddrInfo* addrinfo_next(AddrInfo* ai) = 0;

    // Get address family from addrinfo
    virtual int addrinfo_family(AddrInfo* ai) = 0;

    // Get socket type from addrinfo
    virtual int addrinfo_socktype(AddrInfo* ai) = 0;

    // Get protocol from addrinfo
    virtual int addrinfo_protocol(AddrInfo* ai) = 0;

    // Get sockaddr pointer and length from addrinfo
    virtual const struct sockaddr* addrinfo_addr(AddrInfo* ai, SockLen* out_len) = 0;

    // Monotonic time in milliseconds (for timeouts).
    virtual std::uint64_t now_ms() = 0;

    // Get last errno value from this platform.
    virtual int last_errno() = 0;

    // Get error string for errno (for logging).
    virtual const char* err_string(int errno_val) = 0;
};

} // namespace fujinet::net
