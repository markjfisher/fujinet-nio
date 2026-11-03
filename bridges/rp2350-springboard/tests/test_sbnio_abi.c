/* Host test of the CS1 link ABI (include/sbnio.h) and the sizes the firmware
 * builds on it. ABI 4 values are spelled out here on purpose: changing one
 * means bumping SBN_ABI_VERSION and the Palm library with it. */
#include "bus.h"
#include "packet_link.h"
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

_Static_assert(SBN_MAGIC_VALUE == 0x464E, "magic is \"FN\"");
_Static_assert(SBN_ABI_VERSION == 4, "ABI 4");

/* Read registers sit below the reply, even-aligned, in a fixed order. */
_Static_assert(SBN_R_MAGIC == 0x00 && SBN_R_VERSION == 0x02, "ABI 4");
_Static_assert(SBN_R_STATUS == 0x04 && SBN_R_ACK_SEQ == 0x06, "ABI 4");
_Static_assert(SBN_R_RESP_SEQ == 0x08 && SBN_R_RESP_LEN == 0x0A, "ABI 4");
_Static_assert(SBN_R_RESULT == 0x0C && SBN_R_RESP_CHECK == 0x0E, "ABI 4");
_Static_assert(SBN_R_WRITES == 0x10 && SBN_R_ECHO == 0x12, "ABI 4");
_Static_assert(SBN_R_REQ_BYTES == 0x14 && SBN_R_TICKS == 0x16, "ABI 4");
_Static_assert(SBN_R_LAST == SBN_R_TICKS, "last register");
_Static_assert(SBN_R_RESP == 0x1000, "ABI 4");
_Static_assert(SBN_R_LAST + 2 <= SBN_R_RESP, "registers sit below SBN_R_RESP");
_Static_assert((SBN_R_RESP & 1) == 0 && (SBN_RESP_MAX & 1) == 0,
               "the reply is whole words");

/* CS1 reads use A1-A15: a 64 KiB window of 16-bit words, and the firmware's
 * register table is exactly that window. */
_Static_assert(BUS_TABLE_WORDS * 2 == 0x10000, "64 KiB window");
_Static_assert(SBN_R_RESP + SBN_RESP_MAX <= 0x10000,
               "the reply fits in the 64 KiB window");
_Static_assert(SBN_R_RESP + SBN_RESP_MAX <= 2 * BUS_TABLE_WORDS,
               "the reply fits in the firmware's table");

/* Lengths travel in 16-bit registers and become one packet-link record. */
_Static_assert(SBN_REQ_MAX == 8192 && SBN_RESP_MAX == 8192, "ABI 4");
_Static_assert(SBN_REQ_MAX <= 0xFFFF && SBN_RESP_MAX <= 0xFFFF,
               "16-bit lengths");
_Static_assert(SBN_REQ_MAX <= PL_MAX_BODY && SBN_RESP_MAX <= PL_MAX_BODY,
               "a request or reply fits one packet-link record");

/* Write registers: A16-A23, so 8 bits, at 64 KiB spacing. */
_Static_assert(SBN_W_DATA == 0 && SBN_W_BEGIN == 1 && SBN_W_CHECK == 2,
               "ABI 4");
_Static_assert(SBN_W_DOORBELL == 3 && SBN_W_CONTROL == 4, "ABI 4");
_Static_assert(SBN_W_LOG == 5 && SBN_W_ECHO == 6, "ABI 4");
_Static_assert(SBN_W(SBN_W_DATA) == 0x00000 && SBN_W(SBN_W_BEGIN) == 0x10000,
               "write offsets");
_Static_assert(SBN_W(SBN_W_CHECK) == 0x20000 &&
                   SBN_W(SBN_W_DOORBELL) == 0x30000,
               "write offsets");
_Static_assert(SBN_W(SBN_W_CONTROL) == 0x40000 && SBN_W(SBN_W_LOG) == 0x50000 &&
                   SBN_W(SBN_W_ECHO) == 0x60000,
               "write offsets");
_Static_assert(SBN_W(255) == 0xFF0000,
               "A16-A23 are the top eight address bits");

_Static_assert(SBN_ST_HOST == 1 && SBN_ST_BUSY == 2 && SBN_ST_LOOPBACK == 4,
               "ABI 4");
_Static_assert(SBN_RES_OK == 0 && SBN_RES_NO_HOST == 1 &&
                   SBN_RES_TIMEOUT == 2 && SBN_RES_BAD_LEN == 3 &&
                   SBN_RES_BAD_CHECK == 4 && SBN_RES_TOO_BIG == 5 &&
                   SBN_RES_ABORTED == 6 && SBN_RES_UNANSWERED == 7 &&
                   SBN_RES_REFUSED == 8,
               "ABI 4");
_Static_assert(SBN_CTL_ABORT == 1 && SBN_CTL_LOOPBACK_ON == 2 &&
                   SBN_CTL_LOOPBACK_OFF == 3,
               "ABI 4");

static uint16_t check_words(const uint16_t *w, unsigned n) {
    uint16_t c = 0;
    while (n--)
        c = SBN_CHECK_STEP(c, *w++);
    return c;
}

int main(void) {
    /* Expected values worked out by hand: rotate left one, then add. */
    CHECK(SBN_CHECK_STEP(0, 0x1234) == 0x1234);
    CHECK(SBN_CHECK_STEP(1, 2) == 4);
    CHECK(SBN_CHECK_STEP(0x8000, 0) == 1); /* the top bit rotates round */
    CHECK(SBN_CHECK_STEP(0xFFFF, 1) == 0); /* the sum wraps at 16 bits */
    /* The argument is truncated to 16 bits, as on the Palm's int. */
    CHECK(SBN_CHECK_STEP(0x10001u, 0x20002u) == SBN_CHECK_STEP(1, 2));

    const uint16_t clock_get_time[] = {0x4501, 0x0600, 0x4C00};
    CHECK(check_words(clock_get_time, 3) == 0x6C05);
    const uint16_t ones[] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    CHECK(check_words(ones, 4) == 0xFFF8);
    const uint16_t alternating[] = {0xAAAA, 0x5555, 0xAAAA, 0x5555};
    CHECK(check_words(alternating, 4) == 0x5554);

    /* What a plain sum cannot see: words swapped, or moved by one place. */
    const uint16_t ab[] = {0x1234, 0x5678}, ba[] = {0x5678, 0x1234};
    CHECK(check_words(ab, 2) == 0x7AE0 && check_words(ba, 2) == 0xBF24);
    const uint16_t x00[] = {1, 0, 0}, o0x[] = {0, 1, 0}, oox[] = {0, 0, 1};
    CHECK(check_words(x00, 3) == 4 && check_words(o0x, 3) == 2 &&
          check_words(oox, 3) == 1);

    /* A write to CS1 + SBN_W(reg) selects register reg through A16-A23. */
    for (unsigned reg = 0; reg < 256; ++reg)
        CHECK((SBN_W(reg) >> 16 & 0xFF) == reg && (SBN_W(reg) & 0xFFFF) == 0);

    printf(failures ? "sbnio_abi: %d failures\n" : "sbnio_abi: ok\n", failures);
    return failures != 0;
}
