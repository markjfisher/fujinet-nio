#include "capture_program.h"
#include <epio.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

#define AS     (1u << CAPTURE_STROBE_PIN)
#define RW     (1u << CAPTURE_RW_PIN)
#define UDS    (1u << CAPTURE_UDS_PIN)
#define LDS    (1u << CAPTURE_LDS_PIN)
#define SELECT (1u << CAPTURE_SELECT_PIN)

typedef struct {
    unsigned value;
    unsigned controls;
    unsigned accepted;
} transaction;

static const transaction transactions[] = {
    {0x1357, 0, 0}, {0x2468, RW | SELECT, 0},
    {0x369c, UDS | SELECT, 0}, {0x4abc, LDS | SELECT, 0},
    {0x000a, SELECT, 1}, {0x00a5, SELECT, 1},
    {0x0001, SELECT, 1}, {0x0002, SELECT, 1},
    {0x0004, SELECT, 1}, {0x0008, SELECT, 1},
    {0x0010, SELECT, 1}, {0x0020, SELECT, 1},
    {0x0040, SELECT, 1}, {0x0080, SELECT, 1},
    {0x0100, SELECT, 1}, {0x0200, SELECT, 1},
    {0x0400, SELECT, 1}, {0x0800, SELECT, 1},
    {0x1000, SELECT, 1}, {0x2000, SELECT, 1},
    {0x4000, SELECT, 1}, {0x8000, SELECT, 1},
};

static const uint64_t input_mask = ((1ull << CAPTURE_RAW_BITS) - 1ull)
                                   << CAPTURE_DATA_BASE;

static void drive(epio_t *e, unsigned value, unsigned controls) {
    epio_drive_gpios_ext(e, input_mask,
                         ((uint64_t)value << CAPTURE_DATA_BASE) | controls);
}

static unsigned observe(epio_t *e, transaction t) {
    drive(e, t.value, t.controls | AS);
    epio_step_cycles(e, 1); /* released-/AS guard */
    drive(e, t.value, t.controls);
    epio_step_cycles(e, 2); /* WAIT /AS low, then IN PINS,21 */
    epio_step_cycles(e, 1); /* PUSH */
    CHECK(epio_rx_fifo_depth(e, CAPTURE_PIO, CAPTURE_SM) == 1);
    unsigned raw = epio_pop_rx_fifo(e, CAPTURE_PIO, CAPTURE_SM);
    epio_step_cycles(e, 1); /* IRQ */
    CHECK(epio_peek_block_irq_num(e, CAPTURE_PIO, CAPTURE_IRQ) == 1);
    epio_clear_block_irq(e, CAPTURE_PIO, CAPTURE_IRQ);
    drive(e, t.value, t.controls | AS);
    epio_step_cycles(e, 1); /* release and wrap */
    return raw;
}

int main(void) {
    static const unsigned accepted[] = {
        0x000a, 0x00a5,
        0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
        0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000,
    };
    unsigned raw_count = 0, rejected_count = 0, accepted_count = 0;

    capture_program_init();
    /* The protocol uses this same rearm immediately before an interactive run. */
    capture_program_rearm();
    epio_t *e = epio_from_apio();
    CHECK(e != NULL);

    for (size_t index = 0; index < sizeof transactions / sizeof transactions[0]; ++index) {
        const transaction t = transactions[index];
        unsigned raw = observe(e, t);
        unsigned expected_raw = t.value | (t.controls >> CAPTURE_DATA_BASE);
        CHECK(raw == expected_raw);
        CHECK(capture_program_accepts(raw) == t.accepted);
        ++raw_count;
        if (t.accepted)
            CHECK((raw & CAPTURE_VALUE_MASK) == accepted[accepted_count++]);
        else
            ++rejected_count;
    }
    CHECK(raw_count == 22);
    CHECK(rejected_count == 4);
    CHECK(accepted_count == sizeof accepted / sizeof accepted[0]);
    epio_free(e);
    puts("capture width/control: PIO recorded 22 raw states; ARM qualifier accepted 18 selected writes");
    return 0;
}
