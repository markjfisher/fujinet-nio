#pragma once

#include "fujinet/core/system_events.h"

namespace fujinet::platform::esp32 {

/**
 * Starts SNTP when the device gets an IP address.
 * Publishes a TimeEvent when time sync is completed.
 */
class SntpService {
public:
    explicit SntpService(fujinet::core::SystemEvents& events);
    ~SntpService();

    void stop();

private:
    void on_network_event(const fujinet::net::NetworkEvent& ev);

    static void on_time_sync_cb(struct timeval* tv);

private:
    fujinet::core::SystemEvents& _events;

    fujinet::core::EventStream<fujinet::net::NetworkEvent>::Subscription _sub{};
    bool _started{false};

    static SntpService* _instance;
};

} // namespace fujinet::platform::esp32
