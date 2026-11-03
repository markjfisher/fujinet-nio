#include "fujinet/io/atari_sio_nio_frontend.h"

#include "fujinet/io/protocol/fujibus_packet_codec.h"

namespace fujinet::io {
namespace {

constexpr std::uint8_t command_query = 'Q';
constexpr std::uint8_t command_write = 'W';
constexpr std::uint8_t command_poll = 'P';
constexpr std::uint8_t command_read = 'R';
constexpr std::uint8_t command_hsio_index = 0x3f;

constexpr std::uint8_t wire_revision = 1;
// Feature flags in the Q reply. Bit 0: R may ask for more than the response's length (up to the
// capacity); the reply is the response padded with zeros, and the response is released as for an exact R.
// A client that can bound the length of a response (a sector read: the sector size plus the packet
// framing) reads it straight after the W, without the P that exists only to ask for the length. The
// packet's own header gives its true length. A shorter R is refused and keeps the response, as before.
constexpr std::uint8_t feature_any_read_length = 0x01;
// Bit 1: the FujiNet pulses PROC when a network session becomes readable (a read that was NotReady would
// now succeed). Only set while the setting is on; a client that enables the PROC interrupt can then wait
// for the pulse instead of repeating the read, and polling stays correct without it.
constexpr std::uint8_t feature_proc_notify = 0x02;

} // namespace

AtariSioNioFrontend::~AtariSioNioFrontend()
{
    reset_host();
}

std::uint8_t AtariSioNioFrontend::checksum(const std::uint8_t* data, std::size_t len)
{
    std::uint8_t result = 0;
    for (std::size_t i = 0; data && i < len; ++i) {
        const std::uint16_t sum = static_cast<std::uint16_t>(result) + data[i];
        result = static_cast<std::uint8_t>((sum & 0xffU) + (sum >> 8U));
    }
    return result;
}

AtariSioNioFrontend::Result AtariSioNioFrontend::reject() const
{
    return Result{{nak}, 0, false};
}

AtariSioNioFrontend::Result AtariSioNioFrontend::data_reply(const std::vector<std::uint8_t>& data) const
{
    std::vector<std::uint8_t> bytes{ack, complete};
    bytes.insert(bytes.end(), data.begin(), data.end());
    bytes.push_back(checksum(data.data(), data.size()));
    return Result{std::move(bytes), 0, true};
}

std::uint16_t AtariSioNioFrontend::aux(const std::array<std::uint8_t, command_bytes>& command) const
{
    return static_cast<std::uint16_t>(command[2]) |
           static_cast<std::uint16_t>(command[3] << 8U);
}

std::vector<std::uint8_t> AtariSioNioFrontend::bounded_error_response(const IORequest& request) const
{
    IOResponse error = make_base_response(request, StatusCode::InternalError);
    return protocol::encode_fujibus_response(error);
}

AtariSioNioFrontend::Result AtariSioNioFrontend::begin(
    const std::array<std::uint8_t, command_bytes>& command)
{
    if (command[0] != device_id || checksum(command.data(), command.size() - 1) != command.back()) {
        return reject();
    }

    const auto length = aux(command);
    switch (command[1]) {
    case 's':
        if(length || _state==State::Receiving) return reject();
        if(!_stream) _stream=std::make_unique<stream::Session>(_handler,this);
        return data_reply(_stream->query());
    case 'S':
        drop_stale_reply();
        if(!_stream || _state!=State::Idle || length<stream::Session::header ||
           length>stream::Session::header+stream::Session::max_data) return reject();
        _stream_write=true; _state=State::Receiving; _write_bytes=length;
        return Result{{ack},length,true};
    case 'T':
        if(_stream_reply.empty() || length<_stream_reply.size() ||
           length>stream::Session::header+stream::Session::max_data) return reject();
        { auto reply=_stream_reply; reply.resize(length,0); return data_reply(reply); }

    case command_hsio_index: {
        // Stateless: it does not touch a transaction in progress. Off: NAK, as for any unknown command.
        if (_hsio_index < 0) return reject();
        const auto index = static_cast<std::uint8_t>(_hsio_index);
        Result result = data_reply({index});
        result.high_speed = true;
        return result;
    }

    case command_query:
        drop_stale_reply();
        if (_state != State::Idle || length != 0) return reject();
        return data_reply({wire_revision,
                           static_cast<std::uint8_t>(capacity & 0xffU),
                           static_cast<std::uint8_t>(capacity >> 8U),
                           static_cast<std::uint8_t>(feature_any_read_length | (_proc_notify ? feature_proc_notify : 0))});

    case command_write:
        drop_stale_reply();
        if (_state != State::Idle || length == 0 || length > capacity) return reject();
        _response.clear();
        _last_read_length = 0;
        _state = State::Receiving;
        _write_bytes = length;
        return Result{{ack}, _write_bytes, true};

    case command_poll: {
        if (length != 0) return reject();
        std::vector<std::uint8_t> state(3, 0);
        if (_state == State::Receiving) {
            state[0] = 1;
        } else if (_state == State::Ready) {
            state[0] = 2;
            state[1] = static_cast<std::uint8_t>(_response.size() & 0xffU);
            state[2] = static_cast<std::uint8_t>(_response.size() >> 8U);
        }
        return data_reply(state);
    }

    case command_read: {
        // SIOV may retry R if its reply or checksum was lost. Replay only the
        // exact last R, until the next W begins a different transaction.
        if (_state == State::Idle && _last_read_length && length == _last_read_length) {
            std::vector<std::uint8_t> reply = _response;
            reply.resize(length, 0);
            return data_reply(reply);
        }
        if (_state != State::Ready || length < _response.size() || length > capacity) return reject();
        // The reply owns its copy before the transaction slot is consumed.  An
        // ingress may transmit Result::bytes after this function returns.
        std::vector<std::uint8_t> reply = _response;
        reply.resize(length, 0);
        _last_read_length = length;
        _state = State::Idle;
        return data_reply(reply);
    }

    default:
        return reject();
    }
}

AtariSioNioFrontend::Result AtariSioNioFrontend::complete_write(const std::uint8_t* data,
                                                                  std::size_t data_bytes)
{
    if (_state != State::Receiving || !data || data_bytes != _write_bytes + 1 ||
        checksum(data, _write_bytes) != data[_write_bytes]) {
        reset();
        return reject();
    }

    if(_stream_write) {
        const bool early=static_cast<bool>(_early_ack);
        if(early) _early_ack();
        _stream_reply=_stream->execute({data,data+_write_bytes});
        _stream_write=false; _state=State::Idle; _write_bytes=0;
        return early ? Result{{complete},0,true} : Result{{ack,complete},0,true};
    }
    protocol::ByteBuffer raw(data, data + _write_bytes);
    IORequest request;
    if (!protocol::decode_fujibus_request(raw, _next_request_id, request)) {
        reset();
        return reject();
    }
    ++_next_request_id;
    request.clientOwner = this;

    // The frame is good and the request decoded: from here the Atari has been given what it waits a few
    // tens of milliseconds for, so acknowledge it before the request runs (it may take a long time).
    const bool acked_early = static_cast<bool>(_early_ack);
    if (acked_early) _early_ack();

    std::vector<std::uint8_t> response =
        protocol::encode_fujibus_response(_handler.handleRequest(request));
    if (response.empty() || response.size() > capacity) {
        response = bounded_error_response(request);
    }
    if (response.empty() || response.size() > capacity) {
        reset();
        // After an ACK the only refusal left is the error byte; before one it is NAK.
        return acked_early ? Result{{error}, 0, true} : reject();
    }

    _write_bytes = 0;
    _response = std::move(response);
    _state = State::Ready;
    _ready_at_us = _clock ? _clock() : 0;
    if (acked_early) return Result{{complete}, 0, true};
    return Result{{ack, complete}, 0, true};
}

void AtariSioNioFrontend::drop_stale_reply()
{
    if (_state != State::Ready || !_clock) return;
    if (_clock() - _ready_at_us < stale_reply_us) return;
    reset();
    ++_stale_dropped;
}

void AtariSioNioFrontend::abort_write()
{
    if (_state == State::Receiving) reset();
}

void AtariSioNioFrontend::reset()
{
    _state = State::Idle;
    _stream_write = false;
    _write_bytes = 0;
    _response.clear();
    _last_read_length = 0;
}

void AtariSioNioFrontend::reset_host()
{
    reset();
    _stream.reset();
    _stream_reply.clear();
    _handler.resetClient(protocol::to_device_id(protocol::WireDeviceId::NetworkService), this);
}

} // namespace fujinet::io
