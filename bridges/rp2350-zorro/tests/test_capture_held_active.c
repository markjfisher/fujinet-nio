#include "capture_program.h"
#include <epio.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

int main(void) {
    capture_program_init();
    epio_t *e = epio_from_apio();
    CHECK(e != NULL);
    /* C2 starts with D=3, then changes A,5,C while the one /AS remains low. */
    epio_drive_gpios_ext(e, 0x3e, 0x0e); /* /AS high, D=3 */
    epio_step_cycles(e, 8);
    epio_drive_gpios_ext(e, 0x3e, 0x0c); /* /AS low, D=3 */
    epio_step_cycles(e, 1);              /* WAIT LOW */
    epio_step_cycles(e, 1);              /* IN PINS samples 3 */
    epio_drive_gpios_ext(e, 0x3e, 0x28); /* D=A while held low */
    epio_step_cycles(e, 1);              /* PUSH */
    CHECK(epio_rx_fifo_depth(e, 0, 0) == 1);
    CHECK(epio_pop_rx_fifo(e, 0, 0) == 3);
    epio_step_cycles(e, 1);              /* IRQ */
    CHECK(epio_peek_block_irq_num(e, 0, 0) == 1);
    epio_clear_block_irq(e, 0, 0);
    epio_drive_gpios_ext(e, 0x3e, 0x14); /* D=5 while held low */
    epio_step_cycles(e, 16);
    epio_drive_gpios_ext(e, 0x3e, 0x30); /* D=C while held low */
    epio_step_cycles(e, 16);
    CHECK(epio_rx_fifo_depth(e, 0, 0) == 0);
    CHECK(epio_peek_block_irq_num(e, 0, 0) == 0);
    epio_drive_gpios_ext(e, 0x3e, 0x32); /* /AS high, re-arm */
    epio_step_cycles(e, 2);
    CHECK(epio_rx_fifo_depth(e, 0, 0) == 0);
    epio_free(e);
    puts("capture held-active: exactly one D=3 sample while A,5,C changed passed");
    return 0;
}
