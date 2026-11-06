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
    DiskCopy42 = 5, // Apple DiskCopy 4.2 (Mac 400K/800K floppies)
    XexBoot = 6, // Atari executable synthesised as a read-only bootable disk (mount only)
    D64 = 7, // Commodore 1541 disk image, flat 256-byte sectors (mount only)
};

enum class DiskError : std::uint8_t {
    None = 0,
    InvalidSlot,
    InvalidRequest,
    NoSuchFileSystem,
    FileNotFound,
    AlreadyExists,
    OpenFailed,
    UnsupportedImageType,
    BadImage,
    InvalidGeometry,
    NotMounted,
    ReadOnly,
    OutOfRange,
    IoError,
    InternalError,
    // The image's type or sector size cannot be determined from its content
    // or an unambiguous extension; mount it again with an explicit type and/or
    // sector size hint. Appended: DiskError values are on the wire.
    GeometryRequired,
};

// DiskError values are wire protocol (failure payloads, Info.lastError, the
// FN_DISK_ERR_* names in fujinet-nio-lib): append, never renumber.
static_assert(static_cast<std::uint8_t>(DiskError::BadImage) == 8, "DiskError is on the wire");
static_assert(static_cast<std::uint8_t>(DiskError::InvalidGeometry) == 9, "DiskError is on the wire");
static_assert(static_cast<std::uint8_t>(DiskError::InternalError) == 14, "DiskError is on the wire");
static_assert(static_cast<std::uint8_t>(DiskError::GeometryRequired) == 15, "DiskError is on the wire");

struct DiskGeometry {
    std::uint16_t sectorSize{0};
    std::uint32_t sectorCount{0};
    bool supportsVariableSectorSize{false};
};

struct MountOptions {
    bool readOnlyRequested{false};

    // Optional hint for formats that need it (Raw); ignored by most.
    std::uint16_t sectorSizeHint{0};

    // Optional override; Auto means detect from content/path.
    ImageType typeOverride{ImageType::Auto};

    // Optional geometry supplied by image detection. Image implementations
    // still validate final mount state before accepting it.
    DiskGeometry geometryHint{};
};

struct DiskResult {
    DiskError error{DiskError::None};
    // For read/write operations: bytes transferred (0 for non-data operations).
    std::uint16_t bytes{0};
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

