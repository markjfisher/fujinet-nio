#ifndef BRIDGE_RESPONSE_PROGRAM_H
#define BRIDGE_RESPONSE_PROGRAM_H

#include <stddef.h>
#include <stdint.h>

/* Lab-only W1 read responder.  It is intentionally separate from the input
 * capture state machine: PIO1/SM0 owns D[15:0] direction and /ACK while PIO0
 * continues to observe every /AS assertion. */
#define RESPONSE_PIO 1
#define RESPONSE_SM 0
#define RESPONSE_IRQ 0
#define RESPONSE_DATA_BASE 2
#define RESPONSE_DATA_BITS 16
#define RESPONSE_AS_PIN 18
#define RESPONSE_ACK_PIN 26

extern const uint32_t experiment_response_values[];
extern const size_t experiment_response_value_count;

void response_program_init(void);
void response_program_rearm(void);

#endif
