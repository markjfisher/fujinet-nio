#include "stimulus.h"
#include <apio.h>

/* GP2..17 are inputs throughout this read.  PIO owns only the W1 controls;
 * changing the 21-bit PINDIRS mask before the first OUT makes that electrical
 * release part of the deterministic sequence, not a CPU side effect. */
#define AS     (1u << 16)
#define RW     (1u << 17)
#define UDS    (1u << 18)
#define LDS    (1u << 19)
#define SELECT (1u << 20)
#define CONTROL_DIR (AS | RW | UDS | LDS | SELECT)
#define IDLE   CONTROL_DIR
#define READ_LOW (RW | UDS | LDS | SELECT)

const uint32_t stimulus_output_words[STIMULUS_OUTPUT_WORD_COUNT] = {
    CONTROL_DIR, IDLE, READ_LOW, IDLE,
};

void stimulus_program(uint16_t words[STIMULUS_WORDS]) {
    words[0] = APIO_PULL_BLOCK;
    words[1] = APIO_OUT_PINDIRS(21);
    words[2] = APIO_PULL_BLOCK;
    words[3] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 15);
    words[4] = APIO_PULL_BLOCK;
    /* OUT's 30-cycle delay plus the following PULL holds /AS low for the
     * declared 320 us at this 100 kHz PIO clock. */
    words[5] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 30);
    words[6] = APIO_PULL_BLOCK;
    words[7] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 15);
    words[8] = APIO_IRQ_SET(0);
    words[9] = APIO_JMP(9);
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
