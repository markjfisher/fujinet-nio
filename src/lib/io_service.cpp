#include "fujinet/io/transport/io_service.h"

namespace fujinet::io {

void IOService::serviceOnce()
{
    // Let each transport do any internal background work.
    for (auto* t : _transports) {
        if (t) {
            t->poll();
        }
    }

    // Process all available requests on each transport.
    for (auto* t : _transports) {
        if (!t) {
            continue;
        }

        IORequest req;
        while (t->receive(req)) {
            IOResponse resp = _handler.handleRequest(req);
            t->send(resp);
        }
    }
}

} // namespace fujinet::io
