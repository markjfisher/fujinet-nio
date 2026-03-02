#include "doctest.h"

#include "fujinet/fs/uri_parser.h"

using namespace fujinet::fs;

TEST_CASE("parse_uri: simple uri with scheme and path")
{
    auto parts = parse_uri("sd:/path/to/file.ssd");
    
    CHECK(parts.scheme == "sd");
    CHECK(parts.authority == "");
    CHECK(parts.path == "/path/to/file.ssd");
}

TEST_CASE("parse_uri: uri with scheme and authority")
{
    auto parts = parse_uri("tnfs://server/path/to/image.atr");
    
    CHECK(parts.scheme == "tnfs");
    CHECK(parts.authority == "server");
    CHECK(parts.path == "/path/to/image.atr");
}

TEST_CASE("parse_uri: uri with scheme, authority and port")
{
    auto parts = parse_uri("http://example.com:8080/files/disk.dsk");
    
    CHECK(parts.scheme == "http");
    CHECK(parts.authority == "example.com:8080");
    CHECK(parts.path == "/files/disk.dsk");
}

TEST_CASE("parse_uri: uri without scheme (absolute path)")
{
    auto parts = parse_uri("/absolute/path/to/file.ssd");
    
    CHECK(parts.scheme == "");
    CHECK(parts.authority == "");
    CHECK(parts.path == "/absolute/path/to/file.ssd");
}

TEST_CASE("parse_uri: uri without scheme (relative path)")
{
    auto parts = parse_uri("relative/path/to/file.atr");
    
    CHECK(parts.scheme == "");
    CHECK(parts.authority == "");
    CHECK(parts.path == "/relative/path/to/file.atr");
}

TEST_CASE("parse_uri: uri with empty scheme")
{
    auto parts = parse_uri(":relative/path");
    
    CHECK(parts.scheme == "");
    CHECK(parts.authority == "");
    CHECK(parts.path == "/relative/path");
}

TEST_CASE("parse_uri: uri with scheme only")
{
    auto parts = parse_uri("sd:");
    
    CHECK(parts.scheme == "sd");
    CHECK(parts.authority == "");
    CHECK(parts.path == "");
}

TEST_CASE("parse_uri: uri with scheme and authority only")
{
    auto parts = parse_uri("http://example.com");
    
    CHECK(parts.scheme == "http");
    CHECK(parts.authority == "example.com");
    CHECK(parts.path == "/");
}

TEST_CASE("parse_uri: uri with query parameters")
{
    auto parts = parse_uri("http://example.com/path/to/file?param1=value1&param2=value2");
    
    CHECK(parts.scheme == "http");
    CHECK(parts.authority == "example.com");
    CHECK(parts.path == "/path/to/file?param1=value1&param2=value2");
}

TEST_CASE("parse_uri: uri with fragment")
{
    auto parts = parse_uri("http://example.com/path/to/file#fragment");
    
    CHECK(parts.scheme == "http");
    CHECK(parts.authority == "example.com");
    CHECK(parts.path == "/path/to/file#fragment");
}

TEST_CASE("parse_uri: empty string")
{
    auto parts = parse_uri("");
    
    CHECK(parts.scheme == "");
    CHECK(parts.authority == "");
    CHECK(parts.path == "/");
}
