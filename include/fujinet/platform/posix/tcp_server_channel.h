#pragma once

#include "fujinet/io/core/channel.h"

#include <cstdint>
#include <memory>
#include <string>

namespace fujinet::platform::posix {

// `readableWait`: the channel can block the core loop until bytes arrive, for a peer
// that waits on every reply (NetIEC sends one request per bus byte). Off, the loop
// sleeps its idle delay between requests.
std::unique_ptr<fujinet::io::Channel>
create_tcp_server_channel(const std::string& host, std::uint16_t port, bool readableWait = false);

} // namespace fujinet::platform::posix
