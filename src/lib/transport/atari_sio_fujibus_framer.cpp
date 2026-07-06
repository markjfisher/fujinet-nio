#include "fujinet/io/transport/atari_sio_fujibus_framer.h"

#include <algorithm>

namespace fujinet::io::transport {

void AtariSioFujiBusFramer::ingest(const std::uint8_t* data, std::size_t len)
{
    if (!data || len == 0) {
        return;
    }

    for (std::size_t i = 0; i < len; ++i) {
        const std::uint8_t value = data[i];

        switch (_state) {
        case State::Command:
            _command.push_back(value);
            if (_command.size() == 5) {
                process_command();
            }
            break;

        case State::WritePayload:
            _writePayload.push_back(value);
            if (_writeRemaining > 0) {
                --_writeRemaining;
            }
            if (_writeRemaining == 0) {
                _state = State::WriteChecksum;
            }
            break;

        case State::WriteChecksum:
            process_write_checksum(value);
            break;
        }
    }
}

void AtariSioFujiBusFramer::queue_response(const std::uint8_t* data, std::size_t len)
{
    if (data && len > 0) {
        _response.insert(_response.end(), data, data + len);
    }

    if (_pendingReadSize != 0 && !_response.empty()) {
        send_read_response();
    }
}

std::size_t AtariSioFujiBusFramer::read_request(std::uint8_t* buffer, std::size_t maxLen)
{
    if (!buffer || maxLen == 0 || _request.empty()) {
        return 0;
    }

    const std::size_t n = std::min(maxLen, _request.size());
    std::copy_n(_request.begin(), n, buffer);
    _request.erase(_request.begin(), _request.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
}

std::size_t AtariSioFujiBusFramer::read_output(std::uint8_t* buffer, std::size_t maxLen)
{
    if (!buffer || maxLen == 0 || _output.empty()) {
        return 0;
    }

    const std::size_t n = std::min(maxLen, _output.size());
    std::copy_n(_output.begin(), n, buffer);
    _output.erase(_output.begin(), _output.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
}

std::uint8_t AtariSioFujiBusFramer::checksum_update(std::uint8_t checksum, std::uint8_t value)
{
    const std::uint16_t sum = static_cast<std::uint16_t>(checksum) + value;
    return static_cast<std::uint8_t>((sum & 0xFFU) + (sum >> 8U));
}

std::uint8_t AtariSioFujiBusFramer::checksum(const std::uint8_t* data, std::size_t len)
{
    std::uint8_t result = 0;
    if (!data) {
        return result;
    }
    for (std::size_t i = 0; i < len; ++i) {
        result = checksum_update(result, data[i]);
    }
    return result;
}

void AtariSioFujiBusFramer::reset_command()
{
    _state = State::Command;
    _command.clear();
    _writePayload.clear();
    _writeRemaining = 0;
}

std::uint16_t AtariSioFujiBusFramer::aux_word() const
{
    if (_command.size() < 4) {
        return 0;
    }
    return static_cast<std::uint16_t>(_command[2]) |
           static_cast<std::uint16_t>(_command[3] << 8);
}

void AtariSioFujiBusFramer::process_command()
{
    if (_command.size() != 5) {
        reset_command();
        return;
    }

    const std::uint8_t got = _command[4];
    if (_command[0] != DeviceId) {
        reset_command();
        return;
    }

    const std::uint8_t expected = checksum(_command.data(), 4);
    if (got != expected) {
        push_output(SioNak);
        reset_command();
        return;
    }

    const std::uint8_t command = _command[1];
    const std::uint16_t aux = aux_word();

    if (command == CommandWrite) {
        push_output(SioAck);
        _writePayload.clear();
        _writePayload.reserve(aux);
        _writeRemaining = aux;
        _state = aux == 0 ? State::WriteChecksum : State::WritePayload;
        _command.clear();
        return;
    }

    if (command == CommandRead) {
        push_output(SioAck);
        _pendingReadSize = aux == 0 ? DefaultReadSize : aux;
        _command.clear();
        _state = State::Command;
        if (!_response.empty()) {
            send_read_response();
        }
        return;
    }

    push_output(SioNak);
    reset_command();
}

void AtariSioFujiBusFramer::process_write_checksum(std::uint8_t got)
{
    const std::uint8_t expected = checksum(_writePayload.data(), _writePayload.size());
    if (got != expected) {
        push_output(SioNak);
        reset_command();
        return;
    }

    push_output(SioAck);
    push_output(SioComplete);
    _request.insert(_request.end(), _writePayload.begin(), _writePayload.end());
    reset_command();
}

void AtariSioFujiBusFramer::send_read_response()
{
    const std::size_t transferSize = _pendingReadSize == 0 ? DefaultReadSize : _pendingReadSize;
    if (transferSize == 0 || _response.empty()) {
        return;
    }

    push_output(SioComplete);
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < transferSize; ++i) {
        const std::uint8_t value = i < _response.size() ? _response[i] : 0;
        push_output(value);
        sum = checksum_update(sum, value);
    }
    push_output(sum);

    if (_response.size() <= transferSize) {
        _response.clear();
    } else {
        _response.erase(
            _response.begin(),
            _response.begin() + static_cast<std::ptrdiff_t>(transferSize));
    }
    _pendingReadSize = 0;
}

} // namespace fujinet::io::transport
