#include "fujinet/platform/network_registry.h"
#include "fujinet/io/devices/network_protocol_stub.h"

#include "fujinet/platform/posix/tcp_network_protocol_posix.h"
#include "fujinet/platform/posix/tls_network_protocol_posix.h"

#if FN_WITH_CURL == 1
#include "fujinet/platform/posix/http_network_protocol_curl.h"
#endif

namespace fujinet::platform {

io::ProtocolRegistry make_default_network_registry()
{
    io::ProtocolRegistry r;

    // TCP stream sockets (POSIX)
    r.register_scheme("tcp", [] {
        return std::make_unique<posix::TcpNetworkProtocolPosix>();
    });

    // TLS over TCP (secure sockets using OpenSSL)
    r.register_scheme("tls", [] {
        return std::make_unique<posix::TlsNetworkProtocolPosix>();
    });

#if FN_WITH_CURL == 1
    r.register_scheme("http", [] { return std::make_unique<posix::HttpNetworkProtocolCurl>(); });
    r.register_scheme("https", [] { return std::make_unique<posix::HttpNetworkProtocolCurl>(); });
#else
    r.register_scheme("http", [] { return std::make_unique<io::StubNetworkProtocol>(); });
    r.register_scheme("https", [] { return std::make_unique<io::StubNetworkProtocol>(); });
#endif

    return r;
}

} // namespace fujinet::platform
