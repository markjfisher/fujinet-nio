#include "fujinet/platform/network_registry.h"

#include "fujinet/io/devices/network_protocol_stub.h"

namespace fujinet::platform {

io::ProtocolRegistry make_default_network_registry()
{
    io::ProtocolRegistry r;

    // Temporary: stub-only on ESP32 until esp_http_client is integrated.
    r.register_scheme("tcp", [] { return std::make_unique<io::StubNetworkProtocol>(); });
    r.register_scheme("http", [] { return std::make_unique<io::StubNetworkProtocol>(); });
    r.register_scheme("https", [] { return std::make_unique<io::StubNetworkProtocol>(); });

    return r;
}

} // namespace fujinet::platform


