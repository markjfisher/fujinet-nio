#pragma once

// Packet Assembly/Disassembly Protocol over SLP, with the CMP handshake that
// opens a serial HotSync.
// Reference: palm-sync src/protocols/padp-protocol.ts and cmp-protocol.ts

#include "fujinet/hotsync/dlp_transport.h"
#include "fujinet/hotsync/link.h"
#include "fujinet/hotsync/slp.h"

#include <cstdint>
#include <deque>

namespace fujinet::hotsync {

inline constexpr std::uint32_t kCmpInitialBaud = 9600;
inline constexpr std::uint32_t kCmpMaxBaud = 115200;

class PadpTransport : public DlpTransport {
public:
    explicit PadpTransport(Link& link, std::uint32_t maxBaud = kCmpMaxBaud);

    // accept() is wait_for_wakeup() then answer_wakeup(). A cradle that only
    // listens for a while bounds the first, never the second. The wait for
    // WAKEUP ends at timeoutMs even while bytes keep arriving.
    bool accept() override;
    bool wait_for_wakeup(std::uint32_t timeoutMs = 1000);
    bool answer_wakeup();
    bool send(const Bytes& message) override;
    bool receive(Bytes& message, std::uint32_t timeoutMs) override;

    std::uint32_t baud_rate() const { return _baud; }

private:
    struct Fragment {
        std::uint8_t type{0};
        std::uint8_t flags{0};
        std::uint32_t sizeOrOffset{0};
        Bytes data;
    };

    bool send_fragment(std::uint8_t xid, const Fragment& fragment);
    bool wait_for_ack(std::uint8_t xid, std::uint32_t sizeOrOffset);
    bool handle_incoming(const SlpDatagram& datagram, std::uint8_t awaitedXid,
                         std::uint32_t awaitedOffset, bool& acked);
    bool is_duplicate(const SlpDatagram& datagram) const;
    bool accept_data(const SlpDatagram& datagram, const Fragment& fragment);
    void send_ack(const SlpDatagram& datagram, const Fragment& fragment);
    static Bytes encode_fragment(const Fragment& fragment);
    static bool decode_fragment(const Bytes& payload, Fragment& out);
    std::uint8_t next_xid();

    Link& _link;
    SlpReader _reader;
    std::uint32_t _maxBaud;
    std::uint32_t _baud{kCmpInitialBaud};
    std::uint32_t _wakeupBaud{0};
    std::uint8_t _nextXid{1};

    Bytes _assembling;
    std::uint32_t _assemblingSize{0};
    bool _assemblingActive{false};
    std::uint8_t _lastAckedXid{0};
    Bytes _lastAckedPayload;
    std::deque<Bytes> _received;
};

} // namespace fujinet::hotsync
