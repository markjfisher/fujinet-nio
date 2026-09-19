#include "capture_program.h"
#include <epio.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static void release(epio_t *epio, unsigned value) {
    epio_drive_gpios_ext(epio, 0x3e, ((uint64_t)value << CAPTURE_DATA_BASE) | 0x2);
    epio_step_cycles(epio, 2); /* WAIT HIGH then wrap. */
}

static unsigned pop_value(epio_t *epio) {
    epio_step_cycles(epio, 1); /* PUSH */
    CHECK(epio_rx_fifo_depth(epio, CAPTURE_PIO, CAPTURE_SM) == 1);
    unsigned value = epio_pop_rx_fifo(epio, CAPTURE_PIO, CAPTURE_SM);
    epio_step_cycles(epio, 1); /* IRQ */
    CHECK(epio_peek_block_irq_num(epio, CAPTURE_PIO, CAPTURE_IRQ) == 1);
    epio_clear_block_irq(epio, CAPTURE_PIO, CAPTURE_IRQ);
    return value;
}

static void pre_assertion_change(epio_t *epio, unsigned old, unsigned newer) {
    release(epio, old);
    epio_step_cycles(epio, 8);
    epio_drive_gpios_ext(epio, 0x3e, (uint64_t)newer << CAPTURE_DATA_BASE);
    epio_step_cycles(epio, 1); /* The changed value is present before /AS. */
    epio_drive_gpios_ext(epio, 0x3e, (uint64_t)newer << CAPTURE_DATA_BASE);
    epio_step_cycles(epio, 2); /* WAIT LOW then IN samples the new value. */
    CHECK(pop_value(epio) == newer);
    release(epio, newer);
}

static void post_assertion_change(epio_t *epio, unsigned old, unsigned newer) {
    release(epio, old);
    epio_step_cycles(epio, 8);
    epio_drive_gpios_ext(epio, 0x3e, (uint64_t)old << CAPTURE_DATA_BASE);
    epio_step_cycles(epio, 2); /* WAIT LOW then IN samples old data. */
    epio_drive_gpios_ext(epio, 0x3e, (uint64_t)newer << CAPTURE_DATA_BASE);
    epio_step_cycles(epio, 1); /* Data change follows the capture. */
    CHECK(pop_value(epio) == old);
    release(epio, newer);
}

int main(void) {
    capture_program_init();
    epio_t *epio = epio_from_apio();
    CHECK(epio != NULL);
    pre_assertion_change(epio, 0, 1);  /* C3 pre-10 */
    post_assertion_change(epio, 2, 3); /* C3 post-10 */
    pre_assertion_change(epio, 4, 5);  /* C3 pre-50 */
    post_assertion_change(epio, 6, 7); /* C3 post-50 */
    epio_free(epio);
    puts("capture sampling window: pre changes capture new; post changes capture old");
    return 0;
}
