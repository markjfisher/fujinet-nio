#pragma once
#include <cstdint>

namespace fujinet::io::protocol {

enum class FileCommand : std::uint8_t {
    ListDirectory = 0x01,
    // future: Open, Read, Write, Remove, etc.
};

inline FileCommand to_file_command(std::uint16_t raw)
{
    return static_cast<FileCommand>(static_cast<std::uint8_t>(raw));
}

} // namespace fujinet::io::protocol
