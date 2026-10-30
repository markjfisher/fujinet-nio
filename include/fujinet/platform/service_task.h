#pragma once

#include <cstddef>

namespace fujinet::platform {

// Starts fn(ctx) on a thread or task of its own that lives until fn returns,
// for a service that blocks for long stretches (a HotSync waits minutes on a
// Palm) and so cannot run in core.tick(). Returns false, without calling fn,
// when it cannot be started. The stack is in internal RAM on ESP32, so the
// task may write flash and the SD card.
// Implemented in platform-specific .cpp files (POSIX: a detached pthread;
// ESP32: a FreeRTOS task).
bool start_service_task(const char* name, std::size_t stackBytes, void (*fn)(void* ctx), void* ctx);

} // namespace fujinet::platform
