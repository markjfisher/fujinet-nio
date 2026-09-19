#include "capture_program.h"
#include <apio.h>

bool capture_program_accepts(uint32_t raw_sample) {
#ifdef CAPTURE_W1
    const uint32_t required_high = 1u << CAPTURE_SELECT_BIT;
    const uint32_t required_low = (1u << CAPTURE_RW_BIT) |
                                  (1u << CAPTURE_UDS_BIT) |
                                  (1u << CAPTURE_LDS_BIT);
    return (raw_sample & required_high) == required_high &&
           (raw_sample & required_low) == 0;
#else
    (void)raw_sample;
    return true;
#endif
}

void capture_program_init(void) {
    APIO_ENABLE_GPIOS();
    APIO_ENABLE_PIOS();
    APIO_GPIO_INIT();
    APIO_GPIO_INPUT_ONLY(CAPTURE_STROBE_PIN);
    APIO_GPIO_PULL_UP(CAPTURE_STROBE_PIN);
    for (int pin = CAPTURE_DATA_BASE;
         pin < CAPTURE_DATA_BASE + CAPTURE_DATA_BITS; ++pin) {
        APIO_GPIO_INPUT_ONLY(pin);
        APIO_GPIO_PULL_DOWN(pin);
    }
#ifdef CAPTURE_W1
    for (int pin = CAPTURE_RW_PIN; pin <= CAPTURE_SELECT_PIN; ++pin) {
        APIO_GPIO_INPUT_ONLY(pin);
        APIO_GPIO_PULL_UP(pin);
    }
#endif
    APIO_ASM_INIT();
    APIO_CLEAR_ALL_IRQS();
    APIO_SET_BLOCK(CAPTURE_PIO);
    APIO_GPIOBASE_0();
    APIO_SET_SM(CAPTURE_SM);
    APIO_WRAP_BOTTOM();
#ifdef CAPTURE_W1
    /* Begin only from a released transaction. This is also the arming guard:
       a reset during an asserted or electrically transient bus state cannot
       turn that state into the first observed transaction. */
    APIO_ADD_INSTR(APIO_WAIT_GPIO_HIGH(CAPTURE_STROBE_PIN));
#endif
    APIO_ADD_INSTR(APIO_WAIT_GPIO_LOW(CAPTURE_STROBE_PIN));
    /* W1 snapshots the complete contiguous bus state at /AS. The ARM observer
       applies the selected-write qualifier to this bounded PIO FIFO record. */
#ifdef CAPTURE_W1
    APIO_ADD_INSTR(APIO_IN_PINS(CAPTURE_RAW_BITS));
#else
    APIO_ADD_INSTR(APIO_IN_PINS(CAPTURE_DATA_BITS));
#endif
    APIO_ADD_INSTR(APIO_PUSH_BLOCK);
    APIO_ADD_INSTR(APIO_IRQ_SET(CAPTURE_IRQ));
    /* apio wrap markers precede their endpoint instruction. */
    APIO_WRAP_TOP();
    APIO_ADD_INSTR(APIO_WAIT_GPIO_HIGH(CAPTURE_STROBE_PIN));
    APIO_SM_CLKDIV_SET(1, 0);
    APIO_SM_EXECCTRL_SET(0);
    APIO_SM_SHIFTCTRL_SET(APIO_IN_SHIFTDIR_L);
    APIO_SM_PINCTRL_SET(APIO_IN_BASE(CAPTURE_DATA_BASE));
    APIO_SM_JMP_TO_START();
    APIO_END_BLOCK();
    APIO_ENABLE_SMS(CAPTURE_PIO, (1 << CAPTURE_SM));
}

void capture_program_rearm(void) {
    /* Rebuild the APIO-owned SM rather than only clearing ARM-side counters.
       The lab protocol calls this while the generator is idle, before each
       burst, so the next capture must start at the released-/AS guard. */
    capture_program_init();
}
