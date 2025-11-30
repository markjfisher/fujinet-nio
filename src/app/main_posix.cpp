#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

#include "fujinet/core/core.h"

// Quick forward declaration (we’ll make a proper header later).
namespace fujinet {
    std::string_view version();
}

int main()
{
    std::cout << "fujinet-nio starting (POSIX app)\n";
    std::cout << "Version: " << fujinet::version() << "\n";

    fujinet::core::FujinetCore core;

    // Simple demo loop: tick 10 times and print the counter.
    // Later, this can become a proper event loop.
    for (int i = 0; i < 10; ++i) {
        core.tick();

        std::cout << "[POSIX] tick " << core.tick_count() << "\n";

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "fujinet-nio exiting.\n";
    return 0;
}
