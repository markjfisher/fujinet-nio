#include "fujinet/hotsync/cradle_coordinator.h"

#include <chrono>
#include <utility>

namespace fujinet::hotsync {

namespace {

std::uint64_t steady_now_ms()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

CradleCoordinator::CradleCoordinator(LineArbiter& line, NowMs now, CradleTiming timing)
    : _line(line), _now(now ? std::move(now) : NowMs(&steady_now_ms)), _timing(timing)
{}

void CradleCoordinator::bytes_received(std::size_t count)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _unmatched += count;
    _stalling = false;
}

void CradleCoordinator::frame_received(bool valid)
{
    if (!valid) {
        return; // its bytes stay unmatched
    }
    std::lock_guard<std::mutex> lock(_mutex);
    const std::uint64_t now = _now();
    ++_stats.frames;
    _stalling = false;
    _sawFrame = true;
    _lastFrameMs = now;
    _holdUntilMs = now + _timing.hostHoldMs;
    _unmatched = 0;
}

bool CradleCoordinator::line_idle(bool partial)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const std::uint64_t now = _now();
    _transportSeen = true;

    // A partial frame's stall clock starts at the first idle point that finds
    // it with no new bytes, after any requests in hand have been handled.
    if (!partial) {
        _stalling = false;
    } else if (!_stalling) {
        _stalling = true;
        _stallSinceMs = now;
    }
    const bool expired = partial && now - _stallSinceMs >= _timing.partialExpiryMs;
    if (expired) {
        ++_stats.expiredFrames;
        _stalling = false;
    }
    const bool settled = !partial || expired; // nothing more can join the bytes so far

    // Bytes that made no request, away from any request: what a Palm's
    // HotSync start looks like at the bus rate. They end the hold.
    if (settled && _unmatched > 0) {
        _stats.strayBytes += static_cast<std::uint32_t>(_unmatched);
        _unmatched = 0;
        if (!_sawFrame || now - _lastFrameMs >= _timing.strayGraceMs) {
            _holdUntilMs = now;
        }
    }

    bool drop = expired;
    if (_request == Request::Pending && !_shutdown) {
        if (now < _holdUntilMs) {
            _request = Request::Refused;
            ++_stats.refused;
            _decided.notify_all();
        } else if (settled && _line.lend(_requestBaud)) {
            // lend() refuses while bytes wait unread; they are the bus's, so
            // the claim waits for the next idle point, after they are read.
            _request = Request::Granted;
            ++_stats.lends;
            _decided.notify_all();
            drop = partial;
        }
    }
    return drop;
}

CradleClaim CradleCoordinator::claim(std::uint32_t baud)
{
    std::unique_lock<std::mutex> lock(_mutex);
    if (_shutdown) {
        return CradleClaim::Busy;
    }
    if (_line.lent()) {
        return CradleClaim::Granted;
    }
    if (_now() < _holdUntilMs) {
        ++_stats.refused;
        return CradleClaim::HostActive;
    }
    _requestBaud = baud;
    _request = Request::Pending;
    _decided.wait_for(lock, std::chrono::milliseconds(_timing.claimWaitMs),
                      [this] { return _request != Request::Pending || _shutdown; });
    const Request outcome = _request;
    _request = Request::None;
    lock.unlock();

    switch (outcome) {
    case Request::Granted:
        _line.settle();
        return CradleClaim::Granted;
    case Request::Refused:
        return CradleClaim::HostActive;
    case Request::None:
    case Request::Pending:
        break;
    }
    return CradleClaim::Busy;
}

void CradleCoordinator::release()
{
    _line.restore();
}

void CradleCoordinator::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _shutdown = true;
        _decided.notify_all();
    }
    _line.close();
}

bool CradleCoordinator::claim_pending() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _request == Request::Pending;
}

bool CradleCoordinator::host_active() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _now() < _holdUntilMs;
}

bool CradleCoordinator::transport_seen() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _transportSeen;
}

CradleStats CradleCoordinator::stats() const
{
    CradleStats s;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        s = _stats;
    }
    s.droppedWrites = _line.dropped_writes();
    return s;
}

} // namespace fujinet::hotsync
