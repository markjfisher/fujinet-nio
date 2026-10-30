#include "pico/unique_id.h"
#include "pico/usb_reset.h"
#include "tusb.h"
#include <string.h>

/* Raspberry Pi's VID with the reset interface lets picotool -f reboot us
 * into BOOTSEL without touching the board. */
enum { ITF_NIO, ITF_NIO_DATA, ITF_CON, ITF_CON_DATA, ITF_RESET, ITF_TOTAL };
enum {
    STR_LANG,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_NIO,
    STR_CON,
    STR_RESET
};
#define CONFIG_LEN                                                             \
    (TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN + TUD_RPI_RESET_DESC_LEN)

static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x2E8A,
    .idProduct = 0x0009,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_LEN, 0, 250),
    TUD_CDC_DESCRIPTOR(ITF_NIO, STR_NIO, 0x81, 8, 0x02, 0x82, 64),
    TUD_CDC_DESCRIPTOR(ITF_CON, STR_CON, 0x83, 8, 0x04, 0x84, 64),
    TUD_RPI_RESET_DESCRIPTOR(ITF_RESET, STR_RESET),
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return configuration;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t out[40];
    static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char *s;
    unsigned n;
    switch (index) {
    case STR_LANG:
        out[1] = 0x0409;
        out[0] = (uint16_t)(TUSB_DESC_STRING << 8 | 4);
        return out;
    case STR_MANUFACTURER:
        s = "FujiNet";
        break;
    case STR_PRODUCT:
        s = "Springboard NIO";
        break;
    case STR_SERIAL:
        if (!serial[0])
            pico_get_unique_board_id_string(serial, sizeof serial);
        s = serial;
        break;
    case STR_NIO:
        s = "NIO link";
        break;
    case STR_CON:
        s = "Console";
        break;
    case STR_RESET:
        s = "Reset";
        break;
    default:
        return NULL;
    }
    n = (unsigned)strlen(s);
    if (n > 39)
        n = 39;
    for (unsigned i = 0; i < n; ++i)
        out[1 + i] = (uint8_t)s[i];
    out[0] = (uint16_t)(TUSB_DESC_STRING << 8 | (2 * n + 2));
    return out;
}
