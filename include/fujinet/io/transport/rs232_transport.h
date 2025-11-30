#pragma once

#include <vector>

#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/transport/transport.h"

namespace fujinet::io {

// Skeleton RS232-based transport.
//
// For now, this does nothing in receive() (always returns false) and
// writes a simple placeholder in send(). We'll flesh out the real
// framing/decoding later.
class Rs232Transport : public ITransport {
public:
    explicit Rs232Transport(Channel& channel)
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
