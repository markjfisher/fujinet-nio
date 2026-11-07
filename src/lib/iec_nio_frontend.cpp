#include "fujinet/io/iec_nio_frontend.h"

#include "fujinet/io/protocol/fujibus_packet_codec.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <algorithm>
#include <cstring>

namespace fujinet::io {

IecNioFrontend::~IecNioFrontend()
{
    bus_reset();
}

void IecNioFrontend::open(std::uint8_t, const std::uint8_t*, std::size_t)
{
    // Packets need no OPEN: the channel is always there.
}

void IecNioFrontend::close(std::uint8_t)
{
}

void IecNioFrontend::listen(std::uint8_t channel)
{
    _receiving = channel == packet_channel;
    _overflow = false;
    _request.clear();
}

std::size_t IecNioFrontend::receive(const std::uint8_t* data, std::size_t length, bool)
{
    if (!_receiving || !data) {
        return 0;
    }
    if (_overflow || _request.size() + length > capacity) {
        _overflow = true;
        return 0;
    }
    _request.insert(_request.end(), data, data + length);
    return length;
}

void IecNioFrontend::unlisten()
{
    if (!_receiving) {
        return;
    }
    _receiving = false;
    // Addressing the channel with no data is how a client checks the device is
    // there; it is not a request, and leaves any response waiting.
    if (_request.empty() && !_overflow) {
        return;
    }
    _response.clear();
    _responsePos = 0;
    if (!_overflow) {
        run_request();
    }
    _request.clear();
}

void IecNioFrontend::run_request()
{
    IORequest request;
    if (!protocol::decode_fujibus_request(_request, _nextRequestId, request)) {
        return; // no response: the client's read ends at once
    }
    ++_nextRequestId;
    request.clientOwner = this;

    std::vector<std::uint8_t> response =
        protocol::encode_fujibus_response(_handler.handleRequest(request));
    if (response.empty() || response.size() > capacity) {
        response = protocol::encode_fujibus_response(
            make_base_response(request, StatusCode::InternalError));
    }
    if (response.size() <= capacity) {
        _response = std::move(response);
    }
}

void IecNioFrontend::talk(std::uint8_t channel)
{
    _sending = channel == packet_channel;
    // Each TALK reads the response from its start, so a client can read it again
    // if it lost the first attempt.
    _responsePos = 0;
}

IecReadResult IecNioFrontend::send(std::uint8_t* dst, std::size_t maxBytes)
{
    if (!_sending || !dst) {
        return IecReadResult{0, true};
    }
    const std::size_t left = _response.size() - _responsePos;
    const std::size_t n = std::min(left, maxBytes);
    if (n > 0) {
        std::memcpy(dst, _response.data() + _responsePos, n);
    }
    _responsePos += n;
    return IecReadResult{n, _responsePos >= _response.size()};
}

void IecNioFrontend::untalk()
{
    _sending = false;
}

void IecNioFrontend::bus_reset()
{
    _receiving = false;
    _overflow = false;
    _sending = false;
    _request.clear();
    _response.clear();
    _responsePos = 0;
    // A reset abandons the program's network sessions.
    _handler.resetClient(protocol::to_device_id(protocol::WireDeviceId::NetworkService), this);
}

} // namespace fujinet::io
