#pragma once
#include <cstdint>

namespace fujinet::io::protocol {

enum class FileCommand : std::uint8_t {
    Stat          = 0x01,
    ListDirectory = 0x02,
    ReadFile      = 0x03,
    WriteFile     = 0x04,
    ResolvePath   = 0x05,
    MakeDirectory = 0x06,
};

// ListDirectory (0x02) request: after startIndex and maxEntries (u16le each), an optional
// u8 `listFlags` is read if the payload is long enough. Bit 0: omit u64 size+mtime per entry
// in the response. Bit 1: sort by basename (full directory is collected before paging; sort
// is a copy+sort, cache order is unchanged).
namespace list_directory {
inline constexpr std::uint8_t kListFlagCompactOmitMetadata = 0x01U;
inline constexpr std::uint8_t kListFlagSortByName        = 0x02U;
} // namespace list_directory

inline FileCommand to_file_command(std::uint16_t raw)
{
    return static_cast<FileCommand>(static_cast<std::uint8_t>(raw));
}

} // namespace fujinet::io::protocol
