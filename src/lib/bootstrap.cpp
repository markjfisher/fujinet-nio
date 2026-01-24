#include "fujinet/core/bootstrap.h"

#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/config/fuji_config.h"
// #include "fujinet/io/transport/iec_transport.h"
// etc.

#if defined(FN_BUILD_ATARI_SIO) || defined(FN_BUILD_ATARI_PTY) || defined(FN_BUILD_ATARI_NETSIO)
#include "fujinet/io/transport/legacy/sio_transport.h"
#define FN_HAS_LEGACY_SIO 1
#endif

// Future: enable when Apple II legacy transport is actually built.
// #if defined(FN_BUILD_APPLE_IWM)
// #include "fujinet/io/transport/legacy/iwm_transport.h"
// #define FN_HAS_LEGACY_IWM 1
// #endif

#if defined(FN_ENABLE_LEGACY_TRANSPORT) || defined(FN_HAS_LEGACY_SIO) || defined(FN_HAS_LEGACY_IWM)
#include "fujinet/io/legacy/legacy_network_adapter.h"
#endif

namespace fujinet::core {

io::ITransport* setup_transports(FujinetCore& core,
                                 io::Channel& channel,
                                 const build::BuildProfile& profile,
                                 const config::FujiConfig* config)
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
#if defined(FN_HAS_LEGACY_SIO)
        // Pass NetSIO config if available (should always be available)
        const config::NetSioConfig* netsioConfig = config ? &config->netsio : nullptr;
        auto* t = new io::transport::legacy::SioTransport(channel, profile, netsioConfig);
        core.addTransport(t);
        primary = t;
#else
        // SIO transport not compiled for this build.
        break;
#endif
        break;
    }
    case TransportKind::IWM: {
// #if defined(FN_HAS_LEGACY_IWM)
//         auto* t = new io::transport::legacy::IwmTransport(channel);
//         core.addTransport(t);
//         primary = t;
//         break;
// #else
        break;
// #endif
    }
    case TransportKind::IEC: {
        // ...
        break;
    }
    }

#if defined(FN_HAS_LEGACY_SIO) || defined(FN_HAS_LEGACY_IWM)
    // Only install legacy routing overrides when legacy transports are in use.
    // This keeps non-legacy builds free of legacy feature overhead.
    if (profile.primaryTransport == TransportKind::SIO ||
        profile.primaryTransport == TransportKind::IWM ||
        profile.primaryTransport == TransportKind::IEC) {
        core.routingManager().setOverrideHandler(
            std::make_unique<fujinet::io::legacy::LegacyNetworkAdapter>(core.deviceManager())
        );
    }
#endif

    return primary;
}

} // namespace fujinet::core
