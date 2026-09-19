#include "stimulus_expectations.h"

static const uint16_t words[] = {
    0xe021, 0xa901, 0xe043, 0xe900, 0xe801, 0x0083, 0xe022, 0xa901,
    0xe043, 0xe400, 0xe801, 0x0089, 0xe023, 0xa901, 0xe043, 0xe100,
    0xe801, 0x008f, 0xe024, 0xa901, 0xe043, 0xe900, 0xe301, 0x0095,
    0xe025, 0xa901, 0xe043, 0xe900, 0xe001, 0x009b, 0xc000, 0x001f,
};
static const unsigned values[] = {1,1,1,1, 2,2,2,2, 3,3,3,3, 4,4,4,4, 5,5,5,5};
static const stimulus_phase phases[] = {
    {1,0,1},{11,1,1},
    {10,1,0},{10,1,1},{10,1,0},{10,1,1},{10,1,0},{10,1,1},{10,1,0},{11,1,1},
    {11,2,1},
    {5,2,0},{10,2,1},{5,2,0},{10,2,1},{5,2,0},{10,2,1},{5,2,0},{11,2,1},
    {11,3,1},
    {2,3,0},{10,3,1},{2,3,0},{10,3,1},{2,3,0},{10,3,1},{2,3,0},{11,3,1},
    {11,4,1},
    {10,4,0},{5,4,1},{10,4,0},{5,4,1},{10,4,0},{5,4,1},{10,4,0},{6,4,1},
    {11,5,1},
    {10,5,0},{2,5,1},{10,5,0},{2,5,1},{10,5,0},{2,5,1},{10,5,0},{43,5,1},
};
const stimulus_expectations stimulus_expected = {
    .words = words, .word_count = sizeof words / sizeof words[0],
    .values = values, .value_count = sizeof values / sizeof values[0],
    .data_start_cycle = 1, .data_period_cycles = 1,
    .completion_irq_cycle = 356, .observation_cycles = 397, .abort_after_cycles = 20,
    .phases = phases, .phase_count = sizeof phases / sizeof phases[0],
};
