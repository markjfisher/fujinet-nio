#include "fujinet/platform/early_init.h"
#include "fujinet/platform/esp32/tinyusb_cdc.h"

namespace fujinet::platform {

void early_init()
{
    platform_usb_console_early_init();
}

} // namespace fujinet::platform
