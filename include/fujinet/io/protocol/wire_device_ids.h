#pragma once
#include <cstdint>

#include "fujinet/io/core/io_message.h"

namespace fujinet::io::protocol {

// These are WIRE-LEVEL device identifiers.
// They appear in FujiBus and (future) LegacyFrame transports.
// They do NOT encode commands or device semantics.

enum class WireDeviceId : std::uint8_t {
    FujiNet         = 0x70,     // using the legacy ID for FujiNet (FujiDevice)
    Clock           = 0x45,     // ... and clock

    // New NIO devices
    ModemService    = 0xFB,
    DiskService     = 0xFC,
    NetworkService  = 0xFD,
    FileService     = 0xFE,
    HostService     = 0xF0,
};

inline io::DeviceID to_device_id(WireDeviceId id)
{
    return static_cast<io::DeviceID>(
        static_cast<std::uint8_t>(id)
    );
}

} // namespace fujinet::io::protocol
