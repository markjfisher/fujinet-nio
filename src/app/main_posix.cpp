#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>

#include "fujinet/core/core.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/config/build_profile.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/platform/posix/channel_factory.h"

// Quick forward declaration (we’ll make a proper header later).
namespace fujinet {
    std::string_view version();
}

using namespace fujinet;

// ---------------------------------------------------------
// Temporary dummy implementations to prove the IO pipeline.
// We'll replace DummyChannel with a PTY-backed Channel next.
// ---------------------------------------------------------

class DummyDevice : public io::VirtualDevice {
public:
    io::IOResponse handle(const io::IORequest& request) override {
        io::IOResponse resp;
        resp.id       = request.id;
        resp.deviceId = request.deviceId;
        resp.status   = io::StatusCode::Ok;
        resp.payload  = request.payload;

        std::cout << "[DummyDevice] handle: deviceId="
                  << static_cast<int>(request.deviceId)
                  << " type=" << static_cast<int>(request.type)
                  << " command=" << static_cast<int>(request.command)
                  << " payloadSize=" << request.payload.size()
                  << std::endl;

        return resp;
    }

    void poll() override {}
};

class DummyChannel : public io::Channel {
public:
    bool available() override {
        return false;
    }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override {
        (void)buffer;
        (void)maxLen;
        return 0;
    }

    void write(const std::uint8_t* buffer, std::size_t len) override {
        std::cout << "[DummyChannel] write " << len << " bytes\n";
        (void)buffer;
    }
};

int main()
{
    std::cout << "fujinet-nio starting (POSIX app)\n";
    std::cout << "Version: " << fujinet::version() << "\n";

    core::FujinetCore core;

    // 1. Register a dummy device on DeviceID 1.
    {
        auto dev = std::make_unique<DummyDevice>();
        bool ok = core.deviceManager().registerDevice(1, std::move(dev));
        if (!ok) {
            std::cerr << "Failed to register DummyDevice on DeviceID 1\n";
            return 1;
        }
    }

    // 2. Determine build profile.
    auto profile = config::current_build_profile();
    std::cout << "Build profile: " << profile.name << "\n";

    // 3. Create a Channel appropriate for this profile (PTY, RS232, etc.).
    auto channel = platform::posix::create_channel_for_profile(profile);
    if (!channel) {
        std::cerr << "Failed to create Channel for profile\n";
        return 1;
    }

    // 4. Set up transports based on profile (RS232/PTY/etc.).
    core::setup_transports(core, *channel, profile);

    // 5. Tick the core a few times.
    for (int i = 0; i < 10; ++i) {
        core.tick();
        std::cout << "[POSIX] tick " << core.tick_count() << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "fujinet-nio exiting.\n";
    return 0;
}
