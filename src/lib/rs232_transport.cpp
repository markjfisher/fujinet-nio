#include "fujinet/io/transport/rs232_transport.h"

#include <iostream>   // temporary for debug
#include <algorithm>  // for std::find

namespace fujinet::io {

void Rs232Transport::poll()
{
    std::uint8_t temp[256];

    while (_channel.available()) {
        std::size_t n = _channel.read(temp, sizeof(temp));
        if (n == 0) {
            break;
        }
        _rxBuffer.insert(_rxBuffer.end(), temp, temp + n);
    }

    // In a SLIP-style setup, you might also drive a state machine here.
}

// For now, we use a very simple "one line = one request" framing:
//
//  - Bytes are accumulated into _rxBuffer by poll().
//  - receive() looks for '\n' (LF).
//  - Everything before the newline is one frame.
//  - If the frame ends with '\r', we strip it (CRLF support).
//
// Later, we'll replace this with SLIP framing while keeping the overall shape:
//   poll()  -> gather bytes
//   receive() -> look for complete frame, parse into IORequest
bool Rs232Transport::receive(IORequest& outReq)
{
    // Look for LF as a frame terminator.
    auto it = std::find_if(
        _rxBuffer.begin(),
        _rxBuffer.end(),
        [](std::uint8_t b) { return b == '\n' || b == '\r'; }
    );
    
    if (it == _rxBuffer.end()) {
        // No complete frame yet.
        return false;
    }

    std::size_t endIndex = static_cast<std::size_t>(it - _rxBuffer.begin());

    // Frame is [_rxBuffer[0], ..., _rxBuffer[endIndex-1]].
    // Handle optional '\r' before '\n' (CRLF).
    std::size_t frameLen = endIndex;
    if (frameLen > 0 && _rxBuffer[frameLen - 1] == static_cast<std::uint8_t>('\r')) {
        --frameLen;
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(frameLen);
    payload.insert(payload.end(), _rxBuffer.begin(), _rxBuffer.begin() + frameLen);

    // Erase consumed bytes including the terminator '\n'.
    _rxBuffer.erase(_rxBuffer.begin(), _rxBuffer.begin() + endIndex + 1);

    // For now, we hard-code:
    //   - deviceId = 1 (DummyDevice)
    //   - type = Command
    //   - command = 0
    // Later, SLIP framing and a real header can carry these.
    outReq.id       = _nextRequestId++;
    outReq.deviceId = 1;
    outReq.type     = RequestType::Command;
    outReq.command  = 0;
    outReq.payload  = std::move(payload);

    // Debug print so you can see the requests being detected.
    std::cout << "[Rs232Transport] receive: id=" << outReq.id
              << " deviceId=" << static_cast<int>(outReq.deviceId)
              << " payloadSize=" << outReq.payload.size()
              << std::endl;

    return true;
}

void Rs232Transport::send(const IOResponse& resp)
{
    // Log to stdout for debugging.
    std::cout << "[Rs232Transport] send: deviceId="
              << static_cast<int>(resp.deviceId)
              << " status=" << static_cast<int>(resp.status)
              << " payloadSize=" << resp.payload.size()
              << std::endl;

    // For now, we use a simple "line" framing:
    //   [payload bytes] '\n'
    //
    // This matches the simple line-based receive() and works well with
    // terminals and minicom. Later, SLIP framing will replace this.
    std::vector<std::uint8_t> frame;
    frame.reserve(resp.payload.size() + 1);

    frame.insert(frame.end(), resp.payload.begin(), resp.payload.end());
    frame.push_back(static_cast<std::uint8_t>('\n'));

    if (!frame.empty()) {
        _channel.write(frame.data(), frame.size());
    }
}


} // namespace fujinet::io
