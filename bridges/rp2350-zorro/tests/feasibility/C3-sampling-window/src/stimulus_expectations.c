#include "stimulus_expectations.h"

static const uint16_t words[] = {
    0xe020, 0xa401, 0xe021, 0xa001, 0xe100, 0xe001,
    0xe043, 0xe022, 0xa401, 0xe000, 0xa002, 0xe001,
    0xe024, 0xa401, 0xe025, 0xa401, 0xe000, 0xe001,
    0xe047, 0xe026, 0xa401, 0xe400, 0xa002, 0xe001,
    0xc000, 0x0019,
};
static const unsigned values[] = {1, 2, 5, 6};
static const stimulus_phase phases[] = {
    {7, 0, 1}, {1, 1, 1}, {2, 1, 0}, {3, 1, 1},
    {5, 2, 1}, {1, 2, 0}, {1, 3, 0}, {2, 3, 1},
    {6, 4, 1}, {5, 5, 1}, {1, 5, 0}, {3, 5, 1},
    {5, 6, 1}, {5, 6, 0}, {1, 7, 0}, {32, 7, 1},
};

const stimulus_expectations stimulus_expected = {
    .words = words,
    .word_count = sizeof words / sizeof words[0],
    .values = values,
    .value_count = sizeof values / sizeof values[0],
    .data_start_cycle = 1,
    .data_period_cycles = 1,
    .completion_irq_cycle = 49,
    .observation_cycles = 80,
    .abort_after_cycles = 20,
    .phases = phases,
    .phase_count = sizeof phases / sizeof phases[0],
};
