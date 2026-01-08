#include "fujinet/disk/disk_service.h"

#include "fujinet/disk/raw_image.h"

namespace fujinet::disk {

DiskService::DiskService(fs::StorageManager& storage, ImageRegistry registry)
    : _storage(storage), _registry(std::move(registry))
{
    // Ensure Raw is always available for v1 testing/tooling.
    _registry.register_type(ImageType::Raw, [] { return make_raw_disk_image(); });
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

    // Helper: make file size by seeking to last byte then writing one 0.
    auto ensure_size = [&](std::uint64_t totalBytes) -> bool {
        if (totalBytes == 0) return false;
        if (!f->seek(totalBytes - 1)) return false;
        const std::uint8_t z = 0;
        return f->write(&z, 1) == 1;
    };

    if (type == ImageType::Raw) {
        const std::uint64_t total = static_cast<std::uint64_t>(sectorSize) * sectorCount;
        if (!ensure_size(total) || !f->flush()) return DiskResult{DiskError::IoError};
        return DiskResult{DiskError::None};
    }

    if (type == ImageType::Ssd) {
        // SSD is always 256-byte sectors, common sizes: 400 or 800 sectors.
        if (sectorSize != 256) return DiskResult{DiskError::InvalidGeometry};
        if (!(sectorCount == 400 || sectorCount == 800)) return DiskResult{DiskError::InvalidGeometry};
        const std::uint64_t total = 256ull * sectorCount;
        if (!ensure_size(total) || !f->flush()) return DiskResult{DiskError::IoError};
        return DiskResult{DiskError::None};
    }

    if (type == ImageType::Atr) {
        // Minimal ATR create derived from classic firmware behavior.
        // Header is 16 bytes.
        if (!(sectorSize == 128 || sectorSize == 256 || sectorSize == 512)) {
            return DiskResult{DiskError::InvalidGeometry};
        }

        std::uint64_t totalData = static_cast<std::uint64_t>(sectorSize) * sectorCount;
        // Adjust for first 3 sectors being 128 bytes when sectorSize == 256 (matches old firmware logic).
        if (sectorSize == 256) {
            if (sectorCount < 3) return DiskResult{DiskError::InvalidGeometry};
            totalData -= 384ull;
        }

        const std::uint32_t paragraphs = static_cast<std::uint32_t>(totalData / 16ull);

        std::uint8_t hdr[16]{};
        hdr[0] = 0x96;
        hdr[1] = 0x02;
        hdr[2] = static_cast<std::uint8_t>(paragraphs & 0xFF);
        hdr[3] = static_cast<std::uint8_t>((paragraphs >> 8) & 0xFF);
        hdr[4] = static_cast<std::uint8_t>(sectorSize & 0xFF);
        hdr[5] = static_cast<std::uint8_t>((sectorSize >> 8) & 0xFF);
        hdr[6] = static_cast<std::uint8_t>((paragraphs >> 16) & 0xFF);

        if (f->write(hdr, sizeof(hdr)) != sizeof(hdr)) return DiskResult{DiskError::IoError};

        // Write first three 128-byte sectors for sectorSize < 512 (spec convention).
        std::uint8_t blank[512]{};
        if (sectorSize < 512) {
            for (int i = 0; i < 3; ++i) {
                if (f->write(blank, 128) != 128) return DiskResult{DiskError::IoError};
            }
        }

        // Sparse seek to last sector and write it.
        if (sectorCount == 0) return DiskResult{DiskError::InvalidGeometry};

        std::uint64_t lastOff = 16;
        if (sectorSize < 512) {
            const std::uint32_t remaining = sectorCount - 3;
            if (remaining > 0) {
                lastOff += 3ull * 128ull;
                lastOff += static_cast<std::uint64_t>(remaining) * sectorSize;
                lastOff -= sectorSize;
                if (!f->seek(lastOff)) return DiskResult{DiskError::IoError};
                if (f->write(blank, sectorSize) != sectorSize) return DiskResult{DiskError::IoError};
            }
        } else {
            lastOff += static_cast<std::uint64_t>(sectorCount) * sectorSize;
            lastOff -= sectorSize;
            if (!f->seek(lastOff)) return DiskResult{DiskError::IoError};
            if (f->write(blank, sectorSize) != sectorSize) return DiskResult{DiskError::IoError};
        }

        if (!f->flush()) return DiskResult{DiskError::IoError};
        return DiskResult{DiskError::None};
    }

    return DiskResult{DiskError::UnsupportedImageType};
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


