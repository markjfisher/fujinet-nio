#include "fujinet/disk/image_registry.h"

#include "fujinet/disk/atr_image.h"
#include "fujinet/disk/raw_image.h"
#include "fujinet/disk/ssd_image.h"

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

bool ImageRegistry::register_creator(ImageType type, Creator creator, CreateValidator validator)
{
    if (type == ImageType::Auto || !creator || !validator) {
        return false;
    }
    const auto key = static_cast<std::uint8_t>(type);
    if (_creators.find(key) != _creators.end()) {
        return false;
    }
    _creators.emplace(key, std::move(creator));
    _createValidators.emplace(key, std::move(validator));
    return true;
}

DiskResult ImageRegistry::validate_create(
    ImageType type,
    std::uint16_t sectorSize,
    std::uint32_t sectorCount
) const {
    if (type == ImageType::Auto) return DiskResult{DiskError::UnsupportedImageType};
    auto it = _createValidators.find(static_cast<std::uint8_t>(type));
    if (it == _createValidators.end()) return DiskResult{DiskError::UnsupportedImageType};
    return (it->second)(sectorSize, sectorCount);
}

DiskResult ImageRegistry::create_file(ImageType type, fs::IFile& file, std::uint16_t sectorSize, std::uint32_t sectorCount) const
{
    if (type == ImageType::Auto) return DiskResult{DiskError::UnsupportedImageType};
    const auto key = static_cast<std::uint8_t>(type);
    auto it = _creators.find(key);
    if (it == _creators.end()) {
        return DiskResult{DiskError::UnsupportedImageType};
    }
    return (it->second)(file, sectorSize, sectorCount);
}

ImageRegistry make_default_image_registry()
{
    ImageRegistry reg;

    // Built-in image types (platform-agnostic):
    reg.register_type(ImageType::Raw, [] { return make_raw_disk_image(); });
    reg.register_type(ImageType::Atr, [] { return make_atr_disk_image(); });
    reg.register_type(ImageType::Ssd, [] { return make_ssd_disk_image(); });
    reg.register_type(ImageType::Dsd, [] { return std::make_unique<UnsupportedImage>(ImageType::Dsd); });

    // Creators (blank image creation).
    reg.register_creator(
        ImageType::Raw,
        [](fs::IFile& f, std::uint16_t ss, std::uint32_t sc) { return create_raw_image_file(f, ss, sc); },
        validate_raw_image_geometry);
    reg.register_creator(
        ImageType::Atr,
        [](fs::IFile& f, std::uint16_t ss, std::uint32_t sc) { return create_atr_image_file(f, ss, sc); },
        validate_atr_image_geometry);
    reg.register_creator(
        ImageType::Ssd,
        [](fs::IFile& f, std::uint16_t ss, std::uint32_t sc) { return create_ssd_image_file(f, ss, sc); },
        validate_ssd_image_geometry);

    return reg;
}

} // namespace fujinet::disk
