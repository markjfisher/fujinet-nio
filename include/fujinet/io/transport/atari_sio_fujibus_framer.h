#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fujinet::io::transport {

class AtariSioFujiBusFramer {
public:
    static constexpr std::uint8_t DeviceId = 0x7F;
    static constexpr std::uint8_t CommandWrite = 'W';
    static constexpr std::uint8_t CommandRead = 'R';
    static constexpr std::uint8_t SioAck = 'A';
    static constexpr std::uint8_t SioNak = 'N';
    static constexpr std::uint8_t SioComplete = 'C';
    static constexpr std::uint16_t DefaultReadSize = 128;

    void ingest(const std::uint8_t* data, std::size_t len);
    void queue_response(const std::uint8_t* data, std::size_t len);

    bool has_request() const noexcept { return !_request.empty(); }
    bool has_output() const noexcept { return !_output.empty(); }

    std::size_t read_request(std::uint8_t* buffer, std::size_t maxLen);
    std::size_t read_output(std::uint8_t* buffer, std::size_t maxLen);

    static std::uint8_t checksum_update(std::uint8_t checksum, std::uint8_t value);
    static std::uint8_t checksum(const std::uint8_t* data, std::size_t len);

private:
    enum class State {
        Command,
        WritePayload,
        WriteChecksum,
    };

    void reset_command();
    void process_command();
    void process_write_checksum(std::uint8_t got);
    void send_read_response();
    void push_output(std::uint8_t value) { _output.push_back(value); }
    std::uint16_t aux_word() const;

    State _state{State::Command};
    std::vector<std::uint8_t> _command;
    std::vector<std::uint8_t> _writePayload;
    std::vector<std::uint8_t> _request;
    std::vector<std::uint8_t> _response;
    std::vector<std::uint8_t> _output;
    std::uint16_t _writeRemaining{0};
    std::uint16_t _pendingReadSize{0};
};

} // namespace fujinet::io::transport
