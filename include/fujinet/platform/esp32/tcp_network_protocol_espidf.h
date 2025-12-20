#pragma once

#include "fujinet/io/devices/network_protocol.h"
#include "fujinet/net/tcp_network_protocol_common.h"

#include <cstdint>
#include <cstddef>

namespace fujinet::platform::esp32 {

// TCP stream backend using ESP-IDF / lwIP BSD sockets.
// Same contract as POSIX implementation.
class TcpNetworkProtocolEspIdf final : public fujinet::io::INetworkProtocol {
public:
    TcpNetworkProtocolEspIdf();
    ~TcpNetworkProtocolEspIdf() override;

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

} // namespace fujinet::platform::esp32
