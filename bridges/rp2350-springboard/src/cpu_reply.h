/* From mozzwald/fujinet-nio experiment/dual-cs-write-capture, bridges/rp2350-springboard/src.
 * Unchanged. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sb_cpu_request { uint32_t high, low; uint16_t word; bool valid; };
struct sb_cpu_bus {
    volatile uint32_t *pio;
    const volatile uint32_t *gpio_in;
    const volatile bool *stop;
};
/* Fixed ARM ABI used by cpu_reply.S. Pointer arguments also permit functional
 * testing of the actual routine against RAM registers in an ARM emulator. */
_Static_assert(offsetof(struct sb_cpu_request, high) == 0, "reply ABI");
_Static_assert(offsetof(struct sb_cpu_request, low) == 4, "reply ABI");
_Static_assert(offsetof(struct sb_cpu_request, word) == 8, "reply ABI");
_Static_assert(offsetof(struct sb_cpu_request, valid) == 10, "reply ABI");
_Static_assert(sizeof(void *) == 4 && sizeof(bool) == 1, "32-bit ARM ABI only");
_Static_assert(offsetof(struct sb_cpu_bus, gpio_in) == 4, "bus ABI");
_Static_assert(offsetof(struct sb_cpu_bus, stop) == 8, "bus ABI");
bool sb_rx_first_reply(struct sb_cpu_request *request, const uint16_t *words,
                       const struct sb_cpu_bus *bus);
