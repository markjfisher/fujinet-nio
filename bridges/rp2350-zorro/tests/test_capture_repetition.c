#include "capture_program.h"
#include <epio.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static void one(epio_t *e, unsigned value) {
    epio_drive_gpios_ext(e, 0x3e, (uint64_t)value << CAPTURE_DATA_BASE);
    epio_step_cycles(e, 2); epio_step_cycles(e, 1);
    CHECK(epio_rx_fifo_depth(e, 0, 0) == 1); CHECK(epio_pop_rx_fifo(e, 0, 0) == value);
    epio_step_cycles(e, 1); CHECK(epio_peek_block_irq_num(e, 0, 0) == 1);
    epio_clear_block_irq(e, 0, 0);
    epio_drive_gpios_ext(e, 0x3e, ((uint64_t)value << CAPTURE_DATA_BASE) | 0x2);
    epio_step_cycles(e, 1);
}
int main(void) {
    capture_program_init(); epio_t *e = epio_from_apio(); CHECK(e);
    for (unsigned value = 1; value <= 5; ++value) for (unsigned n = 0; n < 4; ++n) one(e, value);
    epio_free(e); puts("capture repetition: five four-pulse groups captured in order"); return 0;
}
