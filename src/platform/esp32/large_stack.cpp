#include "fujinet/platform/large_stack.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

namespace fujinet::platform {

namespace {

// All byte-addressable heap: internal RAM and PSRAM together.
constexpr uint32_t kHeapCaps = MALLOC_CAP_8BIT;

// The original ESP32 has additional ROM/cache restrictions on external stacks.
// Keep its worker stack internal even when PSRAM is available for heap data.
#if CONFIG_IDF_TARGET_ESP32 || !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
constexpr uint32_t kStackCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#else
constexpr uint32_t kStackCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif

struct Job {
    void (*fn)(void*);
    void* ctx;
    SemaphoreHandle_t done;
};

void run_job(void* arg)
{
    auto* job = static_cast<Job*>(arg);
    job->fn(job->ctx);
    xSemaphoreGive(job->done);
    // The caller deletes this task and frees its stack with vTaskDeleteWithCaps.
    // Wait here until it does.
    vTaskSuspend(nullptr);
}

} // namespace

// A temporary task with an internal stack on original ESP32, or an external
// stack on targets that permit it (such as S3 with PSRAM). The allocation
// only exists for this call; returns false if the selected heap cannot fit it.
// The caller blocks until it finishes, so the work stays synchronous.
// The work must not write flash: a task with an external stack cannot run
// while the flash cache is disabled.
bool run_with_large_stack(std::size_t stackBytes,
                          void (*fn)(void* ctx),
                          void* ctx,
                          core::LargeStackReport* report)
{
    Job job{fn, ctx, xSemaphoreCreateBinary()};
    if (job.done == nullptr) {
        return false;
    }

    const std::size_t freeBefore = heap_caps_get_free_size(kHeapCaps);
    const std::size_t largestBefore = heap_caps_get_largest_free_block(kHeapCaps);
    // Measures the lowest free heap from here on; fails only if another
    // caller is already monitoring, in which case the low point is not ours.
    const bool monitoring = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;

    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreateWithCaps(&run_job,
                                                   "fn_large_stack",
                                                   static_cast<uint32_t>(stackBytes),   // bytes in ESP-IDF
                                                   &job,
                                                   uxTaskPriorityGet(nullptr),
                                                   &task,
                                                   kStackCaps);
    if (created != pdPASS) {
        if (monitoring) {
            heap_caps_monitor_local_minimum_free_size_stop();
        }
        vSemaphoreDelete(job.done);
        return false;
    }

    xSemaphoreTake(job.done, portMAX_DELAY);
    // StackType_t is a byte on ESP-IDF, so the high-water mark is in bytes.
    const std::size_t stackFree = uxTaskGetStackHighWaterMark(task);
    const std::size_t lowestFree = heap_caps_get_minimum_free_size(kHeapCaps);
    if (monitoring) {
        heap_caps_monitor_local_minimum_free_size_stop();
    }
    vTaskDeleteWithCaps(task);
    vSemaphoreDelete(job.done);

    if (report != nullptr) {
        *report = core::LargeStackReport{};
        report->stackUsedBytes = stackBytes > stackFree ? stackBytes - stackFree : 0;
        report->heapFreeBeforeBytes = freeBefore;
        report->heapLargestBlockBytes = largestBefore;
        report->heapLowestFreeBytes = monitoring ? lowestFree : 0;
    }
    return true;
}

} // namespace fujinet::platform
