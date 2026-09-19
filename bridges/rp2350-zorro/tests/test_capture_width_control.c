#include "capture_program.h"
#include <epio.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static const uint64_t data_mask = ((1ull << CAPTURE_DATA_BITS) - 1ull) << CAPTURE_DATA_BASE;
static const uint64_t control_mask = (1ull << CAPTURE_STROBE_PIN) |
    (1ull << CAPTURE_RW_PIN) | (1ull << CAPTURE_UDS_PIN) |
    (1ull << CAPTURE_LDS_PIN) | (1ull << CAPTURE_SELECT_PIN);

static void drive(epio_t *e, unsigned value, unsigned controls) {
    epio_drive_gpios_ext(e, data_mask | control_mask,
                         ((uint64_t)value << CAPTURE_DATA_BASE) | controls);
}

static void selected_write(epio_t *e, unsigned value) {
    /* SELECT=1, R/W=0 and both lane strobes=0 are stable before /AS. */
    drive(e, value, (1u << CAPTURE_STROBE_PIN) | (1u << CAPTURE_SELECT_PIN));
    epio_step_cycles(e, 5); /* released-/AS guard, then four qualifiers */
    drive(e, value, 1u << CAPTURE_SELECT_PIN);
    epio_step_cycles(e, 2); /* WAIT /AS low, then IN PINS. */
    epio_step_cycles(e, 1); /* PUSH */
    CHECK(epio_rx_fifo_depth(e, CAPTURE_PIO, CAPTURE_SM) == 1);
    CHECK(epio_pop_rx_fifo(e, CAPTURE_PIO, CAPTURE_SM) == value);
    epio_step_cycles(e, 1); /* IRQ */
    CHECK(epio_peek_block_irq_num(e, CAPTURE_PIO, CAPTURE_IRQ) == 1);
    epio_clear_block_irq(e, CAPTURE_PIO, CAPTURE_IRQ);
    drive(e, value, (1u << CAPTURE_STROBE_PIN) | (1u << CAPTURE_SELECT_PIN));
    epio_step_cycles(e, 1); /* WAIT /AS high and wrap */
}

static void no_capture(epio_t *e) {
    CHECK(epio_rx_fifo_depth(e, CAPTURE_PIO, CAPTURE_SM) == 0);
    CHECK(epio_peek_block_irq_num(e, CAPTURE_PIO, CAPTURE_IRQ) == 0);
}

int main(void) {
    static const unsigned expected[] = {
        0x000a, 0x00a5,
        0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
        0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000,
    };
    capture_program_init();
    epio_t *e = epio_from_apio();
    CHECK(e != NULL);

    /* The DUT protocol rearms before each generator burst. Verify that a
       rearm begins at the released-/AS guard and cannot accept the first
       unselected assertion. */
    capture_program_rearm();
    epio_free(e);
    e = epio_from_apio();
    CHECK(e != NULL);

    /* Match the C5 invalid-control prefix. Each leaves the PIO waiting for
       the next control in its selected-write admission sequence. */
    drive(e, 0x1357, 1u << CAPTURE_STROBE_PIN);                 /* unselected */
    epio_step_cycles(e, 5); drive(e, 0x1357, 0); epio_step_cycles(e, 5); no_capture(e);
    drive(e, 0x2468, (1u << CAPTURE_STROBE_PIN) | (1u << CAPTURE_RW_PIN) |
                      (1u << CAPTURE_SELECT_PIN));              /* read */
    epio_step_cycles(e, 3); no_capture(e);
    drive(e, 0x369c, (1u << CAPTURE_STROBE_PIN) | (1u << CAPTURE_UDS_PIN) |
                      (1u << CAPTURE_SELECT_PIN));              /* lower only */
    epio_step_cycles(e, 2); no_capture(e);
    drive(e, 0x4abc, (1u << CAPTURE_STROBE_PIN) | (1u << CAPTURE_LDS_PIN) |
                      (1u << CAPTURE_SELECT_PIN));              /* upper only */
    epio_step_cycles(e, 2); no_capture(e);

    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; ++i)
        selected_write(e, expected[i]);
    no_capture(e);
    epio_free(e);
    puts("capture width/control: selected 4/8/16-bit writes captured; invalid controls ignored");
    return 0;
}
