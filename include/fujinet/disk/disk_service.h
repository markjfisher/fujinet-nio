#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "fujinet/disk/disk_types.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/fs/storage_manager.h"

namespace fujinet::disk {

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

    DiskResult unmount(std::size_t slotIndex);

    DiskResult read_sector(std::size_t slotIndex, std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes);
    DiskResult write_sector(std::size_t slotIndex, std::uint32_t lba, const std::uint8_t* src, std::size_t srcBytes);

    DiskSlotInfo info(std::size_t slotIndex) const;

    void clear_changed(std::size_t slotIndex);

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


