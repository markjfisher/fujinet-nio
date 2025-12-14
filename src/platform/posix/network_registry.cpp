#include "fujinet/platform/network_registry.h"

#include "fujinet/io/devices/network_protocol_stub.h"

#if FN_WITH_CURL == 1
#include "fujinet/platform/posix/http_network_protocol_curl.h"
#endif

namespace fujinet::platform {

io::ProtocolRegistry make_default_network_registry()
{
    io::ProtocolRegistry r;

    // Always provide tcp (stub for now).
    r.register_scheme("tcp", [] { return std::make_unique<io::StubNetworkProtocol>(); });

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


