#include "capture_program.h"

#include "hardware/pio.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

/* Lab-only control protocol. It reports observation counters after a finite
 * stimulus run; it is not a bridge mailbox or a production interface. */
#define CAPTURE_VALUES_MAX 64
#ifndef CAPTURE_DRAIN_PAUSE_US
#define CAPTURE_DRAIN_PAUSE_US 0
#endif
#ifndef CAPTURE_PRESSURE_ASSERTIONS
#define CAPTURE_PRESSURE_ASSERTIONS 0
#endif
static unsigned capture_count, capture_irq_count;
static unsigned raw_capture_count, rejected_capture_count;
static unsigned capture_values[CAPTURE_VALUES_MAX];
#if CAPTURE_DRAIN_PAUSE_US
static bool drain_pause_armed, drain_pause_active;
static uint64_t drain_resume_at;
static unsigned drain_pause_count;
#endif

static void drain_capture(void) {
#if CAPTURE_DRAIN_PAUSE_US
    /* This is a bounded lab fault injection.  It starts only when the first
       PIO RX record exists, so the interactive reset/prompt interval cannot
       consume the pause.  PIO PUSH BLOCK then makes the resulting stall
       explicit instead of overwriting FIFO data. */
    if (drain_pause_armed && !pio_sm_is_rx_fifo_empty(pio0, CAPTURE_SM)) {
        drain_pause_armed = false;
        drain_pause_active = true;
        drain_resume_at = time_us_64() + CAPTURE_DRAIN_PAUSE_US;
        ++drain_pause_count;
    }
    if (drain_pause_active) {
        if (time_us_64() < drain_resume_at)
            return;
        drain_pause_active = false;
    }
#endif
    while (!pio_sm_is_rx_fifo_empty(pio0, CAPTURE_SM)) {
        unsigned raw = pio_sm_get(pio0, CAPTURE_SM);
        ++raw_capture_count;
        if (!capture_program_accepts(raw)) {
            ++rejected_capture_count;
            continue;
        }
        unsigned value = raw & CAPTURE_VALUE_MASK;
        if (capture_count < CAPTURE_VALUES_MAX)
            capture_values[capture_count] = value;
        ++capture_count;
        ++capture_irq_count;
    }
    if (pio_interrupt_get(pio0, CAPTURE_IRQ)) {
        pio_interrupt_clear(pio0, CAPTURE_IRQ);
    }
}

static void reset_counters(void) {
    drain_capture();
    capture_program_rearm();
    capture_count = 0;
    capture_irq_count = 0;
    raw_capture_count = 0;
    rejected_capture_count = 0;
#if CAPTURE_DRAIN_PAUSE_US
    drain_pause_armed = true;
    drain_pause_active = false;
    drain_pause_count = 0;
#endif
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
                printf("reset protocol=capture-observer-v1\n");
            } else if (!strcmp(command, "report")) {
                drain_capture();
                printf("result protocol=capture-observer-v1 capture_count=%u capture_irq_count=%u raw_capture_count=%u rejected_capture_count=%u",
                       capture_count, capture_irq_count, raw_capture_count,
                       rejected_capture_count);
#if CAPTURE_DRAIN_PAUSE_US
                /* The loss count is an explicit reconciliation with the
                   finite, manifest-declared stimulus count.  It is not a
                   hardware overflow flag: PUSH BLOCK retained the records
                   that reached the FIFO and stalled the state machine. */
                printf(" pressure_pause_count=%u pressure_pause_us=%u pressure_expected_assertions=%u unobserved_assertion_count=%u",
                       drain_pause_count, CAPTURE_DRAIN_PAUSE_US,
                       CAPTURE_PRESSURE_ASSERTIONS,
                       raw_capture_count <= CAPTURE_PRESSURE_ASSERTIONS
                           ? CAPTURE_PRESSURE_ASSERTIONS - raw_capture_count : 0);
#endif
                printf(" values=");
                for (unsigned i = 0; i < capture_count && i < CAPTURE_VALUES_MAX; ++i)
                    printf("%s%u", i ? "," : "", capture_values[i]);
                printf("\n");
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
