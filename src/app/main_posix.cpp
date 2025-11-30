#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>

#include "fujinet/core/core.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/transport/rs232_transport.h"

// Quick forward declaration (we’ll make a proper header later).
namespace fujinet {
    std::string_view version();
}

using namespace fujinet;

// ---------------------------------------------------------
// Dummy implementations to prove the IO pipeline works.
// ---------------------------------------------------------

// A simple device that just echoes the payload.
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

    void poll() override {
        // No background work for now.
    }
};

// A channel that never has input and logs writes.
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

    // 2. Create a dummy channel + RS232 transport and attach it to the core.
    DummyChannel channel;
    io::Rs232Transport rs232(channel);
    core.addTransport(&rs232);

    // 3. Tick the core a few times.
    for (int i = 0; i < 10; ++i) {
        core.tick();

        std::cout << "[POSIX] tick " << core.tick_count() << "\n";

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "fujinet-nio exiting.\n";
    return 0;
}
