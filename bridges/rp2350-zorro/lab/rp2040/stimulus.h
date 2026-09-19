#ifndef STIMULUS_H
#define STIMULUS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifndef STIMULUS_WORDS
#define STIMULUS_WORDS 7
#endif
#ifndef STIMULUS_SAMPLE_COUNT
#define STIMULUS_SAMPLE_COUNT 16
#endif
#ifndef STIMULUS_OUTPUT_BASE
#define STIMULUS_OUTPUT_BASE 2
#endif
#ifndef STIMULUS_OUTPUT_PINS
#define STIMULUS_OUTPUT_PINS 4
#endif
/* GPIOs driven by the state machine.  This is wider than OUT_COUNT for the
 * original W0 programs because /AS is selected with SET, rather than OUT. */
#ifndef STIMULUS_DRIVE_PINS
#define STIMULUS_DRIVE_PINS 5
#endif
#ifndef STIMULUS_SET_BASE
#define STIMULUS_SET_BASE 6
#endif
#ifndef STIMULUS_SET_PINS
#define STIMULUS_SET_PINS 1
#endif
#ifndef STIMULUS_IDLE_VALUE
#define STIMULUS_IDLE_VALUE (1u << 4)
#endif
#ifndef STIMULUS_USE_DMA
#define STIMULUS_USE_DMA 0
#endif
#ifndef STIMULUS_OUTPUT_WORD_COUNT
#define STIMULUS_OUTPUT_WORD_COUNT 0
#endif
#ifndef STIMULUS_SHIFTCTRL
#define STIMULUS_SHIFTCTRL 0
#endif
#ifndef STIMULUS_HZ
#define STIMULUS_HZ 100000
#endif
#ifndef STIMULUS_DYNAMIC_DIRECTIONS
#define STIMULUS_DYNAMIC_DIRECTIONS 0
#endif
#define STIMULUS_DATA_BASE 2
#define STIMULUS_DATA_BITS 4
#ifndef STIMULUS_AS_PIN
#define STIMULUS_AS_PIN 6
#endif
#define STIMULUS_PIN_MASK \
    ((((1u << STIMULUS_DRIVE_PINS) - 1u) << STIMULUS_OUTPUT_BASE))
#define STIMULUS_IDLE_PIN_VALUES (STIMULUS_IDLE_VALUE << STIMULUS_OUTPUT_BASE)
typedef struct {
    uint32_t clkdiv, execctrl, shiftctrl, pinctrl;
} stimulus_registers;
stimulus_registers stimulus_config(uint32_t system_hz);
void stimulus_program(uint16_t words[STIMULUS_WORDS]);
#if STIMULUS_USE_DMA
extern const uint32_t stimulus_output_words[STIMULUS_OUTPUT_WORD_COUNT];
#endif
typedef struct {
    void (*start)(void *);
    void (*release)(void *); /* Must deassert /AS before releasing outputs. */
    void *ctx;
    bool active, overflow;
    unsigned used, generated;
    uint64_t deadline;
    char line[32];
    const char *result;
} stimulus_control;
void stimulus_init(stimulus_control *s, void (*start)(void *),
                   void (*release)(void *), void *ctx);
const char *stimulus_feed(stimulus_control *s, int ch, uint64_t now);
void stimulus_poll(stimulus_control *s, uint64_t now, bool connected,
                   bool done);
typedef struct {
    bool connected, ready;
    uint32_t epoch;
} stimulus_console;
typedef struct {
    void *ctx;
    void (*flush)(void *);
    int (*read_byte)(void *);                /* Nonblocking; -1 if empty. */
    bool (*try_reply)(void *, const char *); /* Nonblocking; false if full. */
} stimulus_console_io;
/* Caller holds a stable connection/epoch snapshot through this bounded step.
 * epoch must change for every observed DTR/session transition, even if a
 * disconnect and reconnect both occur between main-loop iterations. */
void stimulus_console_step(stimulus_console *c, stimulus_control *s,
                           const stimulus_console_io *io, uint64_t now,
                           bool connected, uint32_t epoch, bool done);
#endif
