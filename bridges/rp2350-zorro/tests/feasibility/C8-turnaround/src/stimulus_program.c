#include "stimulus.h"
#include <apio.h>
#define AS (1u << 16)
#define RW (1u << 17)
#define UDS (1u << 18)
#define LDS (1u << 19)
#define SELECT (1u << 20)
#define CONTROLS (AS | RW | UDS | LDS | SELECT)
#define ALL_PINS ((1u << 21) - 1u)
#define READ_LOW (RW | UDS | LDS | SELECT)
#define WRITE_HIGH(v) ((uint32_t)(v) | AS | SELECT)
#define WRITE_LOW(v) ((uint32_t)(v) | SELECT)
const uint32_t stimulus_output_words[STIMULUS_OUTPUT_WORD_COUNT] = {
 CONTROLS, CONTROLS, READ_LOW, CONTROLS,
 ALL_PINS, WRITE_HIGH(0x3c3c), WRITE_LOW(0x3c3c), CONTROLS,
 CONTROLS, CONTROLS, READ_LOW, CONTROLS,
};
void stimulus_program(uint16_t w[STIMULUS_WORDS]) {
 unsigned i=0;
 for (unsigned transaction=0; transaction<3; ++transaction) {
   w[i++]=APIO_PULL_BLOCK; w[i++]=APIO_OUT_PINDIRS(21);
   w[i++]=APIO_PULL_BLOCK; w[i++]=APIO_ADD_DELAY(APIO_OUT_PINS(21),15);
   /* The following PULL is part of the asserted interval: delay 30 makes
    * /AS low for the manifest's 320 us, rather than 330 us. */
   w[i++]=APIO_PULL_BLOCK; w[i++]=APIO_ADD_DELAY(APIO_OUT_PINS(21),30);
   w[i++]=APIO_PULL_BLOCK; w[i++]=APIO_ADD_DELAY(APIO_OUT_PINS(21),15);
 }
 w[i++]=APIO_IRQ_SET(0); w[i]=APIO_JMP(i);
}
stimulus_registers stimulus_config(uint32_t hz) { return (stimulus_registers){
 .clkdiv=(uint32_t)(((uint64_t)hz*256/STIMULUS_HZ)<<8), .execctrl=(STIMULUS_WORDS-1u)<<12,
 .shiftctrl=STIMULUS_SHIFTCTRL, .pinctrl=APIO_OUT_BASE(STIMULUS_OUTPUT_BASE)|APIO_SET_BASE(STIMULUS_SET_BASE)|APIO_OUT_COUNT(STIMULUS_OUTPUT_PINS)|APIO_SET_COUNT(STIMULUS_SET_PINS)}; }
