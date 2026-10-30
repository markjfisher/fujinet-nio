/* Springboard bridge link ABI, shared by the RP2350 firmware and Palm code.
 *
 * CS0 is only the module's ROM. CS1 (HsCardAttrGet CsBase + CsSize) is the
 * link to fujinet-nio, both ways, with plain 16-bit MOVE.W cycles:
 *
 *   read   CS1 + offset, A1-A15 (the 64 KiB window repeats above that):
 *          registers from 0x0000, the reply at SBN_R_RESP.
 *   write  CS1 + SBN_W(reg): A16-A23 pick the register and all 16 data bits
 *          are captured; A1-A15 are ignored.
 *
 * A request is one raw FujiBus packet: SBN_W_BEGIN with its length in bytes,
 * the bytes two per SBN_W_DATA word (first byte in D15-D8, an odd last byte
 * padded with 0), SBN_W_CHECK with the SBN_CHECK_STEP sum of those words, then
 * SBN_W_DOORBELL with a sequence 1-255. The firmware sends it to NIO over NIO's
 * packet link and publishes the raw reply packet, its length, result and
 * check, then RESP_SEQ last; the Palm polls RESP_SEQ.
 */
#ifndef SBNIO_H
#define SBNIO_H

/* clang-format off */
#define SBN_MAGIC_VALUE   0x464E   /* "FN" */
#define SBN_ABI_VERSION   0x0004

/* Registers, read as byte offsets from the CS1 base */
#define SBN_R_MAGIC       0x0000
#define SBN_R_VERSION     0x0002
#define SBN_R_STATUS      0x0004
#define SBN_R_ACK_SEQ     0x0006   /* last doorbell sequence received */
#define SBN_R_RESP_SEQ    0x0008   /* sequence of the reply now in RESP */
#define SBN_R_RESP_LEN    0x000A   /* reply length in bytes */
#define SBN_R_RESULT      0x000C   /* SBN_RES_* for RESP_SEQ */
#define SBN_R_RESP_CHECK  0x000E   /* SBN_CHECK_STEP sum of the reply words */
#define SBN_R_WRITES      0x0010   /* CS1 writes captured, low 16 bits */
#define SBN_R_ECHO        0x0012   /* last SBN_W_ECHO word */
#define SBN_R_REQ_BYTES   0x0014   /* request bytes since the last BEGIN */
#define SBN_R_TICKS       0x0016   /* firmware milliseconds, low 16 bits */
#define SBN_R_LAST        SBN_R_TICKS
#define SBN_R_RESP        0x1000   /* reply bytes, two per word, first in D15-D8 */
#define SBN_RESP_MAX      8192
#define SBN_REQ_MAX       8192

/* SBN_R_STATUS bits */
#define SBN_ST_HOST       0x0001   /* NIO has the USB link port open */
#define SBN_ST_BUSY       0x0002   /* a request is waiting for its reply */
#define SBN_ST_LOOPBACK   0x0004   /* requests are echoed back without NIO */

/* SBN_R_RESULT values */
#define SBN_RES_OK        0
#define SBN_RES_NO_HOST   1        /* NIO is not running */
#define SBN_RES_TIMEOUT   2        /* NIO sent nothing back */
#define SBN_RES_BAD_LEN   3        /* length 0, over SBN_REQ_MAX, or not what BEGIN said */
#define SBN_RES_BAD_CHECK 4        /* request words do not match SBN_W_CHECK: resend */
#define SBN_RES_TOO_BIG   5        /* reply longer than SBN_RESP_MAX */
#define SBN_RES_ABORTED   6
#define SBN_RES_UNANSWERED 7       /* NIO took it but sent no answer: it may have run */
#define SBN_RES_REFUSED   8        /* NIO's link refused it: it did not run */

/* Write registers: byte offset from the CS1 base */
#define SBN_W(reg)        ((unsigned long)(reg) << 16)
#define SBN_W_DATA        0        /* two request bytes */
#define SBN_W_BEGIN       1        /* start a request of this many bytes */
#define SBN_W_CHECK       2        /* SBN_CHECK_STEP sum of the request words */
#define SBN_W_DOORBELL    3        /* low byte = sequence: send the request */
#define SBN_W_CONTROL     4        /* SBN_CTL_* */
#define SBN_W_LOG         5        /* two characters for the firmware console, 0 = none */
#define SBN_W_ECHO        6        /* copied to SBN_R_ECHO */

#define SBN_CTL_ABORT        1
#define SBN_CTL_LOOPBACK_ON  2
#define SBN_CTL_LOOPBACK_OFF 3
/* clang-format on */

/* Check over 16-bit words, from 0: rotate left one bit, then add the word.
 * Unlike a plain sum it also catches words swapped or shifted. */
#define SBN_CHECK_STEP(c, w)                                                   \
    ((unsigned short)((((unsigned short)(c) << 1) |                            \
                       ((unsigned short)(c) >> 15)) +                          \
                      (unsigned short)(w)))

#endif
