#pragma once

#include <cstdint>

namespace fujinet::io::protocol {

inline constexpr std::uint8_t SLOT_CATALOG_PROTOCOL_VERSION = 1;

enum class SlotCatalogCommand : std::uint8_t {
    Get    = 0x01,
    Put    = 0x02,
    Delete = 0x03,
    Range  = 0x04,
};

inline SlotCatalogCommand to_slot_catalog_command(std::uint16_t raw)
{
    return static_cast<SlotCatalogCommand>(static_cast<std::uint8_t>(raw));
}

namespace slot_catalog {
inline constexpr std::uint8_t kRequestTailUri = 0x01U;
inline constexpr std::uint8_t kRequestFormatted = 0x02U;
inline constexpr std::uint8_t kResponseMore = 0x01U;
inline constexpr std::uint8_t kResponseFormatted = 0x02U;
inline constexpr std::uint8_t kEntryValid = 0x01U;
inline constexpr std::uint8_t kEntryReadOnly = 0x02U;
inline constexpr std::uint8_t kEntryUriTruncated = 0x04U;
inline constexpr std::uint8_t kDeleteRemoved = 0x01U;
} // namespace slot_catalog

} // namespace fujinet::io::protocol
