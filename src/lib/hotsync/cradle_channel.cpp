// HotSync builds only (FN_WITH_HOTSYNC): the ESP32 build compiles every
// source, so elsewhere this file compiles to nothing.
#if FN_WITH_HOTSYNC == 1

#include "fujinet/hotsync/cradle_channel.h"

#include "fujinet/io/core/baud_control.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/protocol/slip_codec.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace fujinet::hotsync {

namespace {

constexpr std::uint8_t kSlipEnd = io::protocol::to_byte(io::protocol::SlipByte::End);
// The longest a FujiBus packet (at most 65535 bytes) can be once SLIP
// escapes every byte, with its opening END. Longer is not FujiBus.
constexpr std::size_t kMaxFrameBytes = 2 * 65535 + 1;
// A frame buffer grown past this by a long run of noise is given back.
constexpr std::size_t kKeepFrameCapacity = 4096;
// Bounds the discard so a line that never goes quiet cannot hold us here.
constexpr std::size_t kMaxDiscardBytes = 16384;
constexpr auto kPoll = std::chrono::milliseconds(2);
constexpr auto kLentIdle = std::chrono::milliseconds(10);

// The FujiBus transport's own test of a frame: SlipFramer decodes it with
// decodeSLIP and the transport takes it only if FujiBusPacket::fromRaw does.
// `frame` starts with its opening END.
bool is_fujibus_packet(const io::protocol::ByteBuffer& frame)
{
    const io::protocol::ByteBuffer raw = io::protocol::decodeSLIP(frame);
    return !raw.empty() && io::protocol::FujiBusPacket::fromRaw(raw) != nullptr;
}

std::uint64_t steady_now_ms()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

CradleChannel::CradleChannel(std::unique_ptr<io::Channel> line, io::BaudControl& baud, NowMs now,
                             CradleTiming timing)
    : _line(std::move(line)), _baud(baud), _now(now ? std::move(now) : NowMs(&steady_now_ms)),
      _timing(timing)
{}

bool CradleChannel::available()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return !_lent && (!_pending.empty() || _line->available());
}

std::size_t CradleChannel::read(std::uint8_t* buffer, std::size_t maxLen)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_lent || buffer == nullptr || maxLen == 0) {
        return 0;
    }
    if (!_pending.empty()) {
        const std::size_t n = std::min(maxLen, _pending.size());
        std::copy(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(n), buffer);
        _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
    const std::size_t n = _line->read(buffer, maxLen);
    if (n > 0) {
        scan(buffer, n, _now());
    }
    return n;
}

void CradleChannel::write(const std::uint8_t* buffer, std::size_t len)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_lent) {
        ++_stats.droppedWrites;
        return;
    }
    _line->write(buffer, len);
}

bool CradleChannel::supports_readable_wait() const
{
    return _line->supports_readable_wait();
}

bool CradleChannel::wait_for_readable(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(_mutex);
    if (_lent) {
        lock.unlock();
        std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(kLentIdle)));
        return false;
    }
    if (!_pending.empty()) {
        return true;
    }
    // Held across the wait: some lines (the ESP32 UART) read their driver
    // while waiting, so a claim must not start until the wait ends. claim()
    // therefore waits up to one core idle wait (20 ms on ESP32).
    return _line->wait_for_readable(timeout);
}

void CradleChannel::start_frame()
{
    _inFrame = true;
    _frame.assign(1, kSlipEnd);
}

void CradleChannel::end_frame()
{
    _inFrame = false;
    _frame.clear();
    if (_frame.capacity() > kKeepFrameCapacity) {
        io::protocol::ByteBuffer().swap(_frame);
    }
}

void CradleChannel::stray(std::size_t count, std::uint64_t now)
{
    _stats.strayBytes += static_cast<std::uint32_t>(count);
    if (!_sawFrame || now - _lastFrameMs >= _timing.strayGraceMs) {
        _holdUntilMs = now;
    }
}

// Follows SLIP framing (END bytes only) and hands each whole frame to
// is_fujibus_packet(), so the cradle and the transport cannot disagree about
// what a FujiBus packet is.
void CradleChannel::scan(const std::uint8_t* data, std::size_t len, std::uint64_t now)
{
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint8_t b = data[i];
        if (b != kSlipEnd) {
            if (!_inFrame) {
                stray(1, now);
                continue;
            }
            _frame.push_back(b);
            if (_frame.size() > kMaxFrameBytes) {
                stray(_frame.size() - 1, now);
                end_frame();
            }
            continue;
        }
        if (_inFrame && _frame.size() > 1) {
            if (is_fujibus_packet(_frame)) {
                ++_stats.frames;
                _sawFrame = true;
                _lastFrameMs = now;
                _holdUntilMs = now + _timing.hostHoldMs;
                end_frame();
                continue;
            }
            stray(_frame.size() - 1, now);
        }
        // An END after anything but a packet may open the next one.
        start_frame();
    }
}

void CradleChannel::discard_pending()
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

void CradleChannel::settle() const
{
    if (_timing.settleMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(_timing.settleMs));
    }
}

bool CradleChannel::claim(std::uint32_t baud)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_lent) {
            return true;
        }
        // Scan what FujiBus has not read yet: a request waiting there means
        // a host is active, and it must still reach FujiBus.
        if (_pending.empty()) {
            std::uint8_t scratch[256];
            while (_pending.size() < kMaxDiscardBytes && _line->available()) {
                const std::size_t n = _line->read(scratch, sizeof(scratch));
                if (n == 0) {
                    break;
                }
                scan(scratch, n, _now());
                _pending.insert(_pending.end(), scratch, scratch + n);
            }
        }
        if (_now() < _holdUntilMs) {
            ++_stats.refused;
            return false;
        }
        _busBaud = _baud.baud_rate();
        if (!_baud.set_baud_rate(baud)) {
            _baud.set_baud_rate(_busBaud);
            return false;
        }
        _pending.clear();
        discard_pending();
        end_frame();
        _lent = true;
        ++_stats.lends;
    }
    settle();
    return true;
}

void CradleChannel::release()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_lent) {
        return;
    }
    _baud.set_baud_rate(_busBaud);
    discard_pending();
    _lent = false;
}

bool CradleChannel::lent() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _lent;
}

bool CradleChannel::host_active() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _now() < _holdUntilMs;
}

CradleStats CradleChannel::stats() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _stats;
}

int CradleChannel::LentLink::read(std::uint8_t* buf, std::size_t len, std::uint32_t timeoutMs)
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

int CradleChannel::LentLink::write(const std::uint8_t* buf, std::size_t len)
{
    std::lock_guard<std::mutex> lock(_owner._mutex);
    if (!_owner._lent) {
        return -1;
    }
    _owner._line->write(buf, len);
    return static_cast<int>(len);
}

void CradleChannel::LentLink::set_baud_rate(std::uint32_t baud)
{
    {
        std::lock_guard<std::mutex> lock(_owner._mutex);
        if (!_owner._lent) {
            return;
        }
        _owner._baud.set_baud_rate(baud);
    }
    _owner.settle();
}

} // namespace fujinet::hotsync

#endif // FN_WITH_HOTSYNC
