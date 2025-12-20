#include "fujinet/platform/posix/tcp_network_protocol_posix.h"
#include "fujinet/net/tcp_network_protocol_common.h"
#include "fujinet/platform/posix/tcp_socket_ops_posix.h"

#include <netdb.h>
#include <sys/socket.h>

namespace fujinet::platform::posix {

TcpNetworkProtocolPosix::TcpNetworkProtocolPosix()
    : _common(fujinet::net::get_posix_socket_ops())
{
}

TcpNetworkProtocolPosix::~TcpNetworkProtocolPosix() = default;

fujinet::io::StatusCode TcpNetworkProtocolPosix::open(const fujinet::io::NetworkOpenRequest& req)
{
    return _common.open(req, ::getaddrinfo, ::freeaddrinfo);
}

fujinet::io::StatusCode TcpNetworkProtocolPosix::write_body(std::uint32_t offset,
                                                             const std::uint8_t* data,
                                                             std::size_t len,
                                                             std::uint16_t& written)
{
    return _common.write_body(offset, data, len, written);
}

fujinet::io::StatusCode TcpNetworkProtocolPosix::read_body(std::uint32_t offset,
                                                            std::uint8_t* out,
                                                            std::size_t outLen,
                                                            std::uint16_t& read,
                                                            bool& eof)
{
    return _common.read_body(offset, out, outLen, read, eof);
}

fujinet::io::StatusCode TcpNetworkProtocolPosix::info(std::size_t maxHeaderBytes,
                                                       fujinet::io::NetworkInfo& out)
{
    return _common.info(maxHeaderBytes, out);
}

void TcpNetworkProtocolPosix::poll()
{
    _common.poll();
}

void TcpNetworkProtocolPosix::close()
{
    _common.close();
}

} // namespace fujinet::platform::posix
