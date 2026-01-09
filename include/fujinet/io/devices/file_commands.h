#pragma once
#include <cstdint>

namespace fujinet::io::protocol {

enum class FileCommand : std::uint8_t {
    Stat          = 0x01,
    ListDirectory = 0x02,
    ReadFile      = 0x03,
    WriteFile     = 0x04,
    MakeDirectory = 0x05,
};

inline FileCommand to_file_command(std::uint16_t raw)
{
    return static_cast<FileCommand>(static_cast<std::uint8_t>(raw));
}

} // namespace fujinet::io::protocol
