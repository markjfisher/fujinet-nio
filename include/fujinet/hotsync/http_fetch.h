#pragma once

// Downloads a file over plain HTTP for the HotSync install queue (console
// hotsync.fetch): HTTP/1.0 GET, no TLS, no redirects. Meant for a computer on
// the local network serving the apps, e.g. `python3 -m http.server`.
// Blocks while it waits, so it runs on the console task, never the core loop.

#include "fujinet/hotsync/bytes.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fujinet::net {
class ITcpSocketOps;
}

namespace fujinet::hotsync {

struct HttpUrl {
    std::string host;
    std::string port{"80"};
    std::string path{"/"};
};

// False unless url is http://host[:port][/path].
bool parse_http_url(const std::string& url, HttpUrl& out);

// The body of a 200 reply, at most maxBytes. Returns an empty string on
// success, otherwise what went wrong.
std::string http_fetch(net::ITcpSocketOps& ops, const HttpUrl& url, std::size_t maxBytes, Bytes& body,
                       std::uint32_t timeoutMs = 10000);

} // namespace fujinet::hotsync
