#include "fujinet/disk/image_registry.h"

#include <algorithm>
#include <cctype>

namespace fujinet::disk {

namespace {
class UnsupportedImage final : public IDiskImage {
public:
    explicit UnsupportedImage(ImageType t) : _t(t) {}

    ImageType type() const noexcept override { return _t; }
    DiskGeometry geometry() const noexcept override { return {}; }
    bool read_only() const noexcept override { return true; }

    DiskResult mount(std::unique_ptr<fs::IFile>, std::uint64_t, const MountOptions&) override
    {
        return DiskResult{DiskError::UnsupportedImageType};
    }
    DiskResult unmount() override { return DiskResult{DiskError::None}; }
    DiskResult read_sector(std::uint32_t, std::uint8_t*, std::size_t) override
    {
        return DiskResult{DiskError::UnsupportedImageType};
    }
    DiskResult write_sector(std::uint32_t, const std::uint8_t*, std::size_t) override
    {
        return DiskResult{DiskError::UnsupportedImageType};
    }
    DiskResult flush() override { return DiskResult{DiskError::None}; }

private:
    ImageType _t{ImageType::Auto};
};

static std::string lower_ascii(std::string_view s)
{
    std::string out(s);
    for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}
} // namespace

bool ImageRegistry::register_type(ImageType type, Factory factory)
{
    if (type == ImageType::Auto || !factory) {
        return false;
    }
    const auto key = static_cast<std::uint8_t>(type);
    if (_factories.find(key) != _factories.end()) {
        return false;
    }
    _factories.emplace(key, std::move(factory));
    return true;
}

std::unique_ptr<IDiskImage> ImageRegistry::create(ImageType type) const
{
    if (type == ImageType::Auto) {
        return nullptr;
    }
    const auto key = static_cast<std::uint8_t>(type);
    auto it = _factories.find(key);
    if (it == _factories.end()) {
        return nullptr;
    }
    return (it->second)();
}

ImageType guess_type_from_path(std::string_view path)
{
    const std::string p = lower_ascii(path);

    auto dot = p.find_last_of('.');
    if (dot == std::string::npos) return ImageType::Auto;

    const std::string ext = p.substr(dot + 1);
    if (ext == "atr") return ImageType::Atr;
    if (ext == "ssd") return ImageType::Ssd;
    if (ext == "dsd") return ImageType::Dsd;
    if (ext == "img" || ext == "raw") return ImageType::Raw;
    return ImageType::Auto;
}

ImageRegistry make_default_image_registry()
{
    ImageRegistry reg;

    // Raw is implemented in raw_image.cpp (registered by DiskService init code).
    // Atr/Ssd/Dsd are placeholders for v1; format handlers can be added later.
    reg.register_type(ImageType::Atr, [] { return std::make_unique<UnsupportedImage>(ImageType::Atr); });
    reg.register_type(ImageType::Ssd, [] { return std::make_unique<UnsupportedImage>(ImageType::Ssd); });
    reg.register_type(ImageType::Dsd, [] { return std::make_unique<UnsupportedImage>(ImageType::Dsd); });

    return reg;
}

} // namespace fujinet::disk


