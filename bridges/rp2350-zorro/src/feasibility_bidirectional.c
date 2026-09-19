#include "capture_program.h"
#include "response_program.h"

#include "hardware/pio.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

#define VALUES_MAX 16
static unsigned capture_count, capture_irq_count, raw_capture_count, rejected_capture_count;
static unsigned response_count, response_irq_count;
static unsigned capture_values[VALUES_MAX];

static void drain_capture(void) {
    while (!pio_sm_is_rx_fifo_empty(pio0, CAPTURE_SM)) {
        unsigned raw = pio_sm_get(pio0, CAPTURE_SM);
        ++raw_capture_count;
        if (!capture_program_accepts(raw)) {
            ++rejected_capture_count;
            continue;
        }
        if (capture_count < VALUES_MAX)
            capture_values[capture_count] = raw & CAPTURE_VALUE_MASK;
        ++capture_count;
        ++capture_irq_count;
    }
    if (pio_interrupt_get(pio0, CAPTURE_IRQ))
        pio_interrupt_clear(pio0, CAPTURE_IRQ);
    if (pio_interrupt_get(pio1, RESPONSE_IRQ)) {
        pio_interrupt_clear(pio1, RESPONSE_IRQ);
        ++response_irq_count;
        ++response_count;
    }
}

static void reset_observers(void) {
    capture_program_rearm();
    response_program_rearm();
    capture_count = capture_irq_count = raw_capture_count = rejected_capture_count = 0;
    response_count = response_irq_count = 0;
    memset(capture_values, 0, sizeof capture_values);
}

static void print_values(const unsigned *values, unsigned count) {
    for (unsigned index = 0; index < count; ++index)
        printf("%s%u", index ? "," : "", values[index]);
}

int main(void) {
    char command[16];
    size_t used = 0;
    stdio_init_all();
    reset_observers();
    for (;;) {
        drain_capture();
        int ch = getchar_timeout_us(0);
        if (ch < 0)
            continue;
        if (ch != '\r' && ch != '\n') {
            if (used + 1 < sizeof command)
                command[used++] = (char)ch;
            continue;
        }
        command[used] = 0;
        used = 0;
        if (!strcmp(command, "reset")) {
            reset_observers();
            printf("reset protocol=bidirectional-observer-v1\n");
        } else if (!strcmp(command, "report")) {
            drain_capture();
            printf("result protocol=bidirectional-observer-v1 capture_count=%u capture_irq_count=%u raw_capture_count=%u rejected_capture_count=%u response_count=%u response_irq_count=%u values=",
                   capture_count, capture_irq_count, raw_capture_count, rejected_capture_count,
                   response_count, response_irq_count);
            print_values(capture_values, capture_count);
            printf(" response_values=");
            print_values((const unsigned *)experiment_response_values, response_count);
            printf("\n");
        }
    }
}
