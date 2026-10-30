#pragma once

#include <cstddef>
#include <cstdint>

namespace fujinet::hotsync {

// Raw byte pipe to a Palm: a TCP socket, a lent serial line, or a test fixture.
class Link {
public:
    virtual ~Link() = default;

    // Reads up to len bytes, waiting at most timeoutMs for the first one.
    // Returns the count read, 0 on timeout, or -1 once the link is closed.
    virtual int read(std::uint8_t* buf, std::size_t len, std::uint32_t timeoutMs) = 0;
    // Returns len once every byte is written, or -1.
    virtual int write(const std::uint8_t* buf, std::size_t len) = 0;

    // Only a serial line honours this; sockets and fixtures ignore it.
    virtual void set_baud_rate(std::uint32_t baud) { (void)baud; }
};

} // namespace fujinet::hotsync
