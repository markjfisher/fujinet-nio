#pragma once

#include <cstdint>

namespace fujinet::io::protocol {

enum class DiskCommand : std::uint8_t {
    Mount        = 0x01,
    Unmount      = 0x02,
    ReadSector   = 0x03,
    WriteSector  = 0x04,
    Info         = 0x05,
    ClearChanged = 0x06,
};

inline DiskCommand to_disk_command(std::uint16_t raw)
{
    return static_cast<DiskCommand>(static_cast<std::uint8_t>(raw));
}

} // namespace fujinet::io::protocol


