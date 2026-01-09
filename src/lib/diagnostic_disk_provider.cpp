#include "fujinet/diag/diagnostic_provider.h"

#include "fujinet/core/core.h"
#include "fujinet/io/devices/disk_device.h"
#include "fujinet/io/devices/disk_device_diagnostics.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::diag {

namespace {

static fujinet::io::DiskDevice* get_disk_device(fujinet::core::FujinetCore& core)
{
    using fujinet::io::protocol::WireDeviceId;
    using fujinet::io::protocol::to_device_id;

    auto* dev = core.deviceManager().getDevice(to_device_id(WireDeviceId::DiskService));
    return dynamic_cast<fujinet::io::DiskDevice*>(dev);
}

static const char* image_type_str(fujinet::disk::ImageType t) noexcept
{
    using fujinet::disk::ImageType;
    switch (t) {
        case ImageType::Auto: return "auto";
        case ImageType::Atr:  return "atr";
        case ImageType::Ssd:  return "ssd";
        case ImageType::Dsd:  return "dsd";
        case ImageType::Raw:  return "raw";
    }
    return "unknown";
}

static const char* disk_err_str(fujinet::disk::DiskError e) noexcept
{
    using fujinet::disk::DiskError;
    switch (e) {
        case DiskError::None:               return "none";
        case DiskError::InvalidSlot:        return "invalid_slot";
        case DiskError::InvalidRequest:     return "invalid_request";
        case DiskError::NoSuchFileSystem:   return "no_such_fs";
        case DiskError::FileNotFound:       return "file_not_found";
        case DiskError::AlreadyExists:      return "already_exists";
        case DiskError::OpenFailed:         return "open_failed";
        case DiskError::UnsupportedImageType:return "unsupported_type";
        case DiskError::BadImage:           return "bad_image";
        case DiskError::InvalidGeometry:    return "invalid_geometry";
        case DiskError::NotMounted:         return "not_mounted";
        case DiskError::ReadOnly:           return "read_only";
        case DiskError::OutOfRange:         return "out_of_range";
        case DiskError::IoError:            return "io_error";
        case DiskError::InternalError:      return "internal_error";
    }
    return "unknown";
}

class DiskDiagnosticProvider final : public IDiagnosticProvider {
public:
    explicit DiskDiagnosticProvider(fujinet::core::FujinetCore& core)
        : _core(core)
    {}

    std::string_view provider_id() const noexcept override { return "disk"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "disk.slots",
            .summary = "list disk slots (mounted images, geometry, state)",
            .usage = "disk.slots",
            .safe = true,
        });
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }

        const std::string_view cmd = args.argv[0];
        if (cmd == "disk.slots") {
            return cmd_slots();
        }

        return DiagResult::not_found("unknown disk command");
    }

private:
    DiagResult cmd_slots()
    {
        auto* dev = get_disk_device(_core);
        if (!dev) {
            return DiagResult::not_ready("DiskDevice not registered");
        }

        const auto slots = fujinet::io::DiskDeviceDiagnosticsAccessor::slots(*dev);

        std::string text;
        text.reserve(256 + slots.size() * 96);

        for (std::size_t i = 0; i < slots.size(); ++i) {
            const auto& s = slots[i];
            const unsigned slot1 = static_cast<unsigned>(i + 1);

            text += "slot=";
            text += std::to_string(slot1);
            text += " inserted=";
            text += s.inserted ? "1" : "0";
            text += " ro=";
            text += s.readOnly ? "1" : "0";
            text += " dirty=";
            text += s.dirty ? "1" : "0";
            text += " changed=";
            text += s.changed ? "1" : "0";
            text += " type=";
            text += image_type_str(s.type);
            text += " ss=";
            text += std::to_string(s.geometry.sectorSize);
            text += " sc=";
            text += std::to_string(s.geometry.sectorCount);
            text += " last_err=";
            text += disk_err_str(s.lastError);
            if (!s.fsName.empty() || !s.path.empty()) {
                text += " image=";
                text += s.fsName;
                text += ":";
                text += s.path;
            }
            text += "\n";
        }

        return DiagResult::ok(text);
    }

    fujinet::core::FujinetCore& _core;
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_disk_diagnostic_provider(::fujinet::core::FujinetCore& core)
{
    return std::make_unique<DiskDiagnosticProvider>(core);
}

} // namespace fujinet::diag


