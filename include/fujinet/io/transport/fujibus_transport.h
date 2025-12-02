#pragma once

#include <vector>

#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/transport/transport.h"

namespace fujinet::io {

// Skeleton FujiBus transport.
//
class FujiBusTransport : public ITransport {
public:
    explicit FujiBusTransport(Channel& channel)
        : _channel(channel)
        , _nextRequestId(1)
    {}

    void poll() override;

    bool receive(IORequest& outReq) override;
    void send(const IOResponse& resp) override;

private:
    Channel&                _channel;
    std::vector<std::uint8_t> _rxBuffer;
    RequestID               _nextRequestId;
};

} // namespace fujinet::io
