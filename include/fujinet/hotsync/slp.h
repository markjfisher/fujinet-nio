#pragma once

// Serial Link Protocol: the framing layer of serial HotSync.
// Reference: palm-sync src/protocols/slp-protocol.ts

#include "fujinet/hotsync/bytes.h"
#include "fujinet/hotsync/link.h"

#include <cstddef>
#include <cstdint>

namespace fujinet::hotsync {

enum class SlpType : std::uint8_t {
    System = 0,
    Padp = 2,
    Loopback = 3,
};

inline constexpr std::uint8_t kSlpSocketDlp = 3;
inline constexpr std::size_t kSlpHeaderSize = 10;

struct SlpDatagram {
    std::uint8_t destSocket{kSlpSocketDlp};
    std::uint8_t srcSocket{kSlpSocketDlp};
    SlpType type{SlpType::Padp};
    std::uint8_t xid{0};
    Bytes payload;
};

// CRC-16/CCITT, polynomial 0x1021, over the header and body.
std::uint16_t slp_crc16(const std::uint8_t* data, std::size_t len, std::uint16_t crc = 0);

Bytes slp_encode(const SlpDatagram& datagram);

// Pulls bytes from a link and yields whole, checksum-verified datagrams.
// Garbage before a signature and corrupt frames are skipped.
class SlpReader {
public:
    explicit SlpReader(Link& link) : _link(link) {}

    // True when a datagram arrives before the link stays silent for timeoutMs.
    bool read(SlpDatagram& out, std::uint32_t timeoutMs);
    // True when a datagram arrives within limitMs, however busy the link is.
    bool read_within(SlpDatagram& out, std::uint32_t limitMs);
    bool link_closed() const { return _closed; }

private:
    bool fill(std::uint32_t timeoutMs);
    bool try_parse(SlpDatagram& out);

    Link& _link;
    Bytes _pending;
    bool _closed{false};
};

} // namespace fujinet::hotsync
