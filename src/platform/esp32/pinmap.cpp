#include "fujinet/platform/esp32/pinmap.h"
#include "fujinet/core/logging.h"

namespace fujinet::platform::esp32 {

// Compile-time selector values, *visible to the preprocessor*.
#define FN_PINMAP_BREADBOARD        1
#define FN_PINMAP_FREENOVA_S3       2
#define FN_PINMAP_FUJINET_S3_REV_A  3
#define FN_PINMAP_ATARIV1           4
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
    .sio = SioPins{ .cmd = -1, .int_pin = -1, .mtr = -1, .proc = -1, .cki = -1, .cko = -1, .uart_rx = -1, .uart_tx = -1 },
};

#elif FN_PINMAP == FN_PINMAP_FREENOVA_S3
static constexpr PinMap SD_PINMAP{
    .sd = SdSpiPins{ .mosi = 38, .miso = 40, .sck = 39, .cs = 41 },
    .sio = SioPins{ .cmd = -1, .int_pin = -1, .mtr = -1, .proc = -1, .cki = -1, .cko = -1, .uart_rx = -1, .uart_tx = -1 },
};

#elif FN_PINMAP == FN_PINMAP_FUJINET_S3_REV_A
static constexpr PinMap SD_PINMAP{
    .sd = SdSpiPins{ .mosi = 35, .miso = 37, .sck = 36, .cs = 34 },
    .sio = SioPins{ .cmd = -1, .int_pin = -1, .mtr = -1, .proc = -1, .cki = -1, .cko = -1, .uart_rx = -1, .uart_tx = -1 },
};

#elif FN_PINMAP == FN_PINMAP_ATARIV1
// FujiNet v1 (ESP32) pin mapping for Atari SIO
// Based on atari-common.h: INT=26, PROC=22, CKO=32, CKI=27, MTR=36, CMD=39
// UART2 pins: RX=33, TX=21 (from common.h)
static constexpr PinMap SD_PINMAP{
    .sd = SdSpiPins{ .mosi = 23, .miso = 19, .sck = 18, .cs = 5 },
    .sio = SioPins{
        .cmd = 39,      // GPIO_NUM_39 (CMD line)
        .int_pin = 26,  // GPIO_NUM_26 (INT line)
        .mtr = 36,      // GPIO_NUM_36 (MTR line)
        .proc = 22,     // GPIO_NUM_22 (PROC line)
        .cki = 27,      // GPIO_NUM_27 (CKI line)
        .cko = 32,      // GPIO_NUM_32 (CKO line)
        .uart_rx = 33,  // GPIO_NUM_33 (UART2 RX)
        .uart_tx = 21,  // GPIO_NUM_21 (UART2 TX)
    },
};

#else
#  error "Invalid FN_PINMAP value"
#endif

const PinMap& pinmap()
{
    return SD_PINMAP;
}

} // namespace fujinet::platform::esp32
