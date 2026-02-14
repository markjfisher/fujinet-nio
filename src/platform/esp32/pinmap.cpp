#include "fujinet/platform/esp32/pinmap.h"
#include "fujinet/core/logging.h"

namespace fujinet::platform::esp32 {

// Compile-time selector values, *visible to the preprocessor*.
#define FN_PINMAP_BREADBOARD        1
#define FN_PINMAP_FREENOVA_S3       2
#define FN_PINMAP_FUJINET_S3_REV_A  3
#define FN_PINMAP_ATARIV1           4
#define FN_PINMAP_RS232             5
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
static constexpr PinMap BOARD_PINMAP{
    .sd = SdSpiPins{ .mosi = 7, .miso = 8, .sck = 6, .cs = 9 },
};

#elif FN_PINMAP == FN_PINMAP_FREENOVA_S3
static constexpr PinMap BOARD_PINMAP{
    .sd = SdSpiPins{ .mosi = 38, .miso = 40, .sck = 39, .cs = 41 },
};

#elif FN_PINMAP == FN_PINMAP_FUJINET_S3_REV_A
static constexpr PinMap BOARD_PINMAP{
    .sd = SdSpiPins{ .mosi = 35, .miso = 37, .sck = 36, .cs = 34 },
};

#elif FN_PINMAP == FN_PINMAP_ATARIV1
// FujiNet v1 (ESP32) pin mapping for Atari SIO
// Based on atari-common.h: INT=26, PROC=22, CKO=32, CKI=27, MTR=36, CMD=39
// UART2 pins: RX=33, TX=21 (from common.h)
static constexpr PinMap BOARD_PINMAP{
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

#elif FN_PINMAP == FN_PINMAP_RS232
// FujiNet RS232 S3 pin mapping
// Based on rs232_s3.h from fujinet-firmware
static constexpr PinMap BOARD_PINMAP{
    .sd = SdSpiPins{
        .mosi = 11,     // GPIO_NUM_11 (PIN_SD_HOST_MOSI)
        .miso = 13,     // GPIO_NUM_13 (PIN_SD_HOST_MISO)
        .sck = 21,      // GPIO_NUM_21 (PIN_SD_HOST_SCK)
        .cs = 10,       // GPIO_NUM_10 (PIN_SD_HOST_CS)
    },
    .rs232 = Rs232Pins{
        .uart_rx = 41,  // GPIO_NUM_41 (PIN_UART1_RX)
        .uart_tx = 42,  // GPIO_NUM_42 (PIN_UART1_TX)
        .ri = 16,       // GPIO_NUM_16 (PIN_RS232_RI)
        .dcd = 4,       // GPIO_NUM_4 (PIN_RS232_DCD)
        .rts = 15,      // GPIO_NUM_15 (PIN_RS232_RTS)
        .cts = 7,       // GPIO_NUM_7 (PIN_RS232_CTS)
        .dtr = 6,       // GPIO_NUM_6 (PIN_RS232_DTR)
        .dsr = 5,       // GPIO_NUM_5 (PIN_RS232_DSR)
        .invalid = 18,  // GPIO_NUM_18 (PIN_RS232_INVALID)
    },
};

#else
#  error "Invalid FN_PINMAP value"
#endif

const PinMap& pinmap()
{
    return BOARD_PINMAP;
}

} // namespace fujinet::platform::esp32
