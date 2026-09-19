#include "stimulus.h"
#include <apio.h>

/* W1 packs D[15:0], /AS, R/W, /UDS, /LDS and SELECT into contiguous
 * GP2..22 output bits. DMA supplies complete bus states; PIO alone changes
 * them at the programmed cadence. */
#define AS     (1u << 16)
#define RW     (1u << 17)
#define UDS    (1u << 18)
#define LDS    (1u << 19)
#define SELECT (1u << 20)
#define IDLE   (AS | RW | UDS | LDS)
#define WRITE_HIGH(v) ((uint32_t)(v) | AS | SELECT)
#define WRITE_LOW(v)  ((uint32_t)(v) | SELECT)
#define UNSELECTED_HIGH(v) ((uint32_t)(v) | AS)
#define READ_HIGH(v) ((uint32_t)(v) | AS | RW | SELECT)
#define READ_LOW(v)  ((uint32_t)(v) | RW | SELECT)
#define LOWER_ONLY_HIGH(v) ((uint32_t)(v) | AS | UDS | SELECT)
#define LOWER_ONLY_LOW(v)  ((uint32_t)(v) | UDS | SELECT)
#define UPPER_ONLY_HIGH(v) ((uint32_t)(v) | AS | LDS | SELECT)
#define UPPER_ONLY_LOW(v)  ((uint32_t)(v) | LDS | SELECT)

/* Invalid controls deliberately lead the state machine through its waits in
 * order. The following first valid write is the first possible capture. */
const uint32_t stimulus_output_words[STIMULUS_OUTPUT_WORD_COUNT] = {
    UNSELECTED_HIGH(0x1357), UNSELECTED_HIGH(0x1357) & ~AS,
    READ_HIGH(0x2468),       READ_LOW(0x2468),
    LOWER_ONLY_HIGH(0x369c), LOWER_ONLY_LOW(0x369c),
    UPPER_ONLY_HIGH(0x4abc), UPPER_ONLY_LOW(0x4abc),
    WRITE_HIGH(0x000a), WRITE_LOW(0x000a),
    WRITE_HIGH(0x00a5), WRITE_LOW(0x00a5),
    WRITE_HIGH(0x0001), WRITE_LOW(0x0001),
    WRITE_HIGH(0x0002), WRITE_LOW(0x0002),
    WRITE_HIGH(0x0004), WRITE_LOW(0x0004),
    WRITE_HIGH(0x0008), WRITE_LOW(0x0008),
    WRITE_HIGH(0x0010), WRITE_LOW(0x0010),
    WRITE_HIGH(0x0020), WRITE_LOW(0x0020),
    WRITE_HIGH(0x0040), WRITE_LOW(0x0040),
    WRITE_HIGH(0x0080), WRITE_LOW(0x0080),
    WRITE_HIGH(0x0100), WRITE_LOW(0x0100),
    WRITE_HIGH(0x0200), WRITE_LOW(0x0200),
    WRITE_HIGH(0x0400), WRITE_LOW(0x0400),
    WRITE_HIGH(0x0800), WRITE_LOW(0x0800),
    WRITE_HIGH(0x1000), WRITE_LOW(0x1000),
    WRITE_HIGH(0x2000), WRITE_LOW(0x2000),
    WRITE_HIGH(0x4000), WRITE_LOW(0x4000),
    WRITE_HIGH(0x8000), WRITE_LOW(0x8000),
    IDLE,
};

void stimulus_program(uint16_t words[STIMULUS_WORDS]) {
    words[0] = APIO_SET_Y(21); /* 22 high/low transaction pairs. */
    words[1] = APIO_PULL_BLOCK;
    words[2] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 9); /* high/setup: 100 us */
    words[3] = APIO_PULL_BLOCK;
    words[4] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 9); /* /AS phase: 100 us */
    words[5] = APIO_JMP_Y_DEC(1);
    words[6] = APIO_PULL_BLOCK;
    words[7] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 9);
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
