#include "fujinet/platform/service_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>

namespace fujinet::platform {

namespace {

// Below the core loop, so a long sync never delays the bus.
constexpr UBaseType_t kServicePriority = 1;

struct Job {
    void (*fn)(void*);
    void* ctx;
};

void run_job(void* arg)
{
    const Job job = *static_cast<Job*>(arg);
    delete static_cast<Job*>(arg);
    job.fn(job.ctx);
    vTaskDelete(nullptr);
}

} // namespace

// The stack is in internal RAM (plain xTaskCreate): a task with a PSRAM stack
// cannot run while the flash cache is off, and services write flash and SD.
bool start_service_task(const char* name, std::size_t stackBytes, void (*fn)(void* ctx), void* ctx)
{
    auto* job = new Job{fn, ctx};
    const BaseType_t created = xTaskCreate(&run_job, name, static_cast<uint32_t>(stackBytes), job,
                                           kServicePriority, nullptr);
    if (created != pdPASS) {
        delete job;
        return false;
    }
    return true;
}

} // namespace fujinet::platform
