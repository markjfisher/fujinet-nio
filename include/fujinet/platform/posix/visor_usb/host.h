#pragma once

#include "fujinet/io/visor_usb/protocol.h"
#include "fujinet/config/fuji_config.h"
#include <chrono>
#include <memory>
#include <string>

namespace fujinet::platform::posix::visor_usb {
struct Options {
    unsigned port{2};
    unsigned bus{0};             // zero: unspecified
    std::vector<std::uint8_t> path; // physical port chain, not volatile device address
    std::string portSource{"config"}, busSource{"config"}, pathSource{"config"};
};
Options resolve_options(const config::VisorConfig& config);
Options options_from_environment(); // strict; no invalid-value fallback

// Single-threaded libusb backend. poll/wait callbacks only update private state;
// they never call NIO services. All transfer storage lives until completion.
class Host {
public:
    explicit Host(Options options = {});
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    void poll();
    void wait(std::chrono::milliseconds timeout);
    bool active() const;
    bool needs_poll() const;
    std::uint64_t generation() const;
    bool take(io::protocol::ByteBuffer& packet);
    bool send(const io::protocol::ByteBuffer& packet);
    void retire(const std::string& reason);
    std::string status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace fujinet::platform::posix::visor_usb
