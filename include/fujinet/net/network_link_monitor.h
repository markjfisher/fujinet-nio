#pragma once

#include "fujinet/core/system_events.h"
#include "fujinet/net/network_link.h"
#include "fujinet/core/logging.h"

#include <string>

namespace fujinet::net {

static const char* TAG = "events";


/**
 * Observes an INetworkLink and publishes NetworkEvents on transitions.
 *
 * This keeps Wi-Fi link code platform-focused (state machine only),
 * and pushes side-effects (starting services) into event subscribers.
 */
class NetworkLinkMonitor {
public:
    NetworkLinkMonitor(fujinet::core::SystemEvents& events, fujinet::net::INetworkLink& link)
        : _events(events), _link(link)
    {
    }

    void poll()
    {
        const auto st = _link.state();
        const auto ip = _link.ip_address();

        // LinkUp: first time we see Connecting/Connected from Disconnected/Failed
        if ((_lastState == LinkState::Disconnected || _lastState == LinkState::Failed) &&
            (st == LinkState::Connecting || st == LinkState::Connected))
        {
            fujinet::net::NetworkEvent ev;
            ev.kind = fujinet::net::NetworkEventKind::LinkUp;
            _events.network().publish(ev);
        }

        // GotIp: transition into Connected or IP changed while Connected
        if (st == LinkState::Connected) {
            if (!_everConnected || ip != _lastIp) {
                fujinet::net::NetworkEvent ev;
                ev.kind = fujinet::net::NetworkEventKind::GotIp;
                ev.gotIp.ip4 = ip;
                FN_LOGI(TAG, "sending LinkState::Connected event");
                _events.network().publish(ev);
                _everConnected = true;
            }
        }

        // LinkDown: leaving Connected/Connecting to Disconnected/Failed
        if (( _lastState == LinkState::Connected || _lastState == LinkState::Connecting ) &&
            ( st == LinkState::Disconnected || st == LinkState::Failed ))
        {
            fujinet::net::NetworkEvent ev;
            ev.kind = fujinet::net::NetworkEventKind::LinkDown;
            _events.network().publish(ev);

            // reset per-connection flags
            _everConnected = false;
        }

        _lastState = st;
        _lastIp = ip;
    }

private:
    fujinet::core::SystemEvents& _events;
    fujinet::net::INetworkLink&  _link;

    LinkState _lastState{LinkState::Disconnected};
    std::string _lastIp{};
    bool _everConnected{false};
};

} // namespace fujinet::net
