#include "fujinet/io/devices/disk_device.h"

#include "fujinet/core/logging.h"
#include "fujinet/disk/disk_types.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/disk_codec.h"
#include "fujinet/io/devices/disk_commands.h"

namespace fujinet::io {

using fujinet::disk::DiskError;
using fujinet::disk::DiskResult;
using fujinet::disk::ImageType;
using fujinet::disk::MountOptions;

namespace diskproto = fujinet::io::diskproto;
using fujinet::io::protocol::DiskCommand;
using fujinet::io::protocol::to_disk_command;

static const char* TAG = "disk";

static StatusCode map_disk_error(DiskError e) noexcept
{
    switch (e) {
        case DiskError::None:               return StatusCode::Ok;
        case DiskError::InvalidSlot:        return StatusCode::InvalidRequest;
        case DiskError::NoSuchFileSystem:   return StatusCode::InvalidRequest;
        case DiskError::FileNotFound:       return StatusCode::InvalidRequest;
        case DiskError::OpenFailed:         return StatusCode::IOError;
        case DiskError::UnsupportedImageType:return StatusCode::Unsupported;
        case DiskError::BadImage:           return StatusCode::InvalidRequest;
        case DiskError::NotMounted:         return StatusCode::NotReady;
        case DiskError::ReadOnly:           return StatusCode::InvalidRequest;
        case DiskError::OutOfRange:         return StatusCode::InvalidRequest;
        case DiskError::IoError:            return StatusCode::IOError;
        case DiskError::InternalError:      return StatusCode::InternalError;
    }
    return StatusCode::InternalError;
}

static bool parse_slot_1based(std::uint8_t slot1, std::size_t& outIdx) noexcept
{
    if (slot1 == 0) return false;
    outIdx = static_cast<std::size_t>(slot1 - 1);
    return true;
}

DiskDevice::DiskDevice(fs::StorageManager& storage, disk::ImageRegistry registry)
    : _svc(storage, std::move(registry))
{
}

DiskDevice::DiskDevice(fs::StorageManager& storage)
    : DiskDevice(storage, disk::make_default_image_registry())
{
}

IOResponse DiskDevice::handle(const IORequest& request)
{
    const auto cmd = to_disk_command(request.command);

    diskproto::Reader r(request.payload.data(), request.payload.size());
    std::uint8_t ver = 0;
    if (!r.read_u8(ver) || ver != DISKPROTO_VERSION) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    switch (cmd) {
        case DiskCommand::Mount: {
            std::uint8_t slot1 = 0, flags = 0, typeRaw = 0;
            std::uint16_t sectorHint = 0;
            std::string_view fsName, path;

            if (!r.read_u8(slot1)) return make_base_response(request, StatusCode::InvalidRequest);
            if (!r.read_u8(flags)) return make_base_response(request, StatusCode::InvalidRequest);
            if (!r.read_u8(typeRaw)) return make_base_response(request, StatusCode::InvalidRequest);
            if (!r.read_u16le(sectorHint)) return make_base_response(request, StatusCode::InvalidRequest);
            if (!r.read_lp_u16_string(fsName)) return make_base_response(request, StatusCode::InvalidRequest);
            if (!r.read_lp_u16_string(path)) return make_base_response(request, StatusCode::InvalidRequest);

            std::size_t idx = 0;
            if (!parse_slot_1based(slot1, idx) || idx >= _svc.slot_count()) {
                return make_base_response(request, StatusCode::InvalidRequest);
            }

            MountOptions opts{};
            opts.readOnlyRequested = (flags & 0x01) != 0;
            opts.typeOverride = static_cast<ImageType>(typeRaw);
            opts.sectorSizeHint = sectorHint;

            DiskResult dr = _svc.mount(idx, std::string(fsName), std::string(path), opts);
            IOResponse resp = make_base_response(request, map_disk_error(dr.error));
            if (resp.status != StatusCode::Ok) return resp;

            const auto info = _svc.info(idx);

            std::vector<std::uint8_t> out;
            out.reserve(16);
            diskproto::write_u8(out, DISKPROTO_VERSION);

            std::uint8_t oflags = 0;
            if (info.inserted) oflags |= 0x01;
            if (info.readOnly) oflags |= 0x02;
            diskproto::write_u8(out, oflags);
            diskproto::write_u16le(out, 0);
            diskproto::write_u8(out, slot1);
            diskproto::write_u8(out, static_cast<std::uint8_t>(info.type));
            diskproto::write_u16le(out, info.geometry.sectorSize);
            diskproto::write_u32le(out, info.geometry.sectorCount);

            resp.payload = std::move(out);
            return resp;
        }

        case DiskCommand::Unmount: {
            std::uint8_t slot1 = 0;
            if (!r.read_u8(slot1)) return make_base_response(request, StatusCode::InvalidRequest);

            std::size_t idx = 0;
            if (!parse_slot_1based(slot1, idx) || idx >= _svc.slot_count()) {
                return make_base_response(request, StatusCode::InvalidRequest);
            }

            DiskResult dr = _svc.unmount(idx);
            IOResponse resp = make_base_response(request, map_disk_error(dr.error));
            if (resp.status != StatusCode::Ok) return resp;

            std::vector<std::uint8_t> out;
            diskproto::write_u8(out, DISKPROTO_VERSION);
            diskproto::write_u8(out, 0);
            diskproto::write_u16le(out, 0);
            diskproto::write_u8(out, slot1);
            resp.payload = std::move(out);
            return resp;
        }

        case DiskCommand::ReadSector: {
            std::uint8_t slot1 = 0;
            std::uint32_t lba = 0;
            std::uint16_t maxBytes = 0;
            if (!r.read_u8(slot1)) return make_base_response(request, StatusCode::InvalidRequest);
            if (!r.read_u32le(lba)) return make_base_response(request, StatusCode::InvalidRequest);
            if (!r.read_u16le(maxBytes)) return make_base_response(request, StatusCode::InvalidRequest);

            std::size_t idx = 0;
            if (!parse_slot_1based(slot1, idx) || idx >= _svc.slot_count()) {
                return make_base_response(request, StatusCode::InvalidRequest);
            }

            const auto info = _svc.info(idx);
            if (!info.inserted) return make_base_response(request, StatusCode::NotReady);
            if (info.geometry.sectorSize == 0) return make_base_response(request, StatusCode::InternalError);

            const std::size_t want = info.geometry.sectorSize;
            std::vector<std::uint8_t> buf(want);
            DiskResult dr = _svc.read_sector(idx, lba, buf.data(), buf.size());
            IOResponse resp = make_base_response(request, map_disk_error(dr.error));
            if (resp.status != StatusCode::Ok) return resp;

            std::uint16_t dataLen = static_cast<std::uint16_t>(want);
            std::uint8_t flags = 0;
            if (maxBytes < dataLen) {
                dataLen = maxBytes;
                flags |= 0x01; // truncated (caller buffer limit)
            }

            std::vector<std::uint8_t> out;
            out.reserve(1 + 1 + 2 + 1 + 4 + 2 + dataLen);
            diskproto::write_u8(out, DISKPROTO_VERSION);
            diskproto::write_u8(out, flags);
            diskproto::write_u16le(out, 0);
            diskproto::write_u8(out, slot1);
            diskproto::write_u32le(out, lba);
            diskproto::write_u16le(out, dataLen);
            diskproto::write_bytes(out, buf.data(), dataLen);

            resp.payload = std::move(out);
            return resp;
        }

        case DiskCommand::WriteSector: {
            std::uint8_t slot1 = 0;
            std::uint32_t lba = 0;
            std::uint16_t dataLen = 0;
            const std::uint8_t* bytes = nullptr;

            if (!r.read_u8(slot1)) return make_base_response(request, StatusCode::InvalidRequest);
            if (!r.read_u32le(lba)) return make_base_response(request, StatusCode::InvalidRequest);
            if (!r.read_u16le(dataLen)) return make_base_response(request, StatusCode::InvalidRequest);
            if (!r.read_bytes(bytes, dataLen)) return make_base_response(request, StatusCode::InvalidRequest);

            std::size_t idx = 0;
            if (!parse_slot_1based(slot1, idx) || idx >= _svc.slot_count()) {
                return make_base_response(request, StatusCode::InvalidRequest);
            }

            const auto info = _svc.info(idx);
            if (!info.inserted) return make_base_response(request, StatusCode::NotReady);
            if (info.readOnly) return make_base_response(request, StatusCode::InvalidRequest);
            if (info.geometry.sectorSize == 0) return make_base_response(request, StatusCode::InternalError);
            if (dataLen < info.geometry.sectorSize) return make_base_response(request, StatusCode::InvalidRequest);

            DiskResult dr = _svc.write_sector(idx, lba, bytes, dataLen);
            IOResponse resp = make_base_response(request, map_disk_error(dr.error));
            if (resp.status != StatusCode::Ok) return resp;

            std::vector<std::uint8_t> out;
            out.reserve(1 + 1 + 2 + 1 + 4 + 2);
            diskproto::write_u8(out, DISKPROTO_VERSION);
            diskproto::write_u8(out, 0);
            diskproto::write_u16le(out, 0);
            diskproto::write_u8(out, slot1);
            diskproto::write_u32le(out, lba);
            diskproto::write_u16le(out, info.geometry.sectorSize);
            resp.payload = std::move(out);
            return resp;
        }

        case DiskCommand::Info: {
            std::uint8_t slot1 = 0;
            if (!r.read_u8(slot1)) return make_base_response(request, StatusCode::InvalidRequest);

            std::size_t idx = 0;
            if (!parse_slot_1based(slot1, idx) || idx >= _svc.slot_count()) {
                return make_base_response(request, StatusCode::InvalidRequest);
            }

            const auto info = _svc.info(idx);

            std::uint8_t flags = 0;
            if (info.inserted) flags |= 0x01;
            if (info.readOnly) flags |= 0x02;
            if (info.dirty) flags |= 0x04;
            if (info.changed) flags |= 0x08;
            if (info.geometry.sectorSize && info.geometry.sectorCount) flags |= 0x10;
            flags |= 0x20; // hasLastError (always include as u8)

            IOResponse resp = make_success_response(request);

            std::vector<std::uint8_t> out;
            out.reserve(1 + 1 + 2 + 1 + 1 + 2 + 4 + 1);
            diskproto::write_u8(out, DISKPROTO_VERSION);
            diskproto::write_u8(out, flags);
            diskproto::write_u16le(out, 0);
            diskproto::write_u8(out, slot1);
            diskproto::write_u8(out, static_cast<std::uint8_t>(info.type));
            diskproto::write_u16le(out, info.geometry.sectorSize);
            diskproto::write_u32le(out, info.geometry.sectorCount);
            diskproto::write_u8(out, static_cast<std::uint8_t>(info.lastError));

            resp.payload = std::move(out);
            return resp;
        }

        case DiskCommand::ClearChanged: {
            std::uint8_t slot1 = 0;
            if (!r.read_u8(slot1)) return make_base_response(request, StatusCode::InvalidRequest);

            std::size_t idx = 0;
            if (!parse_slot_1based(slot1, idx) || idx >= _svc.slot_count()) {
                return make_base_response(request, StatusCode::InvalidRequest);
            }

            _svc.clear_changed(idx);

            IOResponse resp = make_success_response(request);
            std::vector<std::uint8_t> out;
            diskproto::write_u8(out, DISKPROTO_VERSION);
            diskproto::write_u8(out, 0);
            diskproto::write_u16le(out, 0);
            diskproto::write_u8(out, slot1);
            resp.payload = std::move(out);
            return resp;
        }
    }

    FN_LOGW(TAG, "Unsupported DiskCommand %u", static_cast<unsigned>(static_cast<std::uint8_t>(cmd)));
    return make_base_response(request, StatusCode::Unsupported);
}

} // namespace fujinet::io


