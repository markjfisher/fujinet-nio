#pragma once

// The FujiBus serial line, lendable at another rate (docs/hotsync.md,
// "Cradle"). LineArbiter owns the line and its rate and knows nothing of what
// is sent on it: no SLIP, no FujiBus. FujiBus reads and writes it through
// io::Channel while it is the bus's; a borrower (the HotSync service) uses
// link() while it is lent. CradleCoordinator decides when to lend it.
//
// One mutex guards the line, so only one side touches it at a time. While it
// is lent the bus reads nothing and its writes are dropped.
// wait_for_readable() holds the mutex for its whole wait, because the ESP32
// UART reads its driver while it waits.

#include "fujinet/hotsync/link.h"
#include "fujinet/io/core/channel.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace fujinet::io {
class BaudControl;
}

namespace fujinet::hotsync {

class LineArbiter final : public io::Channel {
public:
    // `line` is the bus line; `baud` changes its rate (usually the same
    // object). settleMs is how long the line rests after a rate change before
    // anyone talks on it.
    LineArbiter(std::unique_ptr<io::Channel> line, io::BaudControl& baud, std::uint32_t settleMs = 50);

    // io::Channel: the bus's view.
    bool available() override;
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override;
    void write(const std::uint8_t* buffer, std::size_t len) override;
    bool supports_readable_wait() const override;
    bool wait_for_readable(std::chrono::milliseconds timeout) override;

    // The line underneath, for its own diagnostics.
    io::Channel& line() { return *_line; }

    // Lends the line at `baud`, unless bytes are waiting to be read: they
    // arrived for the bus, which must see them first. Called at the bus's
    // idle point, on the bus's task, so it never lands in the middle of a
    // request the bus has read part of or a response it is writing.
    bool lend(std::uint32_t baud);
    // The lent line, for the borrower until restore(). Reads and writes fail
    // once it is restored.
    Link& link() { return _link; }
    // Waits settleMs, as after any rate change. For the borrower, after lend().
    void settle() const;
    // Puts the bus rate back and drops what arrived at the other rate. A rate
    // someone else set while the line was lent (the console's uart.*
    // commands act on the line itself) is kept rather than undone.
    void restore();
    // For shutdown: restores the line and lends it no more.
    void close();

    bool lent() const;
    std::uint32_t lends() const;
    std::uint32_t dropped_writes() const;

private:
    class LentLink final : public Link {
    public:
        explicit LentLink(LineArbiter& owner) : _owner(owner) {}
        int read(std::uint8_t* buf, std::size_t len, std::uint32_t timeoutMs) override;
        int write(const std::uint8_t* buf, std::size_t len) override;
        void set_baud_rate(std::uint32_t baud) override;

    private:
        LineArbiter& _owner;
    };

    void discard_input();

    std::unique_ptr<io::Channel> _line;
    io::BaudControl& _baud;
    std::uint32_t _settleMs;
    LentLink _link{*this};

    mutable std::mutex _mutex;
    bool _lent{false};
    bool _closed{false};
    std::uint32_t _busBaud{0};
    std::uint32_t _lentBaud{0}; // the rate this arbiter last set while lent
    std::uint32_t _lends{0};
    std::uint32_t _droppedWrites{0};
};

} // namespace fujinet::hotsync
