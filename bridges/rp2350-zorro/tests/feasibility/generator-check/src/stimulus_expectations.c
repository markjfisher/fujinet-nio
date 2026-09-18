#include "stimulus_expectations.h"

static const uint16_t words[] = {
    0xe02f, 0xa909, 0xe900, 0xe801, 0x0041, 0xc000, 0x0006
};
static const unsigned values[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};
const stimulus_expectations stimulus_expected = {
    .words = words, .word_count = sizeof words / sizeof words[0],
    .values = values, .value_count = sizeof values / sizeof values[0],
    .data_start_cycle = 1, .data_period_cycles = 30,
    .low_offset = 10, .low_cycles = 10,
    .completion_irq_cycle = 481, .observation_cycles = 550,
    .abort_after_cycles = 15, /* Inside the first /AS low interval. */
};
