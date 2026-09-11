
#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace fujinet::io {

class IPacketIO;

// Abstract byte-level I/O channel (ACM, TTY, UART, etc.).
class Channel {
public:
    virtual ~Channel() = default;

    // Optional complete-packet capability. No byte-stream boundary inference.
    // The returned adapter must outlive its users and retain its identity.
    virtual IPacketIO* packet_io() { return nullptr; }

    // Are there bytes available to read without blocking?
    virtual bool available() = 0;

    // Read up to maxLen bytes into buffer.
    // Returns the number of bytes actually read (0 if none).
    virtual std::size_t read(std::uint8_t* buffer, std::size_t maxLen) = 0;

    // Write len bytes from buffer.
    virtual void write(const std::uint8_t* buffer, std::size_t len) = 0;

    // Optionally wait until the channel may have bytes to read.
    // Returns true if work may be available now. The default is non-blocking
    // and lets the application loop fall back to its normal idle delay.
    virtual bool supports_readable_wait() const { return false; }

    virtual bool wait_for_readable(std::chrono::milliseconds timeout) {
        (void)timeout;
        return false;
    }
};

} // namespace fujinet::io
