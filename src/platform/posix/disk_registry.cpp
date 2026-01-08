#include "fujinet/platform/disk_registry.h"

namespace fujinet::platform {

disk::ImageRegistry make_default_disk_image_registry()
{
    // POSIX: all currently supported image types are platform-agnostic.
    // Platform-specific policies (e.g. limiting formats) can be applied here later.
    return disk::make_default_image_registry();
}

} // namespace fujinet::platform


