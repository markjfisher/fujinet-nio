#include "stimulus.h"
#include <apio.h>

/* C1 emits three deterministic groups in one bounded run:
 * 0..15, eight A/5 alternations, and a fixed seeded tail 6,D,3,C.
 * Each value is held before /AS is asserted, then followed by an explicit
 * release. This is fixture firmware, not a bus protocol. */
void stimulus_program(uint16_t words[STIMULUS_WORDS]) {
    words[0] = APIO_SET_X(15);
    words[1] = APIO_ADD_DELAY(APIO_MOV_SRC_INVERT(APIO_MOV_PINS_X), 9);
    words[2] = APIO_ADD_DELAY(APIO_SET_PINS(0), 9);
    words[3] = APIO_ADD_DELAY(APIO_SET_PINS(1), 9);
    words[4] = APIO_JMP_X_DEC(1);

    words[5] = APIO_SET_Y(7);
    words[6] = APIO_SET_X(10);
    words[7] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 9);
    words[8] = APIO_ADD_DELAY(APIO_SET_PINS(0), 9);
    words[9] = APIO_ADD_DELAY(APIO_SET_PINS(1), 9);
    words[10] = APIO_MOV_SRC_INVERT(APIO_MOV_X_X);
    words[11] = APIO_JMP_Y_DEC(7);

    words[12] = APIO_SET_X(6);
    words[13] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 9);
    words[14] = APIO_ADD_DELAY(APIO_SET_PINS(0), 9);
    words[15] = APIO_ADD_DELAY(APIO_SET_PINS(1), 9);
    words[16] = APIO_SET_X(13);
    words[17] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 9);
    words[18] = APIO_ADD_DELAY(APIO_SET_PINS(0), 9);
    words[19] = APIO_ADD_DELAY(APIO_SET_PINS(1), 9);
    words[20] = APIO_SET_X(3);
    words[21] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 9);
    words[22] = APIO_ADD_DELAY(APIO_SET_PINS(0), 9);
    words[23] = APIO_ADD_DELAY(APIO_SET_PINS(1), 9);
    words[24] = APIO_SET_X(12);
    words[25] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 9);
    words[26] = APIO_ADD_DELAY(APIO_SET_PINS(0), 9);
    words[27] = APIO_ADD_DELAY(APIO_SET_PINS(1), 9);
    words[28] = APIO_IRQ_SET(0);
    words[29] = APIO_JMP(29);
}

stimulus_registers stimulus_config(uint32_t system_hz) {
    return (stimulus_registers){
        .clkdiv = (uint32_t)(((uint64_t)system_hz * 256 / STIMULUS_HZ) << 8),
        .execctrl = (STIMULUS_WORDS - 1u) << 12,
        .shiftctrl = 0,
        .pinctrl = STIMULUS_DATA_BASE | (STIMULUS_AS_PIN << 5) |
                   (STIMULUS_DATA_BITS << 20) | (1u << 26),
    };
}
