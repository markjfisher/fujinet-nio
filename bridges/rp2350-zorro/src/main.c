#include "capture_program.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"

int main(void) {
    /* No stdio initialization: UART would overlap the input-only fixture. */
    capture_program_init();
    for (;;) {
        if (!pio_sm_is_rx_fifo_empty(pio0, CAPTURE_SM)) {
            (void)pio_sm_get(pio0, CAPTURE_SM);
        }
        pio_interrupt_clear(pio0, CAPTURE_IRQ);
        tight_loop_contents();
    }
}
