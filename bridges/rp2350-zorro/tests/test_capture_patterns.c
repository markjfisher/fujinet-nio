#include "capture_program.h"
#include <epio.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static void capture(epio_t *epio, unsigned value) {
    epio_drive_gpios_ext(epio, 0x3e, (uint64_t)value << CAPTURE_DATA_BASE);
    epio_step_cycles(epio, 2); /* WAIT LOW then IN samples the four data pins. */
    epio_step_cycles(epio, 1); /* PUSH */
    CHECK(epio_rx_fifo_depth(epio, CAPTURE_PIO, CAPTURE_SM) == 1);
    CHECK(epio_pop_rx_fifo(epio, CAPTURE_PIO, CAPTURE_SM) == value);
    epio_step_cycles(epio, 1); /* IRQ */
    CHECK(epio_peek_block_irq_num(epio, CAPTURE_PIO, CAPTURE_IRQ) == 1);
    epio_clear_block_irq(epio, CAPTURE_PIO, CAPTURE_IRQ);
    epio_drive_gpios_ext(epio, 0x3e, (uint64_t)value << CAPTURE_DATA_BASE | 0x2);
    epio_step_cycles(epio, 1); /* WAIT HIGH then wrap */
}

int main(void) {
    static const unsigned values[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        10, 5, 10, 5, 10, 5, 10, 5, 6, 13, 3, 12,
    };
    capture_program_init();
    epio_t *epio = epio_from_apio();
    CHECK(epio != NULL);
    epio_drive_gpios_ext(epio, 0x3e, 0x2); /* /AS released. */
    epio_step_cycles(epio, 4);
    for (size_t i = 0; i < sizeof values / sizeof values[0]; ++i)
        capture(epio, values[i]);
    epio_free(epio);
    puts("capture patterns: 0..15, A/5 and seeded sequence captured in order");
    return 0;
}
