#include "console.h"
#include "tusb.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CON_ITF 1
#define RING 16384u

static char ring[RING];
static uint32_t head, tail; /* free-running; tail chases head */
static char line[80];
static unsigned line_used;
static console_command_fn command;

void console_init(console_command_fn handler) { command = handler; }

static void put(const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        ring[(head + i) % RING] = s[i];
    head += n;
    if (head - tail > RING) {
        /* Drop whole lines from the front so the reader resyncs cleanly. */
        tail = head - RING;
        while (tail != head && ring[tail % RING] != '\n')
            ++tail;
        if (tail != head)
            ++tail;
    }
}

void console_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((unsigned)n >= sizeof buf)
        n = sizeof buf - 1;
    put(buf, (uint32_t)n);
}

void console_hex(const char *label, const uint8_t *data, uint32_t len) {
    char buf[16 * 3 + 64];
    for (uint32_t off = 0; off < len; off += 16) {
        int n =
            snprintf(buf, sizeof buf, "%s %04lx:", label, (unsigned long)off);
        for (uint32_t i = off; i < len && i < off + 16; ++i)
            n += snprintf(buf + n, sizeof buf - n, " %02x", data[i]);
        buf[n++] = '\n';
        put(buf, (uint32_t)n);
    }
}

void console_poll(void) {
    if (!tud_cdc_n_connected(CON_ITF))
        return;
    while (tail != head) {
        uint32_t room = tud_cdc_n_write_available(CON_ITF);
        if (!room)
            break;
        uint32_t at = tail % RING, n = head - tail;
        if (n > RING - at)
            n = RING - at;
        if (n > room)
            n = room;
        tail += tud_cdc_n_write(CON_ITF, ring + at, n);
    }
    tud_cdc_n_write_flush(CON_ITF);
    while (tud_cdc_n_available(CON_ITF)) {
        int c = tud_cdc_n_read_char(CON_ITF);
        if (c == '\r' || c == '\n') {
            line[line_used] = 0;
            if (line_used && command)
                command(line);
            line_used = 0;
        } else if (c >= 32 && c < 127 && line_used < sizeof line - 1) {
            line[line_used++] = (char)c;
        }
    }
}
