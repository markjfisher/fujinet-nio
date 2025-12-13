#pragma once
#include <cstdint>

#include "fujinet/io/core/io_message.h"

namespace fujinet::io::protocol {

// These are WIRE-LEVEL device identifiers.
// They appear in FujiBus and (future) LegacyFrame transports.
// They do NOT encode commands or device semantics.

enum class WireDeviceId : std::uint8_t {
// legacy - TODO: Are these still valid?
#if defined(FN_MACHINE_ADAM) || defined(FN_MACHINE_LYNX)
    FujiNet      = 0x0F,
    Keyboard     = 0x01,
    Printer      = 0x02,
    Disk         = 0x04,
#else
    FujiNet      = 0x70,
    DiskFirst    = 0x31,
    DiskLast     = 0x3F,
    PrinterFirst = 0x40,
    PrinterLast  = 0x43,
    Voice        = 0x43,
    Clock        = 0x45,
    NetworkFirst = 0x71,
    NetworkLast  = 0x78,
    Midi         = 0x99,
    Dbc          = 0xFF,

    // New NIO devices
    NetworkService = 0xFD,
    FileService  = 0xFE,
#endif
};

inline io::DeviceID to_device_id(WireDeviceId id)
{
    return static_cast<io::DeviceID>(
        static_cast<std::uint8_t>(id)
    );
}

} // namespace fujinet::io::protocol
