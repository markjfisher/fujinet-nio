#include "fujinet/platform/esp32/pinmap.h"
#include "fujinet/core/logging.h"

namespace fujinet::platform::esp32 {

// Compile-time selector values, *visible to the preprocessor*.
#define FN_PINMAP_BREADBOARD        1
#define FN_PINMAP_FUJINET_S3_REV_A  2
// Add more here as they are used. These ensure we don't get weird macro expansions.

// Ensure at least one of FN_PINMAP or FN_PINMAP_DEFAULT is defined
// The FN_PINMAP is an override that can be used in platromio.local.ini
#if !defined(FN_PINMAP) && !defined(FN_PINMAP_DEFAULT)
#  error "FN_PINMAP or FN_PINMAP_DEFAULT must be defined"
#endif

#ifndef FN_PINMAP
#  define FN_PINMAP FN_PINMAP_DEFAULT
#endif

#if FN_PINMAP == FN_PINMAP_BREADBOARD
static constexpr PinMap SD_PINMAP{
    .sd = SdSpiPins{ .mosi = 7, .miso = 8, .sck = 6, .cs = 9 },
};

#elif FN_PINMAP == FN_PINMAP_FUJINET_S3_REV_A
static constexpr PinMap SD_PINMAP{
    .sd = SdSpiPins{ .mosi = 35, .miso = 37, .sck = 36, .cs = 34 },
};

#else
#  error "Invalid FN_PINMAP value"
#endif

const PinMap& pinmap()
{
    return SD_PINMAP;
}

} // namespace fujinet::platform::esp32
