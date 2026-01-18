#include "fujinet/core/bootstrap.h"

#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/io/transport/legacy/sio_transport.h"
#include "fujinet/io/transport/legacy/iwm_transport.h"
// #include "fujinet/io/transport/iec_transport.h"
// etc.

namespace fujinet::core {

io::ITransport* setup_transports(FujinetCore& core,
                                 io::Channel& channel,
                                 const build::BuildProfile& profile)
{
    using build::TransportKind;
    io::ITransport* primary = nullptr;

    switch (profile.primaryTransport) {
    case TransportKind::FujiBus: {
        auto* t = new io::FujiBusTransport(channel);
        core.addTransport(t);
        primary = t;
        break;
    }
    case TransportKind::SIO: {
        auto* t = new io::transport::legacy::SioTransport(channel);
        core.addTransport(t);
        primary = t;
        break;
    }
    case TransportKind::IWM: {
        auto* t = new io::transport::legacy::IwmTransport(channel);
        core.addTransport(t);
        primary = t;
        break;
    }
    case TransportKind::IEC: {
        // ...
        break;
    }
    }

    return primary;
}

} // namespace fujinet::core
