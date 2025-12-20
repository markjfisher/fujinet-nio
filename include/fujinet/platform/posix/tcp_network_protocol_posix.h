#pragma once

#include "fujinet/io/devices/network_protocol.h"
#include "fujinet/net/tcp_network_protocol_common.h"

#include <cstdint>
#include <cstddef>

namespace fujinet::platform::posix {

// TCP stream backend for NetworkDevice v1.
// - nonblocking connect/send/recv
// - sequential offsets are enforced as stream cursors
// - Info() exposes connection state via pseudo headers in headersBlock
class TcpNetworkProtocolPosix final : public fujinet::io::INetworkProtocol {
public:
    TcpNetworkProtocolPosix();
    ~TcpNetworkProtocolPosix() override;

    // INetworkProtocol
    fujinet::io::StatusCode open(const fujinet::io::NetworkOpenRequest& req) override;
    fujinet::io::StatusCode write_body(std::uint32_t offset,
                                       const std::uint8_t* data,
                                       std::size_t len,
                                       std::uint16_t& written) override;
    fujinet::io::StatusCode read_body(std::uint32_t offset,
                                      std::uint8_t* out,
                                      std::size_t outLen,
                                      std::uint16_t& read,
                                      bool& eof) override;
    fujinet::io::StatusCode info(std::size_t maxHeaderBytes,
                                 fujinet::io::NetworkInfo& out) override;
    void poll() override;
    void close() override;

private:
    fujinet::net::TcpNetworkProtocolCommon _common;
};

} // namespace fujinet::platform::posix
