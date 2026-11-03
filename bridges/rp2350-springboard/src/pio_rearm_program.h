/* From mozzwald/fujinet-nio experiment/dual-cs-write-capture, bridges/rp2350-springboard/src.
 * Unchanged. */
#pragma once
#include <stdbool.h>
#ifndef SB_PIO_REPLY_MARKER
#define SB_PIO_REPLY_MARKER 0
#endif
enum { SB_PR_MAIN=0, SB_PR_SNAPSHOT=4, SB_PR_REQUEST=4,
       SB_PR_PULL=5, SB_PR_PRELOAD=6, SB_PR_ENABLE=12, SB_PR_END=18,
       SB_PR_COMPLETE=19, SB_PR_WATCH=21, SB_PR_OE=22, SB_PR_RESET=27,
       SB_PR_INSTRUCTIONS=32, SB_PR_FALLBACK=29 };
/* RX0 strictly alternates address snapshot / completion token. TX0 has one
 * reply per address, including a cancelled dummy for an invalid snapshot.
 * No runtime SM restart or FIFO flush between transactions. */
void sb_pio_rearm_program_init(bool enable);
