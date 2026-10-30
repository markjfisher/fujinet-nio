#include "fujinet/platform/service_task.h"

#include <pthread.h>

#include <algorithm>
#include <climits>

namespace fujinet::platform {

namespace {

struct Job {
    void (*fn)(void*);
    void* ctx;
};

void* run_job(void* arg)
{
    const Job job = *static_cast<Job*>(arg);
    delete static_cast<Job*>(arg);
    job.fn(job.ctx);
    return nullptr;
}

} // namespace

// The thread is detached: it ends when fn returns, or with the process.
bool start_service_task(const char* name, std::size_t stackBytes, void (*fn)(void* ctx), void* ctx)
{
    (void)name;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        return false;
    }
    pthread_attr_setstacksize(&attr, std::max<std::size_t>(stackBytes, PTHREAD_STACK_MIN));
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    auto* job = new Job{fn, ctx};
    pthread_t thread;
    const bool started = pthread_create(&thread, &attr, &run_job, job) == 0;
    pthread_attr_destroy(&attr);
    if (!started) {
        delete job;
    }
    return started;
}

} // namespace fujinet::platform
