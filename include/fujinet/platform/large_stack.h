#pragma once

#include "fujinet/core/large_stack.h"

#include <cstddef>

namespace fujinet::platform {

// core::LargeStackRunner for this platform: runs fn(ctx) on a temporary
// thread or task whose stack is stackBytes, and waits for it. The stack only
// exists for the call. Implemented in platform-specific .cpp files
// (POSIX: a pthread; original ESP32: an internal FreeRTOS stack;
// ESP32 targets permitting external stacks, such as S3: a PSRAM stack).
bool run_with_large_stack(std::size_t stackBytes,
                          void (*fn)(void* ctx),
                          void* ctx,
                          core::LargeStackReport* report);

} // namespace fujinet::platform
