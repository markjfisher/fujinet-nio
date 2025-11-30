#include "fujinet/io/transport/rs232_transport.h"

#include <iostream> // only for debug output in this stub

namespace fujinet::io {

void Rs232Transport::poll()
{
    std::uint8_t temp[256];

    while (_channel.available()) {
        std::size_t n = _channel.read(temp, sizeof(temp));
        if (n == 0) break;
        _rxBuffer.insert(_rxBuffer.end(), temp, temp + n);
    }

    // Later: partial parsing, etc.
}

bool Rs232Transport::receive(IORequest& outReq)
{
    // Stub implementation:
    // - In a real system, you would:
    //    * read from _channel into _rxBuffer
    //    * check if a full frame/command is available
    //    * parse it into IORequest
    //    * set outReq.id, outReq.deviceId, outReq.type, outReq.command, outReq.payload
    //    * increment _nextRequestId
    //
    // For now, we just say "no request available".
    (void)outReq; // avoid unused parameter warning
    return false;
}

void Rs232Transport::send(const IOResponse& resp)
{
    // Stub implementation:
    // Encode the response in some trivial format and write to the channel.
    // For now, we'll just write a simple debug line to std::cout and no
    // actual bytes to the channel.
    std::cout << "[Rs232Transport] send: deviceId="
              << static_cast<int>(resp.deviceId)
              << " status=" << static_cast<int>(resp.status)
              << " payloadSize=" << resp.payload.size()
              << std::endl;

    // In a real implementation, you would serialize resp into bytes and
    // call _channel.write(...).
}

} // namespace fujinet::io
