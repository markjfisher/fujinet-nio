#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "fujinet/disk/disk_image.h"
#include "fujinet/disk/disk_types.h"

namespace fujinet::disk {

class ImageRegistry {
public:
    using Factory = std::function<std::unique_ptr<IDiskImage>()>;

    bool register_type(ImageType type, Factory factory);
    std::unique_ptr<IDiskImage> create(ImageType type) const;

private:
    std::unordered_map<std::uint8_t, Factory> _factories;
};

// Lowercases ASCII and guesses from extension.
ImageType guess_type_from_path(std::string_view path);

// Default registry (pure/core): provides Raw (implemented) and placeholders for Atr/Ssd/Dsd.
ImageRegistry make_default_image_registry();

} // namespace fujinet::disk


