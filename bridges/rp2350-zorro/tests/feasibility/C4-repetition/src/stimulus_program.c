#include "stimulus.h"
#include <apio.h>

/* Five four-pulse groups at 100 kHz.  Values identify groups: low width is
 * reduced with the gap held at 100 us, then the gap is reduced with low width
 * held at 100 us.  X is loaded with the next group during the released phase;
 * only MOV PINS changes data. */
void stimulus_program(uint16_t words[STIMULUS_WORDS]) {
    words[0] = APIO_SET_X(1);
    words[1] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 9);

    words[2] = APIO_SET_Y(3);  words[3] = APIO_ADD_DELAY(APIO_SET_PINS(0), 9);
    words[4] = APIO_ADD_DELAY(APIO_SET_PINS(1), 8); words[5] = APIO_JMP_Y_DEC(3);
    words[6] = APIO_SET_X(2);  words[7] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 9);

    words[8] = APIO_SET_Y(3);  words[9] = APIO_ADD_DELAY(APIO_SET_PINS(0), 4);
    words[10] = APIO_ADD_DELAY(APIO_SET_PINS(1), 8); words[11] = APIO_JMP_Y_DEC(9);
    words[12] = APIO_SET_X(3); words[13] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 9);

    words[14] = APIO_SET_Y(3); words[15] = APIO_ADD_DELAY(APIO_SET_PINS(0), 1);
    words[16] = APIO_ADD_DELAY(APIO_SET_PINS(1), 8); words[17] = APIO_JMP_Y_DEC(15);
    words[18] = APIO_SET_X(4); words[19] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 9);

    words[20] = APIO_SET_Y(3); words[21] = APIO_ADD_DELAY(APIO_SET_PINS(0), 9);
    words[22] = APIO_ADD_DELAY(APIO_SET_PINS(1), 3); words[23] = APIO_JMP_Y_DEC(21);
    words[24] = APIO_SET_X(5); words[25] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 9);

    words[26] = APIO_SET_Y(3); words[27] = APIO_ADD_DELAY(APIO_SET_PINS(0), 9);
    words[28] = APIO_SET_PINS(1); words[29] = APIO_JMP_Y_DEC(27);
    words[30] = APIO_IRQ_SET(0); words[31] = APIO_JMP(31);
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
