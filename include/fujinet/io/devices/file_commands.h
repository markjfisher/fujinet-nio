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
    AppStoreStat  = 0x20,
    AppStoreRead  = 0x21,
    AppStoreWrite = 0x22,
    AppStoreDelete = 0x23,
    AppStoreList  = 0x24,
};

// ListDirectory (0x02) request: after startIndex and maxPayloadBytes (u16le each), an
// optional u8 `listFlags` is read if the payload is long enough. maxPayloadBytes limits the
// variable entries blob in the response (complete entries only).
// Bit 0: compact — omit u64 size+mtime per entry (binary entries blob).
// Bit 1: sort by basename (full directory is collected before paging).
// Bit 2: formatted — entries blob is UTF-8 text lines (newline-terminated, whole lines only).
//   Incompatible with bit 0.
namespace list_directory {
inline constexpr std::uint8_t kListFlagCompactOmitMetadata = 0x01U;
inline constexpr std::uint8_t kListFlagSortByName        = 0x02U;
inline constexpr std::uint8_t kListFlagFormattedLines    = 0x04U;
inline constexpr std::uint8_t kListResponseFlagFormatted = 0x04U;
} // namespace list_directory

inline FileCommand to_file_command(std::uint16_t raw)
{
    return static_cast<FileCommand>(static_cast<std::uint8_t>(raw));
}

} // namespace fujinet::io::protocol
