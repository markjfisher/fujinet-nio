#pragma once

#include "fujinet/core/event_stream.h"
#include "fujinet/net/network_events.h"
#include "fujinet/time/time_events.h"

namespace fujinet::core {

/**
 * Project-wide event streams.
 * Owned by FujinetCore to keep core as the integration point.
 */
class SystemEvents {
public:
    EventStream<fujinet::net::NetworkEvent>& network() noexcept { return _network; }
    EventStream<fujinet::time::TimeEvent>&   time() noexcept { return _time; }

    const EventStream<fujinet::net::NetworkEvent>& network() const noexcept { return _network; }
    const EventStream<fujinet::time::TimeEvent>&   time() const noexcept { return _time; }

private:
    EventStream<fujinet::net::NetworkEvent> _network;
    EventStream<fujinet::time::TimeEvent>   _time;
};

} // namespace fujinet::core
