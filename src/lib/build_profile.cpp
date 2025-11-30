#include "fujinet/config/build_profile.h"

namespace fujinet::config {

BuildProfile current_build_profile()
{
#if defined(FN_BUILD_ATARI)
    return BuildProfile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .name             = "Atari + SIO",
    };
#elif defined(FN_BUILD_RS232)
    return BuildProfile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::SerialDebug,
        .name             = "Generic + SerialDebug",
    };
#else
    // Default POSIX dev build, etc.
    return BuildProfile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::SerialDebug,
        .name             = "POSIX + SerialDebug",
    };
#endif
}

} // namespace fujinet::config
