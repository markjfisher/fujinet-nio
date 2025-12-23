#pragma once

#include "fujinet/io/devices/network_protocol.h"

#if FN_WITH_CURL == 1

#include <cstdint>
#include <string>
#include <vector>
#include <curl/curl.h>

namespace fujinet::platform::posix {

class HttpNetworkProtocolCurl final : public io::INetworkProtocol {
public:
    io::StatusCode open(const io::NetworkOpenRequest& req) override;

    io::StatusCode write_body(std::uint32_t offset,
                              const std::uint8_t* data,
                              std::size_t dataLen,
                              std::uint16_t& written) override;

    io::StatusCode read_body(std::uint32_t offset,
                             std::uint8_t* out,
                             std::size_t outLen,
                             std::uint16_t& read,
                             bool& eof) override;

    io::StatusCode info(io::NetworkInfo& out) override;

    void poll() override {}
    void close() override;

private:
    static std::size_t write_body_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata);
    static std::size_t write_header_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata);
    io::StatusCode perform_now(); // executes the request synchronously, fills _body/_headersBlock/_httpStatus

    io::NetworkOpenRequest _req{};
    std::uint16_t _httpStatus{0};
    std::string _headersBlock;
    std::vector<std::uint8_t> _body;

    CURL* _curl = nullptr;
    curl_slist* _slist = nullptr;

    std::vector<std::uint8_t> _requestBody;
    std::uint32_t _expectedRequestBodyLen = 0;
    bool _performed = false;

};

} // namespace fujinet::platform::posix

#endif // FN_WITH_CURL


