#ifndef STIMULUS_EXPECTATIONS_H
#define STIMULUS_EXPECTATIONS_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned cycles, value, strobe;
} stimulus_phase;

/* Independent oracle, linked only into the host test. Cycle indices are
 * zero-based observations AFTER stepping epio once. Before data_start_cycle
 * the first value is expected; after the sequence the last value is held.
 * /AS is high outside each sample's [low_offset, low_offset + low_cycles).
 * A zero low_cycles describes an entirely deasserted strobe. */
typedef struct {
    const uint16_t *words;
    size_t word_count;
    const unsigned *values;
    /* Optional per-value cadence. Entry n spans value n until n+1; the final
       entry is ignored. NULL means data_period_cycles for every value. */
    const unsigned *period_cycles;
    size_t value_count;
    unsigned data_start_cycle, data_period_cycles;
    unsigned low_offset, low_cycles;
    unsigned completion_irq_cycle, observation_cycles;
    unsigned abort_after_cycles;
    /* Optional literal cycle schedule for non-periodic behaviors such as a
       single held-active strobe with data transitions. */
    const stimulus_phase *phases;
    size_t phase_count;
    /* Wide/DMA waveforms supply full relative output-pin words. This remains
       a host-only oracle, independent from the firmware's DMA source table. */
    const uint32_t *output_words;
    size_t output_word_count;
    /* Optional pin-level transitions. Direction words may be consumed by a
       dynamic-direction program without creating a visible pin transition. */
    const uint32_t *visible_output_words;
    size_t visible_output_word_count;
} stimulus_expectations;

extern const stimulus_expectations stimulus_expected;
#endif
