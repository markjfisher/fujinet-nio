#pragma once

#include "fujinet/io/devices/modem_device.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace fujinet::io {

// Friend accessor for ModemDevice internals, used by out-of-band diagnostics providers.
// Keeps debug/console helpers out of the main device header.
struct ModemDeviceDiagnosticsAccessor {
    struct StateRow {
        bool cmdMode{true};
        bool connected{false};
        bool listening{false};
        bool pending{false};
        bool autoAnswer{false};
        bool telnet{true};
        bool echo{true};
        bool numeric{false};
        bool baudLock{false};

        std::uint16_t listenPort{0};
        std::uint32_t baud{9600};

        std::uint32_t hostWriteCursor{0};
        std::uint32_t hostReadCursor{0};
        std::uint32_t hostRxAvail{0};
    };

    static StateRow state(const ModemDevice& d) noexcept
    {
        StateRow s;
        s.cmdMode = d._cmdMode;
        s.connected = d.is_connected();
        s.listening = (d._listenFd >= 0);
        s.pending = (d._pendingFd >= 0);
        s.autoAnswer = d._autoAnswer;
        s.telnet = d._useTelnet;
        s.echo = d._commandEcho;
        s.numeric = d._numericResult;
        s.baudLock = d._baudLock;
        s.listenPort = d._listenPort;
        s.baud = d._modemBaud;
        s.hostWriteCursor = d._hostWriteCursor;
        s.hostReadCursor = d._hostReadCursor;
        s.hostRxAvail = static_cast<std::uint32_t>(d._toHost.size());
        return s;
    }

    static void set_baud(ModemDevice& d, std::uint32_t baud) noexcept
    {
        if (d._baudLock) return;
        if (baud == 300 || baud == 600 || baud == 1200 || baud == 1800 ||
            baud == 2400 || baud == 4800 || baud == 9600 || baud == 19200) {
            d._modemBaud = baud;
        }
    }

    static void set_baud_lock(ModemDevice& d, bool enable) noexcept
    {
        d._baudLock = enable;
    }

    static void inject_bytes(ModemDevice& d, std::string_view bytes) noexcept
    {
        for (unsigned char c : bytes) {
            d.process_host_byte(static_cast<std::uint8_t>(c));
        }
    }

    static std::string drain_output(ModemDevice& d, std::size_t maxBytes = 4096)
    {
        std::string out;
        out.reserve(std::min<std::size_t>(maxBytes, d._toHost.size()));

        std::uint8_t b = 0;
        while (out.size() < maxBytes && d._toHost.pop(b)) {
            out.push_back(static_cast<char>(b));
        }
        return out;
    }
};

} // namespace fujinet::io


