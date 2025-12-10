// include/fujinet/io/protocol/fuji_device_ids.h
#pragma once
#include <cstdint>

namespace fujinet::io::protocol {

enum class FujiDeviceId : std::uint8_t {
#if defined(FN_MACHINE_ADAM) || defined(FN_MACHINE_LYNX)
    FujiNet      = 0x0F,
    Keyboard     = 0x01,
    Printer      = 0x02,
    Disk         = 0x04,
    // ...
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

    // New devices
    FileService  = 0xFE,
#endif
};


} // namespace fujinet::io::protocol
