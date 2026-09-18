#include "stimulus.h"
#include <apio.h>

/*
 * C0-idle
 *
 * Exercise the data pins while /AS remains deasserted (HIGH).
 *
 * The DUT must observe no capture and no IRQ activity.
 *
 * Data sequence:
 *     0x0, 0x1, ... 0xF
 *
 * /AS:
 *     HIGH for the entire experiment.
 */

void stimulus_program(uint16_t words[STIMULUS_WORDS])
{
    /*
     * X starts at 15.
     *
     * The data instruction below outputs ~X, so:
     *
     *     X = 15 -> ~15 low nibble = 0
     *     X = 14 -> ~14 low nibble = 1
     *     ...
     *     X = 0  -> ~0  low nibble = 15
     */
    words[0] = APIO_SET_X(15);

    /*
     * Explicitly drive /AS HIGH.
     *
     * SET PINS targets STIMULUS_AS_PIN because SET_BASE/COUNT
     * are configured for the single /AS pin in stimulus_config().
     */
    words[1] = APIO_ADD_DELAY(APIO_SET_PINS(1), 9);

    /*
     * Put the next value on D0..D3.
     *
     * MOV PINS targets the four data GPIOs.
     */
    words[2] =
        APIO_ADD_DELAY(APIO_MOV_SRC_INVERT(APIO_MOV_PINS_X), 9);

    /*
     * Spend another ten PIO cycles with /AS still HIGH.
     *
     * This is deliberately a SET HIGH again rather than doing
     * anything to /AS: C0 must never assert it.
     */
    words[3] = APIO_ADD_DELAY(APIO_SET_PINS(1), 9);

    /*
     * Decrement X and repeat at the data-output instruction.
     */
    words[4] = APIO_JMP_X_DEC(2);

    /*
     * Tell the RP2040 control firmware that this bounded burst
     * has completed.
     *
     * This is the GENERATOR'S PIO IRQ, not the DUT IRQ that C0
     * expects to remain inactive.
     */
    words[5] = APIO_IRQ_SET(0);

    /*
     * Stop here until the host/control code releases the run.
     */
    words[6] = APIO_JMP(6);
}

stimulus_registers stimulus_config(uint32_t system_hz)
{
    return (stimulus_registers){
        /*
         * Same nominal 100 kHz PIO rate as generator-check.
         */
        .clkdiv =
            (uint32_t)(((uint64_t)system_hz * 256 / STIMULUS_HZ) << 8),

        /*
         * Program wraps at the final instruction.
         */
        .execctrl = (STIMULUS_WORDS - 1u) << 12,

        .shiftctrl = 0,

        /*
         * OUT/MOV PINS:
         *     GPIO2..GPIO5, four bits
         *
         * SET PINS:
         *     GPIO6, one bit (/AS)
         */
        .pinctrl =
            STIMULUS_DATA_BASE |
            (STIMULUS_AS_PIN << 5) |
            (STIMULUS_DATA_BITS << 20) |
            (1u << 26)
    };
}