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
    const uint64_t data_mask = 0x3c;
    /* C0 contract: all sixteen data values occur while /AS is pulled high. */
    for (unsigned value = 0; value < 16; ++value) {
        epio_drive_gpios_ext(e, data_mask, (uint64_t)value << CAPTURE_DATA_BASE);
        epio_step_cycles(e, 21);
        CHECK(epio_rx_fifo_depth(e, CAPTURE_PIO, CAPTURE_SM) == 0);
        CHECK(epio_peek_block_irq_num(e, CAPTURE_PIO, CAPTURE_IRQ) == 0);
    }
    epio_free(e);
    puts("capture idle: all data values with /AS high produced no capture or IRQ");
    return 0;
}
