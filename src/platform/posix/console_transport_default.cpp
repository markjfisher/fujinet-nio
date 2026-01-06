#include "fujinet/console/console_engine.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace fujinet::console {

// Implemented in console_transport_stdio.cpp
std::unique_ptr<IConsoleTransport> create_console_transport_stdio();

// Implemented in console_transport_pty.cpp (POSIX only)
std::unique_ptr<IConsoleTransport> create_console_transport_pty();

namespace {

static bool env_is(std::string_view key, std::string_view want)
{
    const char* v = std::getenv(std::string(key).c_str());
    if (!v) {
        return false;
    }
    return std::string_view(v) == want;
}

} // namespace

std::unique_ptr<IConsoleTransport> create_default_console_transport()
{
    // Selector:
    //   FN_CONSOLE=stdio  -> use process stdin/stdout
    //   FN_CONSOLE=pty    -> create a dedicated console PTY (default)
    if (env_is("FN_CONSOLE", "stdio")) {
        std::cout << "[Console] Using stdio console (FN_CONSOLE=stdio)\n";
        return create_console_transport_stdio();
    }

    auto pty = create_console_transport_pty();
    if (pty) {
        return pty;
    }

    std::cout << "[Console] Falling back to stdio console\n";
    return create_console_transport_stdio();
}

} // namespace fujinet::console


