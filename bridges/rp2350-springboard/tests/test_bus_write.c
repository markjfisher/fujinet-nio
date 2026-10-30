/* Host test of the CS1 write snapshot decoding in bus.h. The snapshots are
 * built pin by pin from the carrier pinout, not from the macros' shifts. */
#include "bus.h"
#include "sbnio.h"
#include <stdio.h>

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

/* A snapshot is GPIO16-47, so GPIO n is bit n - 16. */
#define PIN(n) (UINT32_C(1) << ((n) - 16))
#define D0_GPIO 20 /* D0-D15 on GPIO20-35 */
static const uint8_t addr_gpio[8] = {36, 37, 38, 40,
                                     41, 42, 43, 44}; /* A16-A23 */

/* Pins the decoding must ignore: /CS0 (16), /CS1 (17), /OE (18), /WE (19),
 * the LED (39), RESET (45), LOWBAT (46) and GPIO47. */
static const uint32_t noise[] = {
    0,
    PIN(16) | PIN(18) | PIN(45), /* a CS1 write: CS0 and OE high, WE low */
    PIN(16) | PIN(17) | PIN(18) | PIN(19) | PIN(39) | PIN(45) | PIN(46) |
        PIN(47),
};

static uint32_t snapshot(uint16_t data, uint8_t reg, uint32_t other) {
    uint32_t w = other;
    for (unsigned bit = 0; bit < 16; ++bit)
        if (data >> bit & 1)
            w |= PIN(D0_GPIO + bit);
    for (unsigned bit = 0; bit < 8; ++bit)
        if (reg >> bit & 1)
            w |= PIN(addr_gpio[bit]);
    return w;
}

static void check_decode(uint16_t data, uint8_t reg) {
    for (unsigned n = 0; n < sizeof noise / sizeof noise[0]; ++n) {
        uint32_t w = snapshot(data, reg, noise[n]);
        CHECK(BUS_WRITE_DATA(w) == data);
        CHECK(BUS_WRITE_REG(w) == reg);
    }
}

int main(void) {
    /* Spot checks with the bit positions written out. */
    CHECK(BUS_WRITE_DATA(UINT32_C(0x000FFFF0)) == 0xFFFF);
    CHECK(BUS_WRITE_DATA(UINT32_C(0x00000010)) == 0x0001); /* D0 is bit 4 */
    CHECK(BUS_WRITE_DATA(UINT32_C(0x00080000)) == 0x8000); /* D15 is bit 19 */
    CHECK(BUS_WRITE_REG(UINT32_C(0x00100000)) == 0x01);    /* A16 is bit 20 */
    CHECK(BUS_WRITE_REG(UINT32_C(0x00400000)) == 0x04);    /* A18 is bit 22 */
    CHECK(BUS_WRITE_REG(UINT32_C(0x00800000)) == 0x00); /* bit 23 is the LED */
    CHECK(BUS_WRITE_REG(UINT32_C(0x01000000)) == 0x08); /* A19 is bit 24 */
    CHECK(BUS_WRITE_REG(UINT32_C(0x10000000)) == 0x80); /* A23 is bit 28 */
    CHECK(BUS_WRITE_REG(UINT32_C(0xE0000000)) == 0x00); /* GPIO45-47 */

    /* Every register against walking and patterned data. */
    static const uint16_t patterns[] = {0x0000, 0xFFFF, 0xAAAA, 0x5555,
                                        0x1234, 0xBEEF, 0x00FF, 0xFF00};
    for (unsigned reg = 0; reg < 256; ++reg) {
        for (unsigned bit = 0; bit < 16; ++bit)
            check_decode((uint16_t)(1u << bit), (uint8_t)reg);
        for (unsigned i = 0; i < sizeof patterns / sizeof patterns[0]; ++i)
            check_decode(patterns[i], (uint8_t)reg);
    }
    /* All data values on registers of alternating bits. */
    for (unsigned data = 0; data < 0x10000; ++data) {
        check_decode((uint16_t)data, 0x5A);
        check_decode((uint16_t)data, 0xA5);
    }

    /* A write to CS1 + SBN_W(reg) puts A16-A23 on the carrier's address pins
     * and is decoded as register reg. */
    for (unsigned reg = 0; reg < 256; ++reg) {
        unsigned long address = SBN_W(reg);
        uint32_t w = noise[1];
        for (unsigned bit = 0; bit < 8; ++bit)
            if (address >> (16 + bit) & 1)
                w |= PIN(addr_gpio[bit]);
        CHECK(BUS_WRITE_REG(w) == reg);
    }

    printf(failures ? "bus_write: %d failures\n" : "bus_write: ok\n", failures);
    return failures != 0;
}
