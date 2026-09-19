#include "stimulus_expectations.h"

#define AS     (1u << 16)
#define RW     (1u << 17)
#define UDS    (1u << 18)
#define LDS    (1u << 19)
#define SELECT (1u << 20)
#define IDLE   (AS | RW | UDS | LDS)
#define WRITE_HIGH(v) ((uint32_t)(v) | AS | SELECT)
#define WRITE_LOW(v)  ((uint32_t)(v) | SELECT)
#define UNSELECTED_HIGH(v) ((uint32_t)(v) | AS)
#define READ_HIGH(v) ((uint32_t)(v) | AS | RW | SELECT)
#define READ_LOW(v)  ((uint32_t)(v) | RW | SELECT)
#define LOWER_ONLY_HIGH(v) ((uint32_t)(v) | AS | UDS | SELECT)
#define LOWER_ONLY_LOW(v)  ((uint32_t)(v) | UDS | SELECT)
#define UPPER_ONLY_HIGH(v) ((uint32_t)(v) | AS | LDS | SELECT)
#define UPPER_ONLY_LOW(v)  ((uint32_t)(v) | LDS | SELECT)

static const uint16_t words[] = {
    0xe055, 0x80a0, 0x6915, 0x80a0, 0x6915,
    0x0081, 0x80a0, 0x6915, 0xc000, 0x0009,
};
static const unsigned values[] = {
    0x000a, 0x00a5,
    0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
    0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000,
};
static const uint32_t output_words[] = {
    UNSELECTED_HIGH(0x1357), UNSELECTED_HIGH(0x1357) & ~AS,
    READ_HIGH(0x2468),       READ_LOW(0x2468),
    LOWER_ONLY_HIGH(0x369c), LOWER_ONLY_LOW(0x369c),
    UPPER_ONLY_HIGH(0x4abc), UPPER_ONLY_LOW(0x4abc),
    WRITE_HIGH(0x000a), WRITE_LOW(0x000a),
    WRITE_HIGH(0x00a5), WRITE_LOW(0x00a5),
    WRITE_HIGH(0x0001), WRITE_LOW(0x0001),
    WRITE_HIGH(0x0002), WRITE_LOW(0x0002),
    WRITE_HIGH(0x0004), WRITE_LOW(0x0004),
    WRITE_HIGH(0x0008), WRITE_LOW(0x0008),
    WRITE_HIGH(0x0010), WRITE_LOW(0x0010),
    WRITE_HIGH(0x0020), WRITE_LOW(0x0020),
    WRITE_HIGH(0x0040), WRITE_LOW(0x0040),
    WRITE_HIGH(0x0080), WRITE_LOW(0x0080),
    WRITE_HIGH(0x0100), WRITE_LOW(0x0100),
    WRITE_HIGH(0x0200), WRITE_LOW(0x0200),
    WRITE_HIGH(0x0400), WRITE_LOW(0x0400),
    WRITE_HIGH(0x0800), WRITE_LOW(0x0800),
    WRITE_HIGH(0x1000), WRITE_LOW(0x1000),
    WRITE_HIGH(0x2000), WRITE_LOW(0x2000),
    WRITE_HIGH(0x4000), WRITE_LOW(0x4000),
    WRITE_HIGH(0x8000), WRITE_LOW(0x8000),
    IDLE,
};

const stimulus_expectations stimulus_expected = {
    .words = words, .word_count = sizeof words / sizeof words[0],
    .values = values, .value_count = sizeof values / sizeof values[0],
    .completion_irq_cycle = 520, .observation_cycles = 600,
    .abort_after_cycles = 20,
    .output_words = output_words,
    .output_word_count = sizeof output_words / sizeof output_words[0],
};
