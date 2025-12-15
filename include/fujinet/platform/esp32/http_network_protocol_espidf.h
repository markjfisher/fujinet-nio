#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "fujinet/io/devices/network_protocol.h"

namespace fujinet::platform::esp32 {

struct HttpNetworkProtocolEspIdfState;

// ESP32 HTTP/HTTPS backend using ESP-IDF esp_http_client with bounded streaming.
//
// Key property: sequential offsets only.
// - read_body(offset, ...) succeeds only when offset == current stream cursor.
// - Any other offset returns StatusCode::InvalidRequest.
class HttpNetworkProtocolEspIdf final : public fujinet::io::INetworkProtocol {
public:
    HttpNetworkProtocolEspIdf();
    ~HttpNetworkProtocolEspIdf() override;

    fujinet::io::StatusCode open(const fujinet::io::NetworkOpenRequest& req) override;

    fujinet::io::StatusCode write_body(std::uint32_t offset,
                                       const std::uint8_t* data,
                                       std::size_t dataLen,
                                       std::uint16_t& written) override;

    fujinet::io::StatusCode read_body(std::uint32_t offset,
                                      std::uint8_t* out,
                                      std::size_t outLen,
                                      std::uint16_t& read,
                                      bool& eof) override;

    fujinet::io::StatusCode info(std::size_t maxHeaderBytes, fujinet::io::NetworkInfo& out) override;

    void poll() override;
    void close() override;

private:
    // opaque (ESP-IDF only, defined in .cpp)
    HttpNetworkProtocolEspIdfState* _s{nullptr};
};

} // namespace fujinet::platform::esp32


