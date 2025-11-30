#include <iostream>
#include <string_view>

// Quick forward declaration (we'll make a proper header later).
namespace fujinet {
    std::string_view version();
}

int main()
{
    std::cout << "fujinet-nio starting (POSIX app)\n";
    std::cout << "Version: " << fujinet::version() << "\n";

    // Here is where we'll eventually:
    //  - parse config
    //  - create IODeviceManager, RoutingManager, transports, etc.
    //  - start the main loop

    std::cout << "fujinet-nio exiting.\n";
    return 0;
}
