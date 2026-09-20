#include "stimulus_expectations.h"

static const uint16_t words[] = {
    0x80a0, 0x6095, 0x80a0, 0x6f15, 0x80a0,
    0x7e15, 0x80a0, 0x6f15, 0xc000, 0x0009,
};
static const unsigned values[] = {0xa501};
static const uint32_t output_words[] = {0x1f0000, 0x1f0000, 0x1e0000, 0x1f0000};
static const uint32_t visible_output_words[] = {0x1f0000, 0x1e0000, 0x1f0000};

const stimulus_expectations stimulus_expected = {
    .words = words, .word_count = sizeof words / sizeof words[0],
    .values = values, .value_count = sizeof values / sizeof values[0],
    .completion_irq_cycle = 69, .observation_cycles = 80, .abort_after_cycles = 20,
    .output_words = output_words, .output_word_count = sizeof output_words / sizeof output_words[0],
    .visible_output_words = visible_output_words,
    .visible_output_word_count = sizeof visible_output_words / sizeof visible_output_words[0],
};
