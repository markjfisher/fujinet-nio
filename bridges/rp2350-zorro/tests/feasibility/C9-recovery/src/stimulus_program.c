#include "stimulus.h"
#include <apio.h>

/* W1 encodes D[15:0], /AS, R/W, /UDS, /LDS and SELECT in GP2..22. The
 * pressure series is followed by a PIO-only released delay: enough for the
 * 2 ms observer pause to end before the four recovery sentinels. */
#define AS     (1u << 16)
#define RW     (1u << 17)
#define UDS    (1u << 18)
#define LDS    (1u << 19)
#define SELECT (1u << 20)
#define IDLE   (AS | RW | UDS | LDS)
#define WRITE_HIGH(v) ((uint32_t)(v) | AS | SELECT)
#define WRITE_LOW(v)  ((uint32_t)(v) | SELECT)
#define PRESSURE_PAIR(v) WRITE_HIGH(v), WRITE_LOW(v)

const uint32_t stimulus_output_words[STIMULUS_OUTPUT_WORD_COUNT] = {
    PRESSURE_PAIR(0x1000), PRESSURE_PAIR(0x1001),
    PRESSURE_PAIR(0x1002), PRESSURE_PAIR(0x1003),
    PRESSURE_PAIR(0x1004), PRESSURE_PAIR(0x1005),
    PRESSURE_PAIR(0x1006), PRESSURE_PAIR(0x1007),
    PRESSURE_PAIR(0x1008), PRESSURE_PAIR(0x1009),
    PRESSURE_PAIR(0x100a), PRESSURE_PAIR(0x100b),
    PRESSURE_PAIR(0x100c), PRESSURE_PAIR(0x100d),
    PRESSURE_PAIR(0x100e), PRESSURE_PAIR(0x100f),
    IDLE,
    PRESSURE_PAIR(0xd001), PRESSURE_PAIR(0xd002),
    PRESSURE_PAIR(0xd003), PRESSURE_PAIR(0xd004),
    IDLE,
};

void stimulus_program(uint16_t words[STIMULUS_WORDS]) {
    words[0] = APIO_SET_Y(15); /* Sixteen pressure high/low pairs. */
    words[1] = APIO_PULL_BLOCK;
    words[2] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 9);
    words[3] = APIO_PULL_BLOCK;
    words[4] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 9);
    words[5] = APIO_JMP_Y_DEC(1);
    words[6] = APIO_PULL_BLOCK;
    words[7] = APIO_ADD_DELAY(APIO_OUT_PINS(21), 9); /* released state */
    words[8] = APIO_SET_Y(7); /* Eight released delay iterations. */
    words[9] = APIO_ADD_DELAY(APIO_NOP, 31);
    words[10] = APIO_JMP_Y_DEC(9);
    words[11] = APIO_SET_Y(3); /* Four recovery high/low pairs. */
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
