#pragma once
#include <stdbool.h>
#include <stdint.h>

/* CS0 serves the ROM table, CS1 the register table that follows it. Both are
 * indexed by A1-A15, so each holds 32768 big-endian words. */
#define BUS_TABLE_WORDS 32768u
extern uint16_t *const bus_rom_words;
extern uint16_t *const bus_cs1_words;

/* Every CS1 write is logged as one GPIO16-47 snapshot: D0-D15 in bits 4-19,
 * A16-A18 in bits 20-22 and A19-A23 in bits 24-28 (GP39, bit 23, is the LED).
 */
#define BUS_WRITE_RING_WORDS 8192u
#define BUS_WRITE_DATA(w) ((uint16_t)((w) >> 4))
#define BUS_WRITE_REG(w) ((uint8_t)(((w) >> 20 & 7u) | ((w) >> 21 & 0xf8u)))
extern volatile uint32_t bus_write_ring[BUS_WRITE_RING_WORDS];

struct bus_totals {
    uint32_t runs, requests, posted, completed, attempted, cancelled, fault;
};

void bus_pins_init(void);
void bus_load_rom(const uint8_t *rom, uint32_t size);
void bus_write_capture_init(void);
uint32_t bus_write_head(void);
/* PIO cycles between seeing a CS1 write and sampling it, 0-31. */
void bus_set_write_settle(unsigned cycles);
unsigned bus_write_settle(void);
/* One measured WE low time of a CS1 write, if any are queued. */
bool bus_pop_we_width(uint32_t *ns);
void bus_select_counts(uint32_t *cs0, uint32_t *cs1);
void bus_launch_core1(void);
void bus_stop(void);
void bus_rearm(void);
bool bus_is_active(void);
void bus_get_totals(struct bus_totals *out);
