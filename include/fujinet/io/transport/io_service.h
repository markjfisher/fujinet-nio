#pragma once

#include <vector>

#include "fujinet/io/core/io_message.h"
#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/transport/transport.h"

namespace fujinet::io {

// Owns a set of transports and pumps IORequests through an IRequestHandler.
class IOService {
public:
    explicit IOService(IRequestHandler& handler)
        : _handler(handler)
    {}

    // We don't own the transports; lifetime is managed externally.
    void addTransport(ITransport* transport) {
        if (transport) {
            _transports.push_back(transport);
        }
    }

    // One "tick" of the service loop.
    // - Let transports poll
    // - Pull all available requests
    // - Route them through the handler
    // - Send responses back via the same transport
    void serviceOnce();

private:
    IRequestHandler&              _handler;
    std::vector<ITransport*>      _transports;
};

} // namespace fujinet::io
