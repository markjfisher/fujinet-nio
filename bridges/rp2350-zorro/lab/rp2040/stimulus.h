#ifndef STIMULUS_H
#define STIMULUS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define STIMULUS_WORDS 7
#define STIMULUS_HZ 100000
#define STIMULUS_DATA_BASE 2
#define STIMULUS_DATA_BITS 4
#define STIMULUS_AS_PIN 6
#define STIMULUS_PIN_MASK (0x1fu << STIMULUS_DATA_BASE)
typedef struct {
    uint32_t clkdiv, execctrl, shiftctrl, pinctrl;
} stimulus_registers;
stimulus_registers stimulus_config(uint32_t system_hz);
void stimulus_program(uint16_t words[STIMULUS_WORDS]);
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
