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

    void poll() override;
    void close() override;

private:
    static std::size_t write_body_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata);
    static std::size_t write_header_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata);
    io::StatusCode start_async(); // starts the request via curl_multi, returns immediately
    void tick_async();            // advance multi state and collect completion

    io::NetworkOpenRequest _req{};
    std::uint16_t _httpStatus{0};
    std::string _headersBlock;
    std::vector<std::uint8_t> _body;

    CURL* _curl = nullptr;
    CURLM* _multi = nullptr;
    curl_slist* _slist = nullptr;

    std::vector<std::uint8_t> _requestBody;
    std::uint32_t _expectedRequestBodyLen = 0;
    bool _performed = false;   // true once the transfer is complete
    bool _inProgress = false;  // true while curl transfer is running
    io::StatusCode _finalStatus = io::StatusCode::Ok;

    // Streaming response buffering:
    // We keep a growing buffer as curl delivers bytes, but we advance a base
    // offset as the host reads sequentially, so we don't grow without bound.
    std::uint32_t _bodyBaseOffset = 0;
    std::size_t _bodyStartIndex = 0;

};

} // namespace fujinet::platform::posix

#endif // FN_WITH_CURL


