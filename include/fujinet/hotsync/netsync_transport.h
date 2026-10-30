#pragma once

// Network HotSync framing (TCP port 14238): a 6-byte header per DLP message
// and a fixed three-step handshake.
// Reference: palm-sync src/protocols/net-sync-protocol.ts and sync-connections.ts

#include "fujinet/hotsync/dlp_transport.h"
#include "fujinet/hotsync/link.h"

#include <cstddef>
#include <cstdint>

namespace fujinet::hotsync {

inline constexpr std::uint16_t kNetSyncPort = 14238;

class NetSyncTransport : public DlpTransport {
public:
    explicit NetSyncTransport(Link& link) : _link(link) {}

    bool accept() override;
    bool send(const Bytes& message) override;
    bool receive(Bytes& message, std::uint32_t timeoutMs) override;

private:
    bool read_exact(std::uint8_t* buf, std::size_t len, std::uint32_t timeoutMs);
    bool expect(std::size_t length);

    Link& _link;
    std::uint8_t _xid{0};
};

} // namespace fujinet::hotsync
