#include "capture_program.h"

#include "hardware/pio.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

/* Lab-only control protocol. It reports observation counters after a finite
 * stimulus run; it is not a bridge mailbox or a production interface. */
static unsigned capture_count, capture_irq_count;

static void drain_capture(void) {
    while (!pio_sm_is_rx_fifo_empty(pio0, CAPTURE_SM)) {
        (void)pio_sm_get(pio0, CAPTURE_SM);
        ++capture_count;
    }
    if (pio_interrupt_get(pio0, CAPTURE_IRQ)) {
        pio_interrupt_clear(pio0, CAPTURE_IRQ);
        ++capture_irq_count;
    }
}

static void reset_counters(void) {
    drain_capture();
    capture_count = 0;
    capture_irq_count = 0;
}

int main(void) {
    char command[16];
    size_t used = 0;
    stdio_init_all();
    capture_program_init();
    for (;;) {
        drain_capture();
        int ch = getchar_timeout_us(0);
        if (ch < 0)
            continue;
        if (ch == '\r' || ch == '\n') {
            command[used] = 0;
            if (!strcmp(command, "reset")) {
                reset_counters();
                printf("reset protocol=capture-counters-v1\n");
            } else if (!strcmp(command, "report")) {
                drain_capture();
                printf("result protocol=capture-counters-v1 capture_count=%u capture_irq_count=%u\n",
                       capture_count, capture_irq_count);
            } else if (used) {
                printf("error unknown command\n");
            }
            used = 0;
        } else if (ch >= 32 && ch <= 126 && used + 1 < sizeof(command)) {
            command[used++] = (char)ch;
        } else {
            used = sizeof(command) - 1;
        }
    }
}
