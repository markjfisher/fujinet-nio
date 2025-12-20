#include "fujinet/platform/esp32/tcp_network_protocol_espidf.h"
#include "fujinet/net/tcp_network_protocol_common.h"
#include "fujinet/platform/esp32/tcp_socket_ops_espidf.h"

extern "C" {
#include "lwip/netdb.h"    // lwip_getaddrinfo, lwip_freeaddrinfo
}

namespace fujinet::platform::esp32 {

TcpNetworkProtocolEspIdf::TcpNetworkProtocolEspIdf()
    : _common(fujinet::net::get_espidf_socket_ops())
{
}

TcpNetworkProtocolEspIdf::~TcpNetworkProtocolEspIdf() = default;

fujinet::io::StatusCode TcpNetworkProtocolEspIdf::open(const fujinet::io::NetworkOpenRequest& req)
{
    return _common.open(req, lwip_getaddrinfo, lwip_freeaddrinfo);
}

fujinet::io::StatusCode TcpNetworkProtocolEspIdf::write_body(std::uint32_t offset,
                                                               const std::uint8_t* data,
                                                               std::size_t len,
                                                               std::uint16_t& written)
{
    return _common.write_body(offset, data, len, written);
}

fujinet::io::StatusCode TcpNetworkProtocolEspIdf::read_body(std::uint32_t offset,
                                                             std::uint8_t* out,
                                                             std::size_t outLen,
                                                             std::uint16_t& read,
                                                             bool& eof)
{
    return _common.read_body(offset, out, outLen, read, eof);
}

fujinet::io::StatusCode TcpNetworkProtocolEspIdf::info(std::size_t maxHeaderBytes,
                                                         fujinet::io::NetworkInfo& out)
{
    return _common.info(maxHeaderBytes, out);
}

void TcpNetworkProtocolEspIdf::poll()
{
    _common.poll();
}

void TcpNetworkProtocolEspIdf::close()
{
    _common.close();
}

} // namespace fujinet::platform::esp32
