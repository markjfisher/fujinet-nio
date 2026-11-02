#pragma once

// A Palm cradle on the FujiBus serial line (docs/hotsync.md, "Cradle").
//
// One line carries two protocols: FujiBus from Palm apps at the bus rate, and
// HotSync, which a Palm opens at 9600 baud when its HotSync button is pressed.
// The line itself is a LineArbiter, which can lend it at another rate and
// knows nothing of either protocol. CradleCoordinator decides when it may be
// lent, from what the FujiBus transport reports as it works
// (io::IBusLineCoordinator):
//
// - Every FujiBus request keeps the line for the bus for hostHoldMs: a Palm
//   running an app cannot be HotSyncing. A request is what the transport
//   decoded as one, so the cradle and the transport cannot disagree.
// - Bytes that made no request (a Palm's 9600-baud WAKEUP read at the bus
//   rate, for one) end that hold at once, unless a request came within
//   strayGraceMs: then they are bus noise.
// - The HotSync service claims the line from its own task. The claim waits
//   for the transport's idle point, on the core task, and the line is lent
//   there and only there: never with a request partly received or a response
//   owed, nor with bytes waiting that the transport has not read yet. A
//   partial frame that gets no new bytes for partialExpiryMs of idle points
//   has stalled; it is dropped and no longer holds up a claim. That clock
//   starts at the idle point, so time spent handling requests never counts.
//
// The one hand-over cost left is the one a listening window always has: a
// request that starts arriving while the line is lent, or within the moment
// it takes to change the rate, is lost, and the Palm app retries.

#include "fujinet/hotsync/line_arbiter.h"
#include "fujinet/io/transport/bus_line_coordinator.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

namespace fujinet::hotsync {

struct CradleTiming {
    std::uint32_t hostHoldMs{60000};     // a FujiBus request keeps the line for the bus this long
    std::uint32_t strayGraceMs{1000};    // stray bytes this close to a request are bus noise
    std::uint32_t partialExpiryMs{500};  // a partial frame idle this long with no new bytes has stalled
    std::uint32_t claimWaitMs{1000};     // how long a claim waits for the transport's idle point
};

struct CradleStats {
    std::uint32_t frames{0};        // FujiBus requests seen
    std::uint32_t strayBytes{0};    // bytes that made no FujiBus request
    std::uint32_t expiredFrames{0}; // partial frames dropped after stalling
    std::uint32_t lends{0};         // times the line was lent
    std::uint32_t refused{0};       // claims refused because a FujiBus host was active
    std::uint32_t droppedWrites{0}; // FujiBus writes dropped while lent
};

enum class CradleClaim : std::uint8_t {
    Granted,    // the line is lent: use link(), then release()
    HostActive, // a FujiBus host holds the line
    Busy,       // the transport reached no idle point in time
};

class CradleCoordinator final : public io::IBusLineCoordinator {
public:
    using NowMs = std::function<std::uint64_t()>;

    // `now` defaults to a monotonic clock.
    explicit CradleCoordinator(LineArbiter& line, NowMs now = {}, CradleTiming timing = {});

    // io::IBusLineCoordinator: from the FujiBus transport, on the core task.
    void bytes_received(std::size_t count) override;
    void frame_received(bool valid) override;
    bool line_idle(bool partial) override;

    // HotSync side, from the service task. claim() blocks until the
    // transport's next idle point decides, or claimWaitMs.
    CradleClaim claim(std::uint32_t baud);
    Link& link() { return _line.link(); }
    void release();
    // For shutdown: ends a claim at once, refuses later ones, and gives the
    // line back to the bus, so a sync on the lent line fails fast.
    void shutdown();

    bool lent() const { return _line.lent(); }
    bool host_active() const;
    // True while a claim waits for the transport's idle point.
    bool claim_pending() const;
    // False until the transport has reached an idle point once: a cradle with
    // no FujiBus transport on its line can never be lent.
    bool transport_seen() const;
    CradleStats stats() const;

private:
    enum class Request : std::uint8_t { None, Pending, Granted, Refused };

    LineArbiter& _line;
    NowMs _now;
    CradleTiming _timing;

    mutable std::mutex _mutex;
    std::condition_variable _decided;
    Request _request{Request::None};
    std::uint32_t _requestBaud{0};
    bool _transportSeen{false};
    bool _shutdown{false};
    // Bytes since the last request; stray once nothing more can join them.
    std::size_t _unmatched{0};
    // Since when a partial frame has sat at idle points with no new bytes.
    bool _stalling{false};
    std::uint64_t _stallSinceMs{0};
    bool _sawFrame{false};
    std::uint64_t _lastFrameMs{0};
    std::uint64_t _holdUntilMs{0};
    CradleStats _stats;
};

} // namespace fujinet::hotsync
