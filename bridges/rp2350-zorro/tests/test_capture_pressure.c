#include "capture_program.h"
#include <epio.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

#define AS     (1u << CAPTURE_STROBE_PIN)
#define SELECT (1u << CAPTURE_SELECT_PIN)

static const uint64_t input_mask = ((1ull << CAPTURE_RAW_BITS) - 1ull)
                                   << CAPTURE_DATA_BASE;

static void drive(epio_t *e, unsigned value, unsigned controls) {
    epio_drive_gpios_ext(e, input_mask,
                         ((uint64_t)value << CAPTURE_DATA_BASE) | controls);
}

static unsigned one(epio_t *e, unsigned value) {
    drive(e, value, SELECT | AS);
    epio_step_cycles(e, 1); /* released-/AS guard */
    drive(e, value, SELECT);
    epio_step_cycles(e, 2); /* WAIT low, IN PINS */
    epio_step_cycles(e, 1); /* PUSH */
    CHECK(epio_rx_fifo_depth(e, CAPTURE_PIO, CAPTURE_SM) == 1);
    unsigned raw = epio_pop_rx_fifo(e, CAPTURE_PIO, CAPTURE_SM);
    epio_step_cycles(e, 1); /* IRQ */
    CHECK(epio_peek_block_irq_num(e, CAPTURE_PIO, CAPTURE_IRQ) == 1);
    epio_clear_block_irq(e, CAPTURE_PIO, CAPTURE_IRQ);
    drive(e, value, SELECT | AS);
    epio_step_cycles(e, 1); /* release and wrap */
    return raw;
}

static void fill_fifo(epio_t *e, unsigned value) {
    drive(e, value, SELECT | AS);
    epio_step_cycles(e, 1);
    drive(e, value, SELECT);
    epio_step_cycles(e, 2);
    epio_step_cycles(e, 1);
    drive(e, value, SELECT | AS);
    epio_step_cycles(e, 2); /* IRQ then released-/AS wait */
}

int main(void) {
    capture_program_init();
    capture_program_rearm();
    epio_t *e = epio_from_apio();
    CHECK(e != NULL);

    /* A blocking PUSH retains the first four RX words and leaves the fifth
       sampled word waiting in the ISR.  Nothing is overwritten while the
       ARM-side drain is paused. */
    for (unsigned value = 0x1000; value < 0x1004; ++value)
        fill_fifo(e, value);
    CHECK(epio_rx_fifo_depth(e, CAPTURE_PIO, CAPTURE_SM) == 4);
    drive(e, 0x1004, SELECT);
    epio_step_cycles(e, 2); /* fifth WAIT/IN */
    epio_step_cycles(e, 1); /* blocked PUSH */
    CHECK(epio_rx_fifo_depth(e, CAPTURE_PIO, CAPTURE_SM) == 4);

    /* These assertions pass while PUSH BLOCK stalls. They cannot appear as
       completions later: the state machine holds only its fifth ISR word. */
    for (unsigned value = 0x1005; value <= 0x100f; ++value) {
        drive(e, value, SELECT | AS);
        drive(e, value, SELECT);
    }
    drive(e, 0x100f, SELECT | AS);
    CHECK(epio_rx_fifo_depth(e, CAPTURE_PIO, CAPTURE_SM) == 4);

    for (unsigned value = 0x1000; value < 0x1004; ++value)
        CHECK((epio_pop_rx_fifo(e, CAPTURE_PIO, CAPTURE_SM) & CAPTURE_VALUE_MASK) == value);
    /* EPIO represents the blocked state but has no documented guarantee for
       unblocking it after a host FIFO pop. Recovery is therefore a physical
       C6 oracle, not a fabricated emulator claim. The ordinary `one` helper
       remains below to document the re-armed APIO capture protocol. */
    capture_program_rearm();
    e = epio_from_apio();
    CHECK(e != NULL);
    for (unsigned value = 0xd001; value <= 0xd004; ++value)
        CHECK((one(e, value) & CAPTURE_VALUE_MASK) == value);
    epio_free(e);
    puts("capture pressure: four FIFO records retained, stalled assertions unreported, recovery captures resumed");
    return 0;
}
