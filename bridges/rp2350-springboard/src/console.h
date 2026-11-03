#pragma once
#include <stdint.h>

/* Text console on the second USB serial port. Output is buffered and the
 * oldest lines are dropped when nobody is reading. */
typedef void (*console_command_fn)(const char *line);
void console_init(console_command_fn handler);
void console_poll(void);
void console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void console_hex(const char *label, const uint8_t *data, uint32_t len);
