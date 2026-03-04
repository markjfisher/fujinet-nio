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

// Platform-agnostic socket operations abstraction for UDP.
// Implementations provide the actual syscall glue (POSIX, lwIP, etc.).
class IUdpSocketOps {
public:
    virtual ~IUdpSocketOps() = default;

    // Socket lifecycle
    // Returns socket fd/handle on success, < 0 on error (errno set).
    virtual int socket(int domain, int type, int protocol) = 0;

    // Close socket. Safe to call on invalid fd.
    virtual void close(int fd) = 0;

    // Connect (for connected UDP socket). Returns 0 on success, -1 on error (errno set).
    virtual int connect(int fd, const struct sockaddr* addr, SockLen addrlen) = 0;

    // Set nonblocking mode. Returns 0 on success, -1 on error (errno set).
    virtual int set_nonblocking(int fd) = 0;

    // Send data (nonblocking). Returns bytes sent (>0), -1 on error (errno set).
    virtual SSize send(int fd, const void* buf, std::size_t len) = 0;

    // Receive data (nonblocking). Returns bytes received (>0), -1 on error (errno set).
    virtual SSize recv(int fd, void* buf, std::size_t len) = 0;

    // Poll for readable data. Returns true if data is available to read.
    virtual bool poll_readable(int fd) = 0;

    // Address resolution
    virtual int getaddrinfo(const char* host, const char* port, const void* hints, AddrInfo** out) = 0;

    // Platform-provided hints for a UDP socket resolution
    virtual const void* udp_addrinfo_hints() const noexcept = 0;

    // Free address resolution result
    virtual void freeaddrinfo(AddrInfo* ai) = 0;

    // Iterate address list
    virtual AddrInfo* addrinfo_next(AddrInfo* ai) = 0;

    // Get address family, socktype, protocol from addrinfo
    virtual int addrinfo_family(AddrInfo* ai) = 0;
    virtual int addrinfo_socktype(AddrInfo* ai) = 0;
    virtual int addrinfo_protocol(AddrInfo* ai) = 0;

    // Get sockaddr and length from addrinfo
    virtual const struct sockaddr* addrinfo_addr(AddrInfo* ai, SockLen* out_len) = 0;

    // Error handling
    virtual int last_errno() = 0;
    virtual const char* err_string(int errno_val) = 0;
    virtual bool is_would_block(int errno_val) const noexcept = 0;
};

} // namespace fujinet::net
