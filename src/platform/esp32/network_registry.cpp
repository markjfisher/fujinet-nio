#include "fujinet/platform/network_registry.h"
#include "fujinet/io/devices/network_protocol_stub.h"

#include "fujinet/platform/esp32/tcp_network_protocol_espidf.h"
#include "fujinet/platform/esp32/tls_network_protocol_espidf.h"
#include "fujinet/platform/esp32/http_network_protocol_espidf.h"

namespace fujinet::platform {

io::ProtocolRegistry make_default_network_registry(const config::TlsConfig& tlsConfig)
{
    io::ProtocolRegistry r;

    // TCP stream sockets (ESP-IDF / lwIP)
    r.register_scheme("tcp", [] {
        return std::make_unique<esp32::TcpNetworkProtocolEspIdf>();
    });

    // TLS over TCP (secure sockets using esp_tls)
    r.register_scheme("tls", [tlsConfig] {
        return std::make_unique<esp32::TlsNetworkProtocolEspIdf>(tlsConfig);
    });

    r.register_scheme("http", [tlsConfig] { return std::make_unique<esp32::HttpNetworkProtocolEspIdf>(tlsConfig); });
    r.register_scheme("https", [tlsConfig] { return std::make_unique<esp32::HttpNetworkProtocolEspIdf>(tlsConfig); });

    return r;
}

} // namespace fujinet::platform
