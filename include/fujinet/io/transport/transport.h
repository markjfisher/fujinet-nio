#pragma once

#include "fujinet/io/core/io_message.h"

namespace fujinet::io {

// Forward declaration
class IORequest;
class IOResponse;

// Abstract transport: turns bytes on a Channel into IORequests/IOResponses.
class ITransport {
public:
    virtual ~ITransport() = default;

    // Called every loop iteration so the transport can do background work
    // (e.g. timeouts, internal state machines). Default: no-op.
    virtual void poll() {}

    // Try to read and parse one complete request from this transport.
    // Returns true if a full request was produced and stored in outReq.
    // Returns false if no complete request is available right now.
    virtual bool receive(IORequest& outReq) = 0;

    // Send a response back over this transport.
    virtual void send(const IOResponse& resp) = 0;
};

} // namespace fujinet::io
