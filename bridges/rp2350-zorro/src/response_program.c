#include "response_program.h"

#include <apio.h>

void response_program_init(void) {
    APIO_ENABLE_GPIOS();
    APIO_ENABLE_PIOS();
    APIO_GPIO_INIT();
    for (int pin = RESPONSE_DATA_BASE;
         pin < RESPONSE_DATA_BASE + RESPONSE_DATA_BITS; ++pin) {
        /* The PIO output-enable is zero until /AS is asserted. */
        APIO_GPIO_OUTPUT(pin, RESPONSE_PIO);
        APIO_GPIO_PULL_NONE(pin);
    }
    APIO_GPIO_OUTPUT(RESPONSE_ACK_PIN, RESPONSE_PIO);
    APIO_GPIO_PULL_NONE(RESPONSE_ACK_PIN);

    APIO_ASM_INIT();
    APIO_CLEAR_ALL_IRQS();
    APIO_SET_BLOCK(RESPONSE_PIO);
    APIO_GPIOBASE_0();
    APIO_SET_SM(RESPONSE_SM);
    APIO_WRAP_BOTTOM();
    APIO_ADD_INSTR(APIO_WAIT_GPIO_HIGH(RESPONSE_AS_PIN));
    /* A write may occur between reads.  Do not drive data until the fixture
       declares a read; this guard is what makes the program safe for C8. */
    APIO_ADD_INSTR(APIO_WAIT_GPIO_HIGH(19));
    APIO_ADD_INSTR(APIO_WAIT_GPIO_LOW(RESPONSE_AS_PIN));
    /* !NULL makes all 32 output-enable bits one; OUT PINS is configured for
       D[15:0], so the following pull supplies only the response value. */
    APIO_ADD_INSTR(APIO_MOV_PINDIRS_NOT_NULL);
    APIO_ADD_INSTR(APIO_PULL_BLOCK);
    APIO_ADD_INSTR(APIO_OUT_PINS(RESPONSE_DATA_BITS));
    APIO_ADD_INSTR(APIO_SET_PINS(0));       /* /ACK asserted. */
    APIO_ADD_INSTR(APIO_IRQ_SET(RESPONSE_IRQ));
    APIO_WRAP_TOP();
    APIO_ADD_INSTR(APIO_WAIT_GPIO_HIGH(RESPONSE_AS_PIN));
    APIO_ADD_INSTR(APIO_MOV_PINDIRS_NULL);  /* release D[15:0]. */
    APIO_ADD_INSTR(APIO_SET_PINS(1));       /* /ACK released. */
    APIO_SM_CLKDIV_SET(1, 0);
    APIO_SM_EXECCTRL_SET(0);
    /* The response word is stored in the low 16 bits of the TX FIFO word.
     * Shift right so OUT PINS maps bit 0 to D0 (GP2), through bit 15 to
     * D15 (GP17).  Shift-left would emit the zero-filled high half first. */
    APIO_SM_SHIFTCTRL_SET(APIO_OUT_SHIFTDIR_R);
    APIO_SM_PINCTRL_SET(APIO_OUT_BASE(RESPONSE_DATA_BASE) |
                       APIO_SET_BASE(RESPONSE_ACK_PIN) |
                       APIO_OUT_COUNT(RESPONSE_DATA_BITS) |
                       APIO_SET_COUNT(1));
    APIO_SM_JMP_TO_START();
    APIO_END_BLOCK();
    /* ACK is released before the SM is enabled. Data directions remain zero
       until the MOV PINDIRS instruction after an observed /AS fall. */
    APIO1_SM_TXF(RESPONSE_SM) = experiment_response_values[0];
    for (size_t index = 1; index < experiment_response_value_count; ++index)
        APIO1_SM_TXF(RESPONSE_SM) = experiment_response_values[index];
    APIO1_SM_REG(RESPONSE_SM)->instr = APIO_SET_PIN_DIRS(1);
    APIO1_SM_REG(RESPONSE_SM)->instr = APIO_SET_PINS(1);
    APIO_ENABLE_SMS(RESPONSE_PIO, (1 << RESPONSE_SM));
}

void response_program_rearm(void) { response_program_init(); }
