// Derived from palm-sync (https://github.com/jichu4n/palm-sync),
// Copyright Chuan Ji, Apache License 2.0. See docs/hotsync.md.

// HotSync builds only (FN_WITH_HOTSYNC): the ESP32 build compiles every
// source, so elsewhere this file compiles to nothing.
#if FN_WITH_HOTSYNC == 1

#include "fujinet/hotsync/slp.h"

#include <algorithm>
#include <chrono>
#include <iterator>

namespace fujinet::hotsync {

namespace {

constexpr std::uint8_t kSignature[3] = {0xBE, 0xEF, 0xED};
constexpr std::size_t kCrcSize = 2;
constexpr std::size_t kChecksumOffset = 9;
constexpr std::size_t kLengthOffset = 6;

std::uint8_t header_checksum(const std::uint8_t* header)
{
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < kChecksumOffset; ++i) {
        sum = static_cast<std::uint8_t>(sum + header[i]);
    }
    return sum;
}

} // namespace

std::uint16_t slp_crc16(const std::uint8_t* data, std::size_t len, std::uint16_t crc)
{
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i] << 8);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

Bytes slp_encode(const SlpDatagram& datagram)
{
    Bytes frame(std::begin(kSignature), std::end(kSignature));
    frame.push_back(datagram.destSocket);
    frame.push_back(datagram.srcSocket);
    frame.push_back(static_cast<std::uint8_t>(datagram.type));
    put_be16(frame, static_cast<std::uint16_t>(datagram.payload.size()));
    frame.push_back(datagram.xid);
    frame.push_back(header_checksum(frame.data()));
    frame.insert(frame.end(), datagram.payload.begin(), datagram.payload.end());
    put_be16(frame, slp_crc16(frame.data(), frame.size()));
    return frame;
}

// The timeout is an idle timeout: it restarts whenever bytes arrive.
bool SlpReader::read(SlpDatagram& out, std::uint32_t timeoutMs)
{
    for (;;) {
        if (try_parse(out)) {
            return true;
        }
        if (!fill(timeoutMs)) {
            return false;
        }
    }
}

bool SlpReader::read_within(SlpDatagram& out, std::uint32_t limitMs)
{
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(limitMs);
    for (;;) {
        if (try_parse(out)) {
            return true;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0 || !fill(static_cast<std::uint32_t>(left))) {
            return false;
        }
    }
}

bool SlpReader::fill(std::uint32_t timeoutMs)
{
    std::uint8_t chunk[256];
    const int got = _link.read(chunk, sizeof(chunk), timeoutMs);
    if (got < 0) {
        _closed = true;
    }
    if (got <= 0) {
        return false;
    }
    _pending.insert(_pending.end(), chunk, chunk + got);
    return true;
}

bool SlpReader::try_parse(SlpDatagram& out)
{
    for (;;) {
        auto start = std::search(_pending.begin(), _pending.end(),
                                 std::begin(kSignature), std::end(kSignature));
        // Keep a possible partial signature at the tail for the next fill.
        if (start == _pending.end()) {
            const std::size_t keep = std::min<std::size_t>(_pending.size(), sizeof(kSignature) - 1);
            _pending.erase(_pending.begin(), _pending.end() - static_cast<std::ptrdiff_t>(keep));
            return false;
        }
        _pending.erase(_pending.begin(), start);

        if (_pending.size() < kSlpHeaderSize) {
            return false;
        }
        const std::uint8_t* header = _pending.data();
        if (header[kChecksumOffset] != header_checksum(header)) {
            _pending.erase(_pending.begin());
            continue;
        }

        const std::size_t frameSize = kSlpHeaderSize + get_be16(header + kLengthOffset) + kCrcSize;
        if (_pending.size() < frameSize) {
            return false;
        }
        const std::uint16_t crc = get_be16(_pending.data() + frameSize - kCrcSize);
        if (crc != slp_crc16(_pending.data(), frameSize - kCrcSize)) {
            _pending.erase(_pending.begin());
            continue;
        }

        out.destSocket = header[3];
        out.srcSocket = header[4];
        out.type = static_cast<SlpType>(header[5]);
        out.xid = header[8];
        out.payload.assign(_pending.begin() + kSlpHeaderSize,
                           _pending.begin() + static_cast<std::ptrdiff_t>(frameSize - kCrcSize));
        _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(frameSize));
        return true;
    }
}

} // namespace fujinet::hotsync

#endif // FN_WITH_HOTSYNC
