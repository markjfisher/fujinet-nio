#include "fujinet/hotsync/line_arbiter.h"

#include "fujinet/io/core/baud_control.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace fujinet::hotsync {

namespace {

// Bounds the discard so a line that never goes quiet cannot hold us there.
constexpr std::size_t kMaxDiscardBytes = 16384;
constexpr auto kPoll = std::chrono::milliseconds(2);
constexpr auto kLentIdle = std::chrono::milliseconds(10);

} // namespace

LineArbiter::LineArbiter(std::unique_ptr<io::Channel> line, io::BaudControl& baud, std::uint32_t settleMs)
    : _line(std::move(line)), _baud(baud), _settleMs(settleMs)
{}

bool LineArbiter::available()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return !_lent && _line->available();
}

std::size_t LineArbiter::read(std::uint8_t* buffer, std::size_t maxLen)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_lent || buffer == nullptr || maxLen == 0) {
        return 0;
    }
    return _line->read(buffer, maxLen);
}

void LineArbiter::write(const std::uint8_t* buffer, std::size_t len)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_lent) {
        ++_droppedWrites;
        return;
    }
    _line->write(buffer, len);
}

bool LineArbiter::supports_readable_wait() const
{
    return _line->supports_readable_wait();
}

bool LineArbiter::wait_for_readable(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(_mutex);
    if (_lent) {
        lock.unlock();
        std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(kLentIdle)));
        return false;
    }
    return _line->wait_for_readable(timeout);
}

void LineArbiter::discard_input()
{
    std::uint8_t scratch[256];
    std::size_t dropped = 0;
    while (dropped < kMaxDiscardBytes && _line->available()) {
        const std::size_t n = _line->read(scratch, sizeof(scratch));
        if (n == 0) {
            break;
        }
        dropped += n;
    }
}

void LineArbiter::settle() const
{
    if (_settleMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(_settleMs));
    }
}

bool LineArbiter::lend(std::uint32_t baud)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_lent) {
        return true;
    }
    if (_closed || _line->available()) {
        return false;
    }
    _busBaud = _baud.baud_rate();
    if (!_baud.set_baud_rate(baud)) {
        _baud.set_baud_rate(_busBaud);
        return false;
    }
    _lentBaud = baud;
    _lent = true;
    ++_lends;
    return true;
}

void LineArbiter::restore()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_lent) {
        return;
    }
    if (_baud.baud_rate() == _lentBaud) {
        _baud.set_baud_rate(_busBaud);
    }
    discard_input();
    _lent = false;
}

void LineArbiter::close()
{
    restore();
    std::lock_guard<std::mutex> lock(_mutex);
    _closed = true;
}

bool LineArbiter::lent() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _lent;
}

std::uint32_t LineArbiter::lends() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _lends;
}

std::uint32_t LineArbiter::dropped_writes() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _droppedWrites;
}

int LineArbiter::LentLink::read(std::uint8_t* buf, std::size_t len, std::uint32_t timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(_owner._mutex);
            if (!_owner._lent) {
                return -1;
            }
            if (_owner._line->available()) {
                const std::size_t n = _owner._line->read(buf, len);
                if (n > 0) {
                    return static_cast<int>(n);
                }
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return 0;
        }
        std::this_thread::sleep_for(kPoll);
    }
}

int LineArbiter::LentLink::write(const std::uint8_t* buf, std::size_t len)
{
    std::lock_guard<std::mutex> lock(_owner._mutex);
    if (!_owner._lent) {
        return -1;
    }
    _owner._line->write(buf, len);
    return static_cast<int>(len);
}

void LineArbiter::LentLink::set_baud_rate(std::uint32_t baud)
{
    {
        std::lock_guard<std::mutex> lock(_owner._mutex);
        if (!_owner._lent) {
            return;
        }
        _owner._baud.set_baud_rate(baud);
        _owner._lentBaud = baud;
    }
    _owner.settle();
}

} // namespace fujinet::hotsync
