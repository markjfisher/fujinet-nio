#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "fujinet/disk/disk_types.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/fs/storage_manager.h"

namespace fujinet::disk {

// Public struct for pending mount information (exposed via public API).
struct PendingMountInfo {
    std::string uri;       // Original URI from config
    std::string mode;     // Requested mode (r, rw)
    bool enabled;         // Whether this mount is active
};

// Forward declaration
struct PendingMountInfo;

class DiskService {
public:
    static constexpr std::size_t MAX_SLOTS = 8;

    DiskService(fs::StorageManager& storage, ImageRegistry registry);

    std::size_t slot_count() const noexcept { return MAX_SLOTS; }

    DiskResult mount(
        std::size_t slotIndex,
        const std::string& fsName,
        const std::string& path,
        const MountOptions& opts
    );

    // Create a new image file of a given type/geometry.
    // This does NOT mount it; call mount() afterwards if desired.
    DiskResult create_image(
        const std::string& fsName,
        const std::string& path,
        ImageType type,
        std::uint16_t sectorSize,
        std::uint32_t sectorCount,
        bool overwrite
    );

    DiskResult unmount(std::size_t slotIndex);

    DiskResult read_sector(std::size_t slotIndex, std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes);
    DiskResult write_sector(std::size_t slotIndex, std::uint32_t lba, const std::uint8_t* src, std::size_t srcBytes);

    DiskSlotInfo info(std::size_t slotIndex) const;

    void clear_changed(std::size_t slotIndex);

    // Set a pending (lazy) mount for a slot. The mount will be activated
    // on first access (read/write). This allows config-defined mounts without
    // immediate I/O at startup.
    void set_pending_mount(std::size_t slotIndex, const std::string& uri, const std::string& mode, bool enabled);

    // Get the pending mount config for a slot (if any).
    std::optional<PendingMountInfo> get_pending_mount(std::size_t slotIndex) const;

    // Clear the pending mount for a slot.
    void clear_pending_mount(std::size_t slotIndex);

private:
    struct Slot {
        bool inserted{false};
        bool readOnly{false};
        bool dirty{false};
        bool changed{false};

        ImageType type{ImageType::Auto};
        DiskGeometry geometry{};
        DiskError lastError{DiskError::None};

        std::string fsName;
        std::string path;

        // Lazy-mount support: stores pending mount config until first access.
        // When non-empty, indicates a config-defined mount that hasn't been
        // activated yet. This allows startup without immediate network/file I/O.
        std::optional<PendingMountInfo> pendingMount;

        std::unique_ptr<IDiskImage> image;
    };

    DiskError set_error(std::size_t slotIndex, DiskError e);
    Slot*       slot_ptr(std::size_t slotIndex);
    const Slot* slot_ptr(std::size_t slotIndex) const;

    fs::StorageManager& _storage;
    ImageRegistry _registry;
    std::array<Slot, MAX_SLOTS> _slots{};
};

} // namespace fujinet::disk


