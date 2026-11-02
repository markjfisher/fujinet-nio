#pragma once

#include "fujinet/hotsync/bytes.h"

#include <cstdint>

namespace fujinet::hotsync {

// Carries whole DLP messages. PADP (serial) and NetSync (TCP) implement it.
class DlpTransport {
public:
    virtual ~DlpTransport() = default;

    // Runs the transport-level handshake that precedes the first DLP request.
    virtual bool accept() = 0;
    virtual bool send(const Bytes& message) = 0;
    virtual bool receive(Bytes& message, std::uint32_t timeoutMs) = 0;
};

} // namespace fujinet::hotsync
