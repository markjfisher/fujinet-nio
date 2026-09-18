#include "stimulus_expectations.h"

static const uint16_t words[] = {
    0xe023, 0xb301, 0xf300, 0xe02a, 0xb301, 0xe025,
    0xb301, 0xe02c, 0xb301, 0xf301, 0xc000, 0x000b,
};
static const unsigned values[] = {3};
static const stimulus_phase phases[] = {
    {1, 0, 1}, {20, 3, 1}, {21, 3, 0}, {21, 10, 0},
    {21, 5, 0}, {20, 12, 0}, {20, 12, 1}, {36, 12, 1},
};

const stimulus_expectations stimulus_expected = {
    .words = words,
    .word_count = sizeof words / sizeof words[0],
    .values = values,
    .value_count = sizeof values / sizeof values[0],
    .data_start_cycle = 1,
    .data_period_cycles = 1,
    .completion_irq_cycle = 124,
    .observation_cycles = 160,
    .abort_after_cycles = 30,
    .phases = phases,
    .phase_count = sizeof phases / sizeof phases[0],
};
