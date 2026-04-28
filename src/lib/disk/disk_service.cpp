#include "fujinet/disk/disk_service.h"

#include "fujinet/core/logging.h"
#include "fujinet/disk/raw_image.h"

namespace fujinet::disk {

static constexpr const char* TAG = "disk_svc";

static const char* disk_error_name(DiskError e) noexcept
{
    switch (e) {
        case DiskError::None: return "None";
        case DiskError::InvalidSlot: return "InvalidSlot";
        case DiskError::InvalidRequest: return "InvalidRequest";
        case DiskError::NoSuchFileSystem: return "NoSuchFileSystem";
        case DiskError::FileNotFound: return "FileNotFound";
        case DiskError::AlreadyExists: return "AlreadyExists";
        case DiskError::OpenFailed: return "OpenFailed";
        case DiskError::UnsupportedImageType: return "UnsupportedImageType";
        case DiskError::BadImage: return "BadImage";
        case DiskError::InvalidGeometry: return "InvalidGeometry";
        case DiskError::NotMounted: return "NotMounted";
        case DiskError::ReadOnly: return "ReadOnly";
        case DiskError::OutOfRange: return "OutOfRange";
        case DiskError::IoError: return "IoError";
        case DiskError::InternalError: return "InternalError";
    }
    return "Unknown";
}

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

    FN_LOGI(TAG,
            "Mount start: slot=%u fs='%s' path='%s' readonly_requested=%d type_override=%u sector_hint=%u",
            static_cast<unsigned>(slotIndex),
            fsName.c_str(),
            path.c_str(),
            opts.readOnlyRequested ? 1 : 0,
            static_cast<unsigned>(opts.typeOverride),
            static_cast<unsigned>(opts.sectorSizeHint));

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
    if (!pfs) {
        FN_LOGW(TAG, "Mount failed: filesystem '%s' not registered", fsName.c_str());
        return DiskResult{set_error(slotIndex, DiskError::NoSuchFileSystem)};
    }

    if (!pfs->exists(path)) {
        FN_LOGW(TAG, "Mount failed: path does not exist '%s'", path.c_str());
        return DiskResult{set_error(slotIndex, DiskError::FileNotFound)};
    }

    fs::FileInfo finfo{};
    if (!pfs->stat(path, finfo)) {
        FN_LOGW(TAG, "Mount failed: stat failed for '%s'", path.c_str());
        return DiskResult{set_error(slotIndex, DiskError::OpenFailed)};
    }

    ImageType type = opts.typeOverride;
    if (type == ImageType::Auto) {
        type = guess_type_from_path(path);
    }
    if (type == ImageType::Auto) {
        FN_LOGW(TAG, "Mount failed: could not infer image type from '%s'", path.c_str());
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
            FN_LOGI(TAG, "Writable open failed for '%s'; retrying read-only", path.c_str());
            f = pfs->open(path, "rb");
            readOnlyEffective = true;
        }
    }
    if (!f) {
        FN_LOGW(TAG, "Mount failed: file open failed for '%s'", path.c_str());
        return DiskResult{set_error(slotIndex, DiskError::OpenFailed)};
    }

    auto img = _registry.create(type);
    if (!img) {
        FN_LOGW(TAG, "Mount failed: no image handler for type=%u", static_cast<unsigned>(type));
        return DiskResult{set_error(slotIndex, DiskError::UnsupportedImageType)};
    }

    MountOptions eff = opts;
    eff.readOnlyRequested = readOnlyEffective;
    DiskResult r = img->mount(std::move(f), finfo.sizeBytes, eff);
    if (!r.ok()) {
        FN_LOGW(TAG,
                "Mount failed: image mount error=%s(%u) size=%llu",
                disk_error_name(r.error),
                static_cast<unsigned>(r.error),
                static_cast<unsigned long long>(finfo.sizeBytes));
        return DiskResult{set_error(slotIndex, r.error)};
    }

    s->inserted = true;
    s->readOnly = img->read_only();
    s->type = img->type();
    s->geometry = img->geometry();
    s->image = std::move(img);

    FN_LOGI(TAG,
            "Mount success: slot=%u type=%u readonly=%d sector_size=%u sector_count=%lu",
            static_cast<unsigned>(slotIndex),
            static_cast<unsigned>(s->type),
            s->readOnly ? 1 : 0,
            static_cast<unsigned>(s->geometry.sectorSize),
            static_cast<unsigned long>(s->geometry.sectorCount));

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

    // Lazy mount: if there's a pending mount but no image, activate it now
    if (!s->image && s->pendingMount) {
        // Resolve the pending mount
        auto [fs, resolvedPath] = _storage.resolveUri(s->pendingMount->uri);
        if (!fs) {
            return DiskResult{set_error(slotIndex, DiskError::NoSuchFileSystem)};
        }

        MountOptions opts{};
        opts.readOnlyRequested = (s->pendingMount->mode.find('w') == std::string::npos);

        // Attempt to mount
        auto mountResult = mount(slotIndex, fs->name(), resolvedPath, opts);
        if (!mountResult.ok()) {
            return mountResult;
        }
    }

    if (!s->image) return DiskResult{set_error(slotIndex, DiskError::NotMounted)};

    DiskResult r = s->image->read_sector(lba, dst, dstBytes);
    if (!r.ok()) set_error(slotIndex, r.error);
    return r;
}

DiskResult DiskService::write_sector(std::size_t slotIndex, std::uint32_t lba, const std::uint8_t* src, std::size_t srcBytes)
{
    auto* s = slot_ptr(slotIndex);
    if (!s) return DiskResult{DiskError::InvalidSlot};

    // Lazy mount: if there's a pending mount but no image, activate it now
    if (!s->image && s->pendingMount) {
        // Resolve the pending mount
        auto [fs, resolvedPath] = _storage.resolveUri(s->pendingMount->uri);
        if (!fs) {
            return DiskResult{set_error(slotIndex, DiskError::NoSuchFileSystem)};
        }

        MountOptions opts{};
        opts.readOnlyRequested = (s->pendingMount->mode.find('w') == std::string::npos);

        // Attempt to mount
        auto mountResult = mount(slotIndex, fs->name(), resolvedPath, opts);
        if (!mountResult.ok()) {
            return mountResult;
        }
    }

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

void DiskService::set_pending_mount(std::size_t slotIndex, const std::string& uri, const std::string& mode, bool enabled)
{
    auto* s = slot_ptr(slotIndex);
    if (!s) return;
    
    s->pendingMount = PendingMountInfo{uri, mode, enabled};
    
    // Also store fsName and path for display purposes (will be updated on actual mount)
    // Parse the URI to extract filesystem name
    auto slashPos = uri.find('/');
    if (slashPos != std::string::npos) {
        s->fsName = uri.substr(0, slashPos);
        s->path = uri;
    } else {
        s->fsName = uri;
        s->path = uri;
    }
    
    // Mark as changed so the UI knows there's a pending mount
    s->changed = true;
}

std::optional<PendingMountInfo> DiskService::get_pending_mount(std::size_t slotIndex) const
{
    auto* s = slot_ptr(slotIndex);
    if (!s) return std::nullopt;
    return s->pendingMount;
}

void DiskService::clear_pending_mount(std::size_t slotIndex)
{
    auto* s = slot_ptr(slotIndex);
    if (!s) return;
    s->pendingMount.reset();
}

} // namespace fujinet::disk

