#include "stimulus.h"
#include <apio.h>

/* W1 encodes D[15:0], /AS, R/W, /UDS, /LDS and SELECT in GP2..22. The
 * recovery sequence is followed by a PIO-only released delay: enough for the
 * cleanly separate the first sequence from four post-rearm sentinels. */
#define AS     (1u << 16)
#define RW     (1u << 17)
#define UDS    (1u << 18)
#define LDS    (1u << 19)
#define SELECT (1u << 20)
#define IDLE   (AS | RW | UDS | LDS)
#define WRITE_HIGH(v) ((uint32_t)(v) | AS | SELECT)
#define WRITE_LOW(v)  ((uint32_t)(v) | SELECT)
#define RECOVERY_PAIR(v) WRITE_HIGH(v), WRITE_LOW(v)

const uint32_t stimulus_output_words[STIMULUS_OUTPUT_WORD_COUNT] = {
    RECOVERY_PAIR(0x1000), RECOVERY_PAIR(0x1001),
    RECOVERY_PAIR(0x1002), RECOVERY_PAIR(0x1003),
    RECOVERY_PAIR(0x1004), RECOVERY_PAIR(0x1005),
    RECOVERY_PAIR(0x1006), RECOVERY_PAIR(0x1007),
    RECOVERY_PAIR(0x1008), RECOVERY_PAIR(0x1009),
    RECOVERY_PAIR(0x100a), RECOVERY_PAIR(0x100b),
    RECOVERY_PAIR(0x100c), RECOVERY_PAIR(0x100d),
    RECOVERY_PAIR(0x100e), RECOVERY_PAIR(0x100f),
    IDLE,
    RECOVERY_PAIR(0xd001), RECOVERY_PAIR(0xd002),
    RECOVERY_PAIR(0xd003), RECOVERY_PAIR(0xd004),
    IDLE,
};

void stimulus_program(uint16_t words[STIMULUS_WORDS]) {
    words[0] = APIO_SET_Y(15); /* Sixteen initial high/low pairs. */
    words[1] = APIO_PULL_BLOCK;
    words[2] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 9);
    words[3] = APIO_PULL_BLOCK;
    words[4] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 9);
    words[5] = APIO_JMP_Y_DEC(1);
    words[6] = APIO_PULL_BLOCK;
    words[7] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 9); /* released state */
    words[8] = APIO_SET_Y(7); /* Eight released guard iterations. */
    words[9] = APIO_ADD_DELAY(APIO_NOP, 31);
    words[10] = APIO_JMP_Y_DEC(9);
    words[11] = APIO_SET_Y(3); /* Four post-guard high/low pairs. */
    words[12] = APIO_PULL_BLOCK;
    words[13] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 9);
    words[14] = APIO_PULL_BLOCK;
    words[15] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 9);
    words[16] = APIO_JMP_Y_DEC(12);
    words[17] = APIO_PULL_BLOCK;
    words[18] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 9);
    words[19] = APIO_IRQ_SET(0);
    words[20] = APIO_JMP(20);
}

stimulus_registers stimulus_config(uint32_t system_hz) {
    return (stimulus_registers){
        .clkdiv = (uint32_t)(((uint64_t)system_hz * 256 / STIMULUS_HZ) << 8),
        .execctrl = (STIMULUS_WORDS - 1u) << 12,
        .shiftctrl = STIMULUS_SHIFTCTRL,
        .pinctrl = APIO_OUT_BASE(STIMULUS_OUTPUT_BASE) |
                   APIO_SET_BASE(STIMULUS_SET_BASE) |
                   APIO_OUT_COUNT(STIMULUS_OUTPUT_PINS) |
                   APIO_SET_COUNT(STIMULUS_SET_PINS),
    };
}
