#pragma once

#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/iec_bus.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fujinet::io {

// The packet channel between a Commodore program and FujiNet NIO: one IEC device
// carrying whole raw FujiBus packets. docs/iec/nio-channel.md is the contract. It
// owns no bus lines, sockets or disks, and has nothing to do with drives.
class IecNioFrontend final : public IIecDevice {
public:
    static constexpr std::uint8_t default_device = 30;
    static constexpr std::uint8_t packet_channel = 0;
    static constexpr std::size_t capacity = 512;

    explicit IecNioFrontend(IRequestHandler& handler) : _handler(handler) {}
    ~IecNioFrontend() override;
    IecNioFrontend(const IecNioFrontend&) = delete;
    IecNioFrontend& operator=(const IecNioFrontend&) = delete;

    void open(std::uint8_t channel, const std::uint8_t* name, std::size_t nameLength) override;
    void close(std::uint8_t channel) override;
    void listen(std::uint8_t channel) override;
    std::size_t receive(const std::uint8_t* data, std::size_t length, bool eoi) override;
    void unlisten() override;
    void talk(std::uint8_t channel) override;
    IecReadResult send(std::uint8_t* dst, std::size_t maxBytes) override;
    void untalk() override;
    void bus_reset() override;

private:
    void run_request();

    IRequestHandler& _handler;
    RequestID _nextRequestId{1};
    bool _receiving{false};
    bool _overflow{false};
    bool _sending{false};
    std::vector<std::uint8_t> _request;
    std::vector<std::uint8_t> _response;
    std::size_t _responsePos{0};
};

} // namespace fujinet::io
