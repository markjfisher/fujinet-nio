#ifndef BRIDGE_CAPTURE_PROGRAM_H
#define BRIDGE_CAPTURE_PROGRAM_H
/* Synthetic fixture only; these are not production Zorro pin assignments. */
#define CAPTURE_STROBE_PIN 1
#define CAPTURE_DATA_BASE 2
#define CAPTURE_DATA_BITS 4
#define CAPTURE_PIO 0
#define CAPTURE_SM 0
#define CAPTURE_IRQ 0
void capture_program_init(void);
#endif
