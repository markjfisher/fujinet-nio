#include "stimulus.h"
#include <apio.h>

/* One /AS assertion, deliberately held while D0..D3 move from 3 to A, 5 and C.
 * The capture program must sample 3 once and wait for /AS to release before it
 * can accept another transaction. */
void stimulus_program(uint16_t words[STIMULUS_WORDS]) {
    words[0] = APIO_SET_X(3);
    words[1] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 19);
    words[2] = APIO_ADD_DELAY(APIO_SET_PINS(0), 19);
    words[3] = APIO_SET_X(10);
    words[4] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 19);
    words[5] = APIO_SET_X(5);
    words[6] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 19);
    words[7] = APIO_SET_X(12);
    words[8] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 19);
    words[9] = APIO_ADD_DELAY(APIO_SET_PINS(1), 19);
    words[10] = APIO_IRQ_SET(0);
    words[11] = APIO_JMP(11);
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
