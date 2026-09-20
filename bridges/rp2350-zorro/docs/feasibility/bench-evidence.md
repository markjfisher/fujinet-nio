# Story 2.2 synthetic-bench evidence ledger

This ledger records the reviewed local physical runs for C0–C9. It is an index
to evidence, not a replacement for it: `report.json` is the authoritative
machine verdict, `capture.sr` is the raw analyzer acquisition, and the USB and
tool logs record collection. Those artifacts remain in the ignored local build
tree, so a fresh checkout does not gain the raw captures from this page alone.

Every listed run reports `experiment_status: passed`,
`evidence_scope: stimulus-and-dut`, and a 1 MHz analyzer acquisition. C0–C4 use
the W0 four-bit input fixture; C5–C9 use the W1 fixture. Run IDs below are
relative to `build/feasibility/`.

| Case | Reviewed local run | Fixture result |
| --- | --- | --- |
| C0 | `C0/20260919T093822Z-035d073e` | Data `0..15` changed while `/AS` remained high; DUT reported zero captures and zero capture IRQs. |
| C1 | `C1/20260919T094307Z-f1c3ac46` | All 28 asserted four-bit pattern values were captured once, in order. |
| C2 | `C2/20260919T111235Z-282a7232` | One held-low `/AS` transaction captured `0x3` once; later values while held active did not recapture. |
| C3 | `C3/20260919T112204Z-76ed4f0f` | Four before/after cases produced DUT values `0x1, 0x2, 0x5, 0x6` as declared. |
| C4 | `C4/20260919T133017Z-f7a84933` | Twenty repeated assertions completed in order across the declared low-width and released-gap groups. |
| C5 | `C5/20260919T190948Z-da4c1121` | Eighteen qualified 16-bit writes were captured in order; four unselected/read/single-lane assertions were rejected. The analyzer observed three data bits and five controls. |
| C6 | `C6/20260919T194547Z-6d57a368` | The deliberate 2 ms drain pause retained five initial words and four recovery sentinels; eleven of twenty assertions were explicitly accounted as unobserved while `PUSH BLOCK` stalled capture. |
| C7 | `C7/20260920T131805Z-5dbedfa4` | One qualified read produced response `0xA501` and one response IRQ; `/ACK` was observed during the read and the input observer rejected the read as a write capture. |
| C8 | `C8/20260920T131832Z-f68a0ee9` | Read `0xA501`, write `0x3C3C`, read `0x5A02` completed with two DUT responses and one qualified write capture. |
| C9 | `C9/20260920T140813Z-d888093d` | A freshly rearmed session captured the twenty declared values `0x1000..0x100F, 0xD001..0xD004`, with no stale values. |

## What these runs establish

The reports establish the bounded digital behavior of the current two-board,
3.3 V synthetic fixtures. They join independent RP2040 stimulus waveform
analysis to a fresh Core2350B PIO-to-ARM DUT report. C5–C9 also make the
eight-channel analyzer coverage explicit: it is a representative subset, while
the DUT report checks complete 16-bit values.

C6 establishes the current `PUSH BLOCK` failure mode as visible and counted. It
does not establish a sustained-transfer capacity or select a production ARM/DMA
drain design. C7 and C8 establish functional response and direction sequencing
at the declared fixture conditions. A 1 MHz digital capture does not establish
analog high-impedance quality, contention current, edge quality, or a Zorro
response-time margin. C9 establishes the runner's fresh-rearm case; systematic
power-loss and USB-disconnect fault injection remains an open physical case.

## Evidence still required

Before a Story 2.2 real-bus proceed verdict, retain reviewed raw artifacts in a
durable accessible location, record instrument uncertainty and measured timing
margins, validate the buffered/electrical interface, and collect C10 passive
real-bus evidence. The [Story 2.2 plan](../story-2-2-experiment-plan.md) remains
the acceptance authority.
