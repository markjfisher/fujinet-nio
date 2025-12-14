#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "fujinet/io/devices/network_protocol.h"

namespace fujinet::io {

// Deterministic stub protocol used to validate the device protocol plumbing.
class StubNetworkProtocol final : public INetworkProtocol {
public:
    StatusCode open(const NetworkOpenRequest& req) override;

    StatusCode write_body(std::uint32_t offset,
                          const std::uint8_t* data,
                          std::size_t dataLen,
                          std::uint16_t& written) override;

    StatusCode read_body(std::uint32_t offset,
                         std::uint8_t* out,
                         std::size_t outLen,
                         std::uint16_t& read,
                         bool& eof) override;

    StatusCode info(std::size_t maxHeaderBytes, NetworkInfo& out) override;

    void poll() override {}
    void close() override;

private:
    NetworkOpenRequest _openReq{};

    std::uint16_t _httpStatus{0};
    std::string _headersBlock;
    std::vector<std::uint8_t> _body;
    std::uint64_t _contentLength{0};

    // If POST/PUT, we accept request body writes into this buffer.
    std::vector<std::uint8_t> _requestBody;
};

} // namespace fujinet::io


