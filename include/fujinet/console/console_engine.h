#pragma once

#include "fujinet/diag/diagnostic_registry.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::fs {
class StorageManager;
}

namespace fujinet::console {

class IConsoleTransport {
public:
    virtual ~IConsoleTransport() = default;

    // Reads a single input byte.
    // Returns false on timeout (and implementations may also return false when input is unavailable).
    virtual bool read_byte(std::uint8_t& out, int timeout_ms) = 0;

    // Convenience helper: read a line by buffering bytes until '\n' or '\r'.
    // This is primarily for tests and simple transports; ConsoleEngine uses byte-level input.
    virtual bool read_line(std::string& out, int timeout_ms)
    {
        out.clear();

        std::uint8_t ch = 0;
        if (!read_byte(ch, timeout_ms)) {
            return false;
        }

        for (;;) {
            if (ch == '\r' || ch == '\n') {
                return true;
            }
            out.push_back(static_cast<char>(ch));

            // After the first byte, don't block indefinitely for more.
            if (!read_byte(ch, 0)) {
                return false;
            }
        }
    }

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
    ConsoleEngine(diag::DiagnosticRegistry& registry, IConsoleTransport& io, fujinet::fs::StorageManager& storage);

    // Blocking loop (best for dedicated thread/task).
    void run_loop();

    // One cooperative iteration. Returns false to stop the console.
    // `timeout_ms` is passed to the transport.
    bool step(int timeout_ms);

private:
    // Interactive line editor (ANSI-ish), returns:
    // - true + sets out_line when a full line is committed
    // - false when no line is available (timeout/no input)
    bool read_line_edit(std::string& out_line, int timeout_ms);

    void render_edit_line();
    void clear_edit_line();

    bool handle_line(std::string_view line);

    diag::DiagnosticRegistry& _registry;
    IConsoleTransport& _io;
    fujinet::fs::StorageManager* _storage{nullptr};

    std::string _prompt{"> "};
    bool _edit_rendered{false};

    std::string _cwd_fs;
    std::string _cwd_path{"/"};

    std::string _edit;
    std::size_t _cursor{0};
    char _pending_eol{0}; // swallow CRLF/LFCR as a single line commit

    std::vector<std::string> _history;
    std::size_t _history_max{50};
    bool _hist_active{false};
    std::size_t _hist_index{0};
    std::string _hist_saved;

    std::string _esc; // escape sequence accumulator
};

} // namespace fujinet::console


