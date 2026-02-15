#pragma once

namespace fujinet::platform {

// Per-platform early init: USB console (TinyUSB), logging backend, and any
// hardware init required before the first log. Call first in app_main().
// No logging must occur before this returns.
void early_init();

} // namespace fujinet::platform
