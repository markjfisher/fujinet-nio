#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace fujinet::disk {

enum class ImageType : std::uint8_t {
    Auto = 0,
    Atr  = 1,
    Ssd  = 2,
    Dsd  = 3,
    Raw  = 4, // flat sectors, no header (test-friendly)
};

enum class DiskError : std::uint8_t {
    None = 0,
    InvalidSlot,
    NoSuchFileSystem,
    FileNotFound,
    OpenFailed,
    UnsupportedImageType,
    BadImage,
    NotMounted,
    ReadOnly,
    OutOfRange,
    IoError,
    InternalError,
};

struct DiskGeometry {
    std::uint16_t sectorSize{0};
    std::uint32_t sectorCount{0};
    bool supportsVariableSectorSize{false};
};

struct MountOptions {
    bool readOnlyRequested{false};

    // Optional hint for formats that need it (Raw); ignored by most.
    std::uint16_t sectorSizeHint{0};

    // Optional override; Auto means guess from extension.
    ImageType typeOverride{ImageType::Auto};
};

struct DiskResult {
    DiskError error{DiskError::None};
    bool ok() const noexcept { return error == DiskError::None; }
};

struct DiskSlotInfo {
    bool inserted{false};
    bool readOnly{false};
    bool dirty{false};
    bool changed{false};

    ImageType type{ImageType::Auto};
    DiskGeometry geometry{};

    DiskError lastError{DiskError::None};

    // Optional human-friendly info for tooling/debug (may be empty).
    std::string fsName;
    std::string path;
};

} // namespace fujinet::disk


