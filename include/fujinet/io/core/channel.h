#pragma once

#include <cstddef>
#include <cstdint>

namespace fujinet::io {

// Abstract byte-level I/O channel (ACM, TTY, UART, etc.).
class Channel {
public:
    virtual ~Channel() = default;

    // Are there bytes available to read without blocking?
    virtual bool available() = 0;

    // Read up to maxLen bytes into buffer.
    // Returns the number of bytes actually read (0 if none).
    virtual std::size_t read(std::uint8_t* buffer, std::size_t maxLen) = 0;

    // Write len bytes from buffer.
    virtual void write(const std::uint8_t* buffer, std::size_t len) = 0;
};

} // namespace fujinet::io
