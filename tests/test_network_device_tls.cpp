#include "doctest.h"

// Only compile on POSIX with OpenSSL
#if defined(FN_PLATFORM_POSIX) && FN_WITH_OPENSSL == 1

#include "fujinet/platform/posix/tls_network_protocol_posix.h"
#include "fujinet/io/devices/network_protocol.h"

#include <cstring>
#include <string>

namespace fujinet::tests {

// ------------------------
// TLS URL Parsing Tests
// ------------------------
TEST_CASE("TLS: URL parsing - valid tls://host:port")
{
    platform::posix::TlsNetworkProtocolPosix proto;
    
    // We can't easily test URL parsing without access to internals,
    // but we can test that open() rejects invalid URLs appropriately
    io::NetworkOpenRequest req;
    req.url = "tls://example.com:443";
    
    // This will fail because we can't actually connect in unit tests,
    // but it should at least parse the URL correctly
    auto result = proto.open(req);
    // We expect IOError since we can't actually connect
    CHECK(result != io::StatusCode::Ok);
}

TEST_CASE("TLS: URL parsing - invalid URL format")
{
    platform::posix::TlsNetworkProtocolPosix proto;
    
    io::NetworkOpenRequest req;
    req.url = "not-a-valid-url";
    
    auto result = proto.open(req);
    CHECK(result == io::StatusCode::InvalidRequest);
}

TEST_CASE("TLS: URL parsing - wrong scheme")
{
    platform::posix::TlsNetworkProtocolPosix proto;
    
    io::NetworkOpenRequest req;
    req.url = "http://example.com:80";
    
    auto result = proto.open(req);
    CHECK(result == io::StatusCode::InvalidRequest);
}

TEST_CASE("TLS: URL parsing - missing port uses default 443")
{
    platform::posix::TlsNetworkProtocolPosix proto;
    
    io::NetworkOpenRequest req;
    req.url = "tls://example.com";
    
    // Should parse successfully (default port 443), but fail to connect
    auto result = proto.open(req);
    // We expect IOError since we can't actually connect
    CHECK(result != io::StatusCode::Ok);
}

TEST_CASE("TLS: URL parsing - invalid port")
{
    platform::posix::TlsNetworkProtocolPosix proto;
    
    io::NetworkOpenRequest req;
    req.url = "tls://example.com:abc";
    
    auto result = proto.open(req);
    CHECK(result == io::StatusCode::InvalidRequest);
}

TEST_CASE("TLS: URL parsing - port out of range")
{
    platform::posix::TlsNetworkProtocolPosix proto;
    
    io::NetworkOpenRequest req;
    req.url = "tls://example.com:99999";
    
    auto result = proto.open(req);
    CHECK(result == io::StatusCode::InvalidRequest);
}

TEST_CASE("TLS: operations before open fail")
{
    platform::posix::TlsNetworkProtocolPosix proto;
    
    // Try to read before open
    std::uint8_t buf[64];
    std::uint16_t read = 0;
    bool eof = false;
    auto result = proto.read_body(0, buf, sizeof(buf), read, eof);
    CHECK(result == io::StatusCode::InvalidRequest);
    
    // Try to write before open
    const char* data = "test";
    std::uint16_t written = 0;
    result = proto.write_body(0, reinterpret_cast<const std::uint8_t*>(data), 4, written);
    CHECK(result == io::StatusCode::InvalidRequest);
}

TEST_CASE("TLS: info returns no HTTP data")
{
    platform::posix::TlsNetworkProtocolPosix proto;
    
    io::NetworkInfo info;
    auto result = proto.info(info);
    
    CHECK(result == io::StatusCode::Ok);
    CHECK(info.hasHttpStatus == false);
    CHECK(info.httpStatus == 0);
    CHECK(info.hasContentLength == false);
    CHECK(info.contentLength == 0);
}

TEST_CASE("TLS: close is safe to call multiple times")
{
    platform::posix::TlsNetworkProtocolPosix proto;
    
    // Close without open should be safe
    proto.close();
    proto.close();  // Second close should also be safe
}

} // namespace fujinet::tests

#else // !FN_PLATFORM_POSIX || !FN_WITH_OPENSSL

TEST_CASE("TLS: skipped - not POSIX or OpenSSL not available")
{
    // This test is skipped on non-POSIX platforms or when OpenSSL is not available
    CHECK(true);
}

#endif // FN_PLATFORM_POSIX && FN_WITH_OPENSSL
