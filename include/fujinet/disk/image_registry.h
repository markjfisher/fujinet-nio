#pragma once

#include <functional>
#include <memory>
#include <unordered_map>

#include "fujinet/disk/disk_image.h"
#include "fujinet/disk/disk_types.h"
#include "fujinet/fs/filesystem.h"

namespace fujinet::disk {

class ImageRegistry {
public:
    using Factory = std::function<std::unique_ptr<IDiskImage>()>;
    using Creator = std::function<DiskResult(fs::IFile& file, std::uint16_t sectorSize, std::uint32_t sectorCount)>;

    bool register_type(ImageType type, Factory factory);
    std::unique_ptr<IDiskImage> create(ImageType type) const;

    bool register_creator(ImageType type, Creator creator);
    DiskResult create_file(ImageType type, fs::IFile& file, std::uint16_t sectorSize, std::uint32_t sectorCount) const;

private:
    std::unordered_map<std::uint8_t, Factory> _factories;
    std::unordered_map<std::uint8_t, Creator> _creators;
};

// Default registry (pure/core): provides Raw (implemented) and placeholders for Atr/Ssd/Dsd.
ImageRegistry make_default_image_registry();

} // namespace fujinet::disk

