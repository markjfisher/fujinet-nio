
#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace fujinet::io {

class IPacketIO;

// Abstract byte-level I/O channel (ACM, TTY, UART, etc.).
//
// Readability is a state, not a notification. available() must stay true for
// as long as unread bytes remain, however they were announced, and read() must
// return bytes the platform actually holds, in arrival order, never a count
// taken from an event. POSIX gives this directly: the channels poll() for
// POLLIN, which reports current state, and read() non-blocking descriptors.
//
// A platform whose driver announces arrivals through events instead (the
// ESP-IDF UART, edge-triggered epoll, kqueue counts, Windows overlapped I/O,
// RTOS UART drivers) must not read just what each event announced. Such
// drivers drop events under load while keeping the bytes, which then sit
// unread until they surface inside a later packet. Read everything the driver
// holds; fujinet/io/uart_rx_drain.h does this for any event-driven driver.
class Channel {
public:
    virtual ~Channel() = default;

    // Optional complete-packet capability. No byte-stream boundary inference.
    // The returned adapter must outlive its users and retain its identity.
    virtual IPacketIO* packet_io() { return nullptr; }

    // Are there bytes available to read without blocking? True while any
    // unread bytes remain (see above).
    virtual bool available() = 0;

    // Read up to maxLen of the bytes received so far into buffer, oldest
    // first, without blocking. Returns the number actually read (0 if none);
    // fewer than maxLen is normal, and bytes left over stay readable.
    virtual std::size_t read(std::uint8_t* buffer, std::size_t maxLen) = 0;

    // Write len bytes from buffer.
    virtual void write(const std::uint8_t* buffer, std::size_t len) = 0;

    // Changes whenever the peer behind the channel changes: a connection made or
    // lost. A consumer holding partly received protocol state compares it before
    // using new bytes, and drops that state when it differs, so a half frame from
    // a lost peer is not completed by a new one. Channels with one permanent peer
    // (UART, PTY) never change it.
    virtual std::uint32_t connection_epoch() const { return 0; }

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
