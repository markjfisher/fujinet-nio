#include "capture_program.h"
#include <epio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Always active, including -DNDEBUG builds. Fixture/oracles deliberately use
 * literal pins, block/SM, full words and cycle counts independent of source. */
#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr);         \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
static void state(epio_t *e, unsigned depth, unsigned irq) {
    CHECK(epio_rx_fifo_depth(e, 0, 0) == depth);
    CHECK(epio_peek_block_irq_num(e, 0, 0) == irq);
}
static void capture(epio_t *e, uint32_t word) {
    epio_step_cycles(e, 1);
    state(e, 0, 0); /* WAIT LOW */
    epio_step_cycles(e, 1);
    state(e, 0, 0); /* IN */
    /* Change the bus after IN: PUSH must contain the already sampled data. */
    epio_drive_gpios_ext(e, 0x3e, 0);
    epio_step_cycles(e, 1);
    state(e, 1, 0); /* PUSH */
    CHECK(epio_pop_rx_fifo(e, 0, 0) == word);
    epio_step_cycles(e, 1);
    state(e, 0, 1); /* IRQ */
    epio_clear_block_irq(e, 0, 0);
    state(e, 0, 0);
    epio_step_cycles(e, 32);
    state(e, 0, 0); /* held low */
}
int main(void) {
    capture_program_init();
    epio_t *e = epio_from_apio();
    CHECK(e != NULL);
    CHECK(epio_get_gpio_input(e, 1) == 1);
    CHECK(epio_get_gpio_pull_up(e, 1) == 1);
    for (int pin = 1; pin <= 5; ++pin) {
        CHECK(epio_get_gpio_input_only(e, pin) == 1);
        for (int block = 0; block < 3; ++block)
            CHECK(epio_block_can_control_gpio_output(e, block, pin) == 0);
        if (pin >= 2) {
            CHECK(epio_get_gpio_input(e, pin) == 0);
            CHECK(epio_get_gpio_pull_down(e, pin) == 1);
            CHECK(epio_get_gpio_pull_up(e, pin) == 0);
        }
    }
    epio_drive_gpios_ext(e, 0x3c, 0x28); /* four-bit 0xA, strobe pulled high */
    epio_step_cycles(e, 32);
    state(e, 0, 0);
    epio_drive_gpios_ext(e, 0x3e, 0x28);
    capture(e, 0x0000000a);
    epio_drive_gpios_ext(e, 0x3e, 0x16); /* release with next value 0x5 */
    epio_step_cycles(e, 1);
    state(e, 0, 0); /* WAIT HIGH / wrap */
    epio_step_cycles(e, 16);
    state(e, 0, 0);
    epio_drive_gpios_ext(e, 0x3e, 0x14);
    capture(e, 0x00000005);
    epio_free(e);
    puts("capture: idle, cycle timing, full words, hold-low and rearm passed");
    return 0;
}
