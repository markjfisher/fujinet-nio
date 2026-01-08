#include "fujinet/disk/disk_service.h"

#include "fujinet/disk/raw_image.h"

namespace fujinet::disk {

DiskService::DiskService(fs::StorageManager& storage, ImageRegistry registry)
    : _storage(storage), _registry(std::move(registry))
{
    // No implicit registration here: the platform/build chooses what formats exist via ImageRegistry.
}

DiskService::Slot* DiskService::slot_ptr(std::size_t slotIndex)
{
    if (slotIndex >= MAX_SLOTS) return nullptr;
    return &_slots[slotIndex];
}

const DiskService::Slot* DiskService::slot_ptr(std::size_t slotIndex) const
{
    if (slotIndex >= MAX_SLOTS) return nullptr;
    return &_slots[slotIndex];
}

DiskError DiskService::set_error(std::size_t slotIndex, DiskError e)
{
    if (auto* s = slot_ptr(slotIndex)) {
        s->lastError = e;
    }
    return e;
}

DiskResult DiskService::mount(
    std::size_t slotIndex,
    const std::string& fsName,
    const std::string& path,
    const MountOptions& opts
) {
    auto* s = slot_ptr(slotIndex);
    if (!s) return DiskResult{DiskError::InvalidSlot};

    // Unmount any existing image first.
    if (s->image) {
        s->image->flush();
        s->image->unmount();
        s->image.reset();
    }

    s->inserted = false;
    s->readOnly = false;
    s->dirty = false;
    s->changed = true;
    s->type = ImageType::Auto;
    s->geometry = {};
    s->lastError = DiskError::None;
    s->fsName = fsName;
    s->path = path;

    auto* pfs = _storage.get(fsName);
    if (!pfs) return DiskResult{set_error(slotIndex, DiskError::NoSuchFileSystem)};

    if (!pfs->exists(path)) return DiskResult{set_error(slotIndex, DiskError::FileNotFound)};

    fs::FileInfo finfo{};
    if (!pfs->stat(path, finfo)) return DiskResult{set_error(slotIndex, DiskError::OpenFailed)};

    ImageType type = opts.typeOverride;
    if (type == ImageType::Auto) {
        type = guess_type_from_path(path);
    }
    if (type == ImageType::Auto) {
        return DiskResult{set_error(slotIndex, DiskError::UnsupportedImageType)};
    }

    // Try open writeable if requested; if it fails, fall back to read-only.
    bool readOnlyEffective = opts.readOnlyRequested;
    std::unique_ptr<fs::IFile> f;
    if (opts.readOnlyRequested) {
        f = pfs->open(path, "rb");
    } else {
        f = pfs->open(path, "r+b");
        if (!f) {
            f = pfs->open(path, "rb");
            readOnlyEffective = true;
        }
    }
    if (!f) return DiskResult{set_error(slotIndex, DiskError::OpenFailed)};

    auto img = _registry.create(type);
    if (!img) return DiskResult{set_error(slotIndex, DiskError::UnsupportedImageType)};

    MountOptions eff = opts;
    eff.readOnlyRequested = readOnlyEffective;
    DiskResult r = img->mount(std::move(f), finfo.sizeBytes, eff);
    if (!r.ok()) return DiskResult{set_error(slotIndex, r.error)};

    s->inserted = true;
    s->readOnly = img->read_only();
    s->type = img->type();
    s->geometry = img->geometry();
    s->image = std::move(img);

    return DiskResult{DiskError::None};
}

DiskResult DiskService::create_image(
    const std::string& fsName,
    const std::string& path,
    ImageType type,
    std::uint16_t sectorSize,
    std::uint32_t sectorCount,
    bool overwrite
) {
    if (type == ImageType::Auto) return DiskResult{DiskError::UnsupportedImageType};
    if (sectorSize == 0 || sectorCount == 0) return DiskResult{DiskError::InvalidGeometry};

    auto* pfs = _storage.get(fsName);
    if (!pfs) return DiskResult{DiskError::NoSuchFileSystem};

    if (!overwrite && pfs->exists(path)) {
        return DiskResult{DiskError::AlreadyExists};
    }

    auto f = pfs->open(path, "wb");
    if (!f) return DiskResult{DiskError::OpenFailed};

    DiskResult r = _registry.create_file(type, *f, sectorSize, sectorCount);
    if (!r.ok()) return r;
    if (!f->flush()) return DiskResult{DiskError::IoError};
    return DiskResult{DiskError::None};
}

DiskResult DiskService::unmount(std::size_t slotIndex)
{
    auto* s = slot_ptr(slotIndex);
    if (!s) return DiskResult{DiskError::InvalidSlot};

    if (s->image) {
        s->image->flush();
        s->image->unmount();
        s->image.reset();
    }

    s->inserted = false;
    s->readOnly = false;
    s->dirty = false;
    s->changed = true;
    s->type = ImageType::Auto;
    s->geometry = {};
    s->lastError = DiskError::None;
    s->fsName.clear();
    s->path.clear();

    return DiskResult{DiskError::None};
}

DiskResult DiskService::read_sector(std::size_t slotIndex, std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes)
{
    auto* s = slot_ptr(slotIndex);
    if (!s) return DiskResult{DiskError::InvalidSlot};
    if (!s->image) return DiskResult{set_error(slotIndex, DiskError::NotMounted)};

    DiskResult r = s->image->read_sector(lba, dst, dstBytes);
    if (!r.ok()) set_error(slotIndex, r.error);
    return r;
}

DiskResult DiskService::write_sector(std::size_t slotIndex, std::uint32_t lba, const std::uint8_t* src, std::size_t srcBytes)
{
    auto* s = slot_ptr(slotIndex);
    if (!s) return DiskResult{DiskError::InvalidSlot};
    if (!s->image) return DiskResult{set_error(slotIndex, DiskError::NotMounted)};

    DiskResult r = s->image->write_sector(lba, src, srcBytes);
    if (r.ok()) {
        s->dirty = true;
    } else {
        set_error(slotIndex, r.error);
    }
    return r;
}

DiskSlotInfo DiskService::info(std::size_t slotIndex) const
{
    DiskSlotInfo out{};
    auto* s = slot_ptr(slotIndex);
    if (!s) {
        out.lastError = DiskError::InvalidSlot;
        return out;
    }

    out.inserted = s->inserted;
    out.readOnly = s->readOnly;
    out.dirty = s->dirty;
    out.changed = s->changed;
    out.type = s->type;
    out.geometry = s->geometry;
    out.lastError = s->lastError;
    out.fsName = s->fsName;
    out.path = s->path;
    return out;
}

void DiskService::clear_changed(std::size_t slotIndex)
{
    if (auto* s = slot_ptr(slotIndex)) {
        s->changed = false;
    }
}

} // namespace fujinet::disk


