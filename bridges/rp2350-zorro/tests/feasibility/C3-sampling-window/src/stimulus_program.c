#include "stimulus.h"
#include <apio.h>

/* Four independently visible sampling-window cases at a 100 kHz PIO clock.
 * Data moves 10 us and 50 us before /AS, then 10 us and 50 us after /AS.
 * X supplies the old/new values for pre-assertion cases; Y preserves the new
 * value while X drives the old value for post-assertion cases. */
void stimulus_program(uint16_t words[STIMULUS_WORDS]) {
    words[0] = APIO_SET_X(0);
    words[1] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 4);
    words[2] = APIO_SET_X(1);
    words[3] = APIO_MOV_PINS_X;                 /* D=1, 10 us before /AS */
    words[4] = APIO_ADD_DELAY(APIO_SET_PINS(0), 1);
    words[5] = APIO_SET_PINS(1);

    words[6] = APIO_SET_Y(3);
    words[7] = APIO_SET_X(2);
    words[8] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 4);
    words[9] = APIO_SET_PINS(0);                /* /AS falls with old D=2 */
    words[10] = APIO_MOV_PINS_Y;                 /* D=3, 10 us after /AS */
    words[11] = APIO_SET_PINS(1);

    words[12] = APIO_SET_X(4);
    words[13] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 4);
    words[14] = APIO_SET_X(5);
    words[15] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 4); /* D=5, 50 us before */
    words[16] = APIO_SET_PINS(0);
    words[17] = APIO_SET_PINS(1);

    words[18] = APIO_SET_Y(7);
    words[19] = APIO_SET_X(6);
    words[20] = APIO_ADD_DELAY(APIO_MOV_PINS_X, 4);
    words[21] = APIO_ADD_DELAY(APIO_SET_PINS(0), 4); /* old D=6 for 50 us */
    words[22] = APIO_MOV_PINS_Y;                 /* D=7, 50 us after /AS */
    words[23] = APIO_SET_PINS(1);
    words[24] = APIO_IRQ_SET(0);
    words[25] = APIO_JMP(25);
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
