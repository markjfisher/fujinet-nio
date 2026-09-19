#ifndef BRIDGE_CAPTURE_PROGRAM_H
#define BRIDGE_CAPTURE_PROGRAM_H
#include <stdbool.h>
#include <stdint.h>
/* Synthetic fixture only; these are not production Zorro pin assignments. */
#ifdef CAPTURE_W1
#define CAPTURE_STROBE_PIN 18
#define CAPTURE_DATA_BASE 2
#define CAPTURE_DATA_BITS 16
#define CAPTURE_RW_PIN 19
#define CAPTURE_UDS_PIN 20
#define CAPTURE_LDS_PIN 21
#define CAPTURE_SELECT_PIN 22
#define CAPTURE_VALUE_MASK 0xffffu
#define CAPTURE_RAW_BITS 21
#define CAPTURE_RW_BIT (CAPTURE_RW_PIN - CAPTURE_DATA_BASE)
#define CAPTURE_UDS_BIT (CAPTURE_UDS_PIN - CAPTURE_DATA_BASE)
#define CAPTURE_LDS_BIT (CAPTURE_LDS_PIN - CAPTURE_DATA_BASE)
#define CAPTURE_SELECT_BIT (CAPTURE_SELECT_PIN - CAPTURE_DATA_BASE)
#else
#define CAPTURE_STROBE_PIN 1
#define CAPTURE_DATA_BASE 2
#define CAPTURE_DATA_BITS 4
#define CAPTURE_VALUE_MASK 0x0fu
#endif
#define CAPTURE_PIO 0
#define CAPTURE_SM 0
#define CAPTURE_IRQ 0
void capture_program_init(void);
void capture_program_rearm(void);
bool capture_program_accepts(uint32_t raw_sample);
#endif
