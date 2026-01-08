#pragma once

#include <memory>

#include "fujinet/disk/disk_image.h"

namespace fujinet::disk {

// Atari ATR disk image.
// v1 scope: header parsing + geometry + sector read/write only.
std::unique_ptr<IDiskImage> make_atr_disk_image();

} // namespace fujinet::disk


