#include "fujinet/platform/network_registry.h"

#include "fujinet/io/devices/network_protocol_stub.h"
#include "fujinet/platform/esp32/http_network_protocol_espidf.h"

namespace fujinet::platform {

io::ProtocolRegistry make_default_network_registry()
{
    io::ProtocolRegistry r;

    // Temporary: stub-only on ESP32 until esp_http_client is integrated.
    r.register_scheme("tcp", [] { return std::make_unique<io::StubNetworkProtocol>(); });
    r.register_scheme("http", [] { return std::make_unique<esp32::HttpNetworkProtocolEspIdf>(); });
    r.register_scheme("https", [] { return std::make_unique<esp32::HttpNetworkProtocolEspIdf>(); });

    return r;
}

} // namespace fujinet::platform


