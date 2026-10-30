#pragma once

// A Palm cradle on the FujiBus serial line (docs/hotsync.md, "Cradle").
//
// One line carries two protocols: FujiBus from Palm apps at the bus rate, and
// HotSync, which a Palm opens at 9600 baud when its HotSync button is pressed.
// CradleChannel owns the line. FujiBus sees it as an ordinary io::Channel; the
// HotSync service borrows it between FujiBus conversations:
//
// - Every FujiBus packet keeps the line for the bus for hostHoldMs: a Palm
//   running an app cannot be HotSyncing. A SLIP frame counts as a packet only
//   when the FujiBus transport would take it (decodeSLIP, then
//   FujiBusPacket::fromRaw); a Palm's SLP frames carry SLIP END bytes too.
// - Any other bytes, with no packet close by, end that hold at once. That is
//   what a HotSync start looks like on the bus.
// - While the line is free, claim() lends it at another rate; release() puts
//   the bus rate back and drops what arrived at the other rate.
//
// Only one side touches the line at a time: FujiBus while it is the bus's,
// the HotSync service while it is lent. FujiBus reads nothing and its writes
// are dropped while the line is lent; a Palm app retries.
//
// One mutex guards the line. wait_for_readable() holds it for its whole wait,
// because the ESP32 UART reads its driver while it waits; so claim(), on the
// service task, can wait up to one core idle wait (20 ms on ESP32) for it.

#include "fujinet/hotsync/link.h"
#include "fujinet/io/core/channel.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace fujinet::io {
class BaudControl;
}

namespace fujinet::hotsync {

struct CradleTiming {
    std::uint32_t hostHoldMs{60000}; // a FujiBus frame keeps the line for the bus this long
    std::uint32_t strayGraceMs{1000}; // stray bytes this close to a frame are bus noise
    std::uint32_t settleMs{50};      // after every rate change, before talking
};

struct CradleStats {
    std::uint32_t frames{0};       // FujiBus packets seen
    std::uint32_t strayBytes{0};   // bytes that were not part of a FujiBus packet
    std::uint32_t lends{0};        // times the line was lent
    std::uint32_t refused{0};      // claims refused because a FujiBus host was active
    std::uint32_t droppedWrites{0}; // FujiBus writes dropped while lent
};

class CradleChannel final : public io::Channel {
public:
    using NowMs = std::function<std::uint64_t()>;

    // `line` is the bus line; `baud` changes its rate (usually the same
    // object). `now` defaults to a monotonic clock.
    CradleChannel(std::unique_ptr<io::Channel> line, io::BaudControl& baud, NowMs now = {},
                  CradleTiming timing = {});

    // io::Channel: the FujiBus view.
    bool available() override;
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override;
    void write(const std::uint8_t* buffer, std::size_t len) override;
    bool supports_readable_wait() const override;
    bool wait_for_readable(std::chrono::milliseconds timeout) override;

    // The line underneath, for its own diagnostics.
    io::Channel& line() { return *_line; }

    // HotSync side, from the service task. claim() lends the line at `baud`
    // unless a FujiBus host is active; link() is the lent line until release().
    bool claim(std::uint32_t baud);
    Link& link() { return _link; }
    void release();

    bool lent() const;
    bool host_active() const;
    CradleStats stats() const;

private:
    class LentLink final : public Link {
    public:
        explicit LentLink(CradleChannel& owner) : _owner(owner) {}
        int read(std::uint8_t* buf, std::size_t len, std::uint32_t timeoutMs) override;
        int write(const std::uint8_t* buf, std::size_t len) override;
        void set_baud_rate(std::uint32_t baud) override;

    private:
        CradleChannel& _owner;
    };

    void scan(const std::uint8_t* data, std::size_t len, std::uint64_t now);
    void stray(std::size_t count, std::uint64_t now);
    void start_frame();
    void end_frame();
    void discard_pending();
    void settle() const;

    std::unique_ptr<io::Channel> _line;
    io::BaudControl& _baud;
    NowMs _now;
    CradleTiming _timing;
    LentLink _link{*this};

    mutable std::mutex _mutex;
    // Bytes a claim read off the line to scan, already scanned, kept for
    // FujiBus when they held a frame.
    std::vector<std::uint8_t> _pending;
    bool _lent{false};
    std::uint32_t _busBaud{0};
    // SLIP framing as seen on the way to FujiBus: the frame so far, as sent,
    // from its opening END.
    bool _inFrame{false};
    std::vector<std::uint8_t> _frame;
    bool _sawFrame{false};
    std::uint64_t _lastFrameMs{0};
    std::uint64_t _holdUntilMs{0};
    CradleStats _stats;
};

} // namespace fujinet::hotsync
