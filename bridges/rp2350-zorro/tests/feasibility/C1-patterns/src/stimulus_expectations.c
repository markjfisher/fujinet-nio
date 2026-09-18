#include "stimulus_expectations.h"

static const uint16_t words[] = {
    0xe02f, 0xa909, 0xe900, 0xe901, 0x0041, 0xe047, 0xe02a, 0xa901,
    0xe900, 0xe901, 0xa029, 0x0087, 0xe026, 0xa901, 0xe900, 0xe901,
    0xe02d, 0xa901, 0xe900, 0xe901, 0xe023, 0xa901, 0xe900, 0xe901,
    0xe02c, 0xa901, 0xe900, 0xe901, 0xc000, 0x001d,
};
static const unsigned values[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    10, 5, 10, 5, 10, 5, 10, 5, 6, 13, 3, 12,
};
static const unsigned periods[] = {
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 33,
    32, 32, 32, 32, 32, 32, 32, 33, 31, 31, 31, 31,
};
const stimulus_expectations stimulus_expected = {
    .words = words, .word_count = sizeof words / sizeof words[0],
    .values = values, .period_cycles = periods,
    .value_count = sizeof values / sizeof values[0],
    .data_start_cycle = 1, .data_period_cycles = 30,
    .low_offset = 10, .low_cycles = 10,
    .completion_irq_cycle = 879, .observation_cycles = 980,
    .abort_after_cycles = 15,
};
