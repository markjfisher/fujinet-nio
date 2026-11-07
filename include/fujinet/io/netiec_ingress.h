#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/work_source.h"
#include "fujinet/io/iec_bus.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fujinet::io {

// NetIEC: IEC bus transactions from an emulator's bus master, over a byte stream.
// Every frame is [command u8][payload length u16le][payload]; each request gets
// exactly one reply, whose command is the request's with bit 7 set.
// docs/iec/netiec.md is the protocol.
class NetIecServer {
public:
    static constexpr std::uint8_t version = 1;

    NetIecServer(IecBus& bus, std::string name) : _bus(bus), _name(std::move(name)) {}

    // Takes bytes as they arrive, in any pieces, and appends a reply to `out` for
    // each request they complete.
    void receive(const std::uint8_t* data, std::size_t length, std::vector<std::uint8_t>& out);

    // The connection changed: forget a request received in part. Device state and
    // the bus's addressing are left to the new connection's HELLO.
    void drop_partial_request() { _pending.clear(); }

private:
    void handle(std::uint8_t command, const std::uint8_t* payload, std::size_t length,
                std::vector<std::uint8_t>& out);

    IecBus& _bus;
    std::string _name;
    std::vector<std::uint8_t> _pending;
    std::vector<std::uint8_t> _reply;
};

// Runs a NetIecServer over a Channel: the POSIX TCP listener an emulator connects
// to. Bus work happens on the core loop that calls poll().
class NetIecIngress final : public IWorkSource {
public:
    NetIecIngress(std::unique_ptr<Channel> channel, IecBus& bus, std::string name);

    void poll();
    bool supports_work_wait() const override;
    bool wait_for_work(std::chrono::milliseconds timeout) override;

private:
    std::unique_ptr<Channel> _channel;
    std::uint32_t _epoch{0};
    NetIecServer _server;
    std::vector<std::uint8_t> _out;
};

} // namespace fujinet::io
