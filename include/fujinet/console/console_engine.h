#pragma once

#include "fujinet/diag/diagnostic_registry.h"

#include <memory>
#include <string>
#include <string_view>

namespace fujinet::console {

class IConsoleTransport {
public:
    virtual ~IConsoleTransport() = default;

    // Returns true when a complete line was read and placed in `out`.
    // Returns false on timeout (and implementations may also return false when input is unavailable).
    virtual bool read_line(std::string& out, int timeout_ms) = 0;

    virtual void write(std::string_view s) = 0;

    virtual void write_line(std::string_view s) = 0;
};

// Platform-provided default transport.
// POSIX: stdio
// ESP32: developer console (currently UART/CDC depending on platform glue)
std::unique_ptr<IConsoleTransport> create_default_console_transport();

class ConsoleEngine {
public:
    ConsoleEngine(diag::DiagnosticRegistry& registry, IConsoleTransport& io);

    // Blocking loop (best for dedicated thread/task).
    void run_loop();

    // One cooperative iteration. Returns false to stop the console.
    // `timeout_ms` is passed to the transport.
    bool step(int timeout_ms);

private:
    bool handle_line(std::string_view line);

    diag::DiagnosticRegistry& _registry;
    IConsoleTransport& _io;
    bool _promptShown{false};
};

} // namespace fujinet::console


