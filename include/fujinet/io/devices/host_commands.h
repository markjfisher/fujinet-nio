#pragma once

#include <cstdint>

namespace fujinet::io::protocol {

// HostDevice wire id: WireDeviceId::HostService (0xF0)
//
// All payloads begin with version=1.
//
// GetCurrent:
//   req  [version:1]
//   resp [version:1][hostLen:2][pathLen:2][host][path]
//
// SetCurrent:
//   req  [version:1][specLen:2][spec]
//   resp [version:1]
//
// ListHistory:
//   req  [version:1][offset:2][maxBytes:2]
//   resp [version:1][flags:1][offset:2][dataLen:2][printableText]
//        flags bit0 = more
//
// SelectHistory/DeleteHistory:
//   req  [version:1][index:1]
//   resp [version:1]
enum class HostCommand : std::uint8_t {
    GetCurrent    = 0x01,
    SetCurrent    = 0x02,
    ListHistory   = 0x03,
    SelectHistory = 0x04,
    DeleteHistory = 0x05,
};

inline HostCommand to_host_command(std::uint16_t command)
{
    return static_cast<HostCommand>(static_cast<std::uint8_t>(command & 0xFFU));
}

} // namespace fujinet::io::protocol
