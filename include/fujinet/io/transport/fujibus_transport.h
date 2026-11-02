#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/transport/bus_line_coordinator.h"
#include "fujinet/io/transport/iframer.h"
#include "fujinet/io/transport/transport.h"

namespace fujinet::io {

// Skeleton FujiBus transport.
//
// With a line coordinator (a line shared with a Palm cradle), it reports the
// bytes, frames and requests it sees, and offers the line at its idle point
// (io::IBusLineCoordinator).
class FujiBusTransport : public ITransport {
public:
    FujiBusTransport(Channel& channel, IFramer& framer, IBusLineCoordinator* lineCoordinator = nullptr)
        : _channel(channel)
        , _framer(framer)
        , _lineCoordinator(lineCoordinator)
        , _nextRequestId(1)
    {}

    void poll() override;
    bool supports_work_wait() const override;
    bool wait_for_work(std::chrono::milliseconds timeout) override;

    bool receive(IORequest& outReq) override;
    void send(const IOResponse& resp) override;

    // Optional: parse an inbound packet as a response (status in param[0]).
    // Not used by IOService today, but useful for host-side or test harnesses.
    bool receiveResponse(IOResponse& outResp);

private:
    Channel&   _channel;
    IFramer&   _framer;
    IBusLineCoordinator* _lineCoordinator;
    RequestID  _nextRequestId;
};

} // namespace fujinet::io
