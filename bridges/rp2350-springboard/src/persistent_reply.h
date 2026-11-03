/* From mozzwald/fujinet-nio experiment/dual-cs-write-capture, bridges/rp2350-springboard/src.
 * Unchanged. */
#pragma once
#include "cpu_reply.h"

/* Core1-private during service; posted is temporary invalid-count scratch.
 * All fields have their final meaning only on return; C publishes after exit. */
struct sb_persistent_totals {
    uint32_t requests, posted, completed, attempted, cancelled, fault;
};
_Static_assert(sizeof(struct sb_persistent_totals) == 24, "persistent totals ABI");
_Static_assert(offsetof(struct sb_persistent_totals, completed) == 8, "totals ABI");
_Static_assert(offsetof(struct sb_persistent_totals, fault) == 20, "totals ABI");
void sb_persistent_reply(struct sb_persistent_totals *totals,
                         const uint16_t *words, const struct sb_cpu_bus *bus);
