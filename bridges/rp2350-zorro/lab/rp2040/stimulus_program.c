#include "stimulus.h"
#include <apio.h>
/* Encoders only: APIO hardware initialization addresses RP2350 registers. */
void stimulus_program(uint16_t words[STIMULUS_WORDS]) {
    words[0] = APIO_SET_X(15);
    words[1] = APIO_ADD_DELAY(APIO_MOV_SRC_INVERT(APIO_MOV_PINS_X), 9);
    words[2] = APIO_ADD_DELAY(APIO_SET_PINS(0), 9);
    words[3] = APIO_ADD_DELAY(APIO_SET_PINS(1), 8);
    words[4] = APIO_JMP_X_DEC(1);
    words[5] = APIO_IRQ_SET(0);
    words[6] = APIO_JMP(6);
}

stimulus_registers stimulus_config(uint32_t system_hz) {
    /* RP2040-compatible register layout. Firmware statically checks these
     * field positions against its SDK. Native tests consume these same words.
     * Floor to the PIO divider's 16.8 resolution; 125 MHz is exactly /1250. */
    return (stimulus_registers){
        .clkdiv=(uint32_t)(((uint64_t)system_hz * 256 / STIMULUS_HZ) << 8),
        .execctrl=(STIMULUS_WORDS-1u) << 12,
        .shiftctrl=0,
        .pinctrl=STIMULUS_DATA_BASE | (STIMULUS_AS_PIN << 5) |
            (STIMULUS_DATA_BITS << 20) | (1u << 26)
    };
}
