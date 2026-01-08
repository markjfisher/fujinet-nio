#include "fujinet/platform/disk_registry.h"

namespace fujinet::platform {

disk::ImageRegistry make_default_disk_image_registry()
{
    // ESP32: all currently supported image types are platform-agnostic.
    // Platform-specific policies (e.g. limiting formats based on storage) can be applied here later.
    return disk::make_default_image_registry();
}

} // namespace fujinet::platform


