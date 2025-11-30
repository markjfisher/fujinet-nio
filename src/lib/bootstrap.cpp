#include "fujinet/core/bootstrap.h"

#include "fujinet/io/transport/rs232_transport.h"
// #include "fujinet/io/transport/sio_transport.h"
// #include "fujinet/io/transport/iec_transport.h"
// etc.

namespace fujinet::core {

io::ITransport* setup_transports(FujinetCore& core,
                                 io::Channel& channel,
                                 const config::BuildProfile& profile)
{
    using config::TransportKind;
    io::ITransport* primary = nullptr;

    switch (profile.primaryTransport) {
    case TransportKind::SerialDebug: {
        auto* t = new io::Rs232Transport(channel);
        core.addTransport(t);
        primary = t;
        break;
    }
    case TransportKind::SIO: {
        // auto* t = new io::SioTransport(channel);
        // core.addTransport(t);
        // primary = t;
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
