#include "stimulus_expectations.h"

#define AS     (1u << 16)
#define RW     (1u << 17)
#define UDS    (1u << 18)
#define LDS    (1u << 19)
#define SELECT (1u << 20)
#define IDLE   (AS | RW | UDS | LDS)
#define WRITE_HIGH(v) ((uint32_t)(v) | AS | SELECT)
#define WRITE_LOW(v)  ((uint32_t)(v) | SELECT)
#define RECOVERY_PAIR(v) WRITE_HIGH(v), WRITE_LOW(v)

static const uint16_t words[] = {
    0xe04f, 0x80a0, 0x6915, 0x80a0, 0x6915, 0x0081,
    0x80a0, 0x6915, 0xe047, 0xbf42, 0x0089,
    0xe043, 0x80a0, 0x6915, 0x80a0, 0x6915, 0x008c,
    0x80a0, 0x6915, 0xc000, 0x0014,
};
static const unsigned values[] = {
    0x1000, 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007,
    0x1008, 0x1009, 0x100a, 0x100b, 0x100c, 0x100d, 0x100e, 0x100f,
    0xd001, 0xd002, 0xd003, 0xd004,
};
static const uint32_t output_words[] = {
    RECOVERY_PAIR(0x1000), RECOVERY_PAIR(0x1001),
    RECOVERY_PAIR(0x1002), RECOVERY_PAIR(0x1003),
    RECOVERY_PAIR(0x1004), RECOVERY_PAIR(0x1005),
    RECOVERY_PAIR(0x1006), RECOVERY_PAIR(0x1007),
    RECOVERY_PAIR(0x1008), RECOVERY_PAIR(0x1009),
    RECOVERY_PAIR(0x100a), RECOVERY_PAIR(0x100b),
    RECOVERY_PAIR(0x100c), RECOVERY_PAIR(0x100d),
    RECOVERY_PAIR(0x100e), RECOVERY_PAIR(0x100f),
    IDLE,
    RECOVERY_PAIR(0xd001), RECOVERY_PAIR(0xd002),
    RECOVERY_PAIR(0xd003), RECOVERY_PAIR(0xd004),
    IDLE,
};

const stimulus_expectations stimulus_expected = {
    .words = words, .word_count = sizeof words / sizeof words[0],
    .values = values, .value_count = sizeof values / sizeof values[0],
    .completion_irq_cycle = 770, .observation_cycles = 880,
    .abort_after_cycles = 20,
    .output_words = output_words,
    .output_word_count = sizeof output_words / sizeof output_words[0],
};
