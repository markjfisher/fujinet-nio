# C7 — Read response

This W1 two-board test has RP2040 PIO release `D[15:0]` then issue one selected
read. APIO configures Core2350B `PIO1 SM0` to enable the data outputs, put
`0xA501` on the bus and assert `/ACK` after `/AS` falls; it releases both before
the next transaction. `PIO0 SM0` remains the independent input observer. ARM
preloads the response and reports only after the finite waveform.

This is a 3.3 V synthetic fixture experiment, not a Zorro-II `/DTACK` claim.
Do not run it before the bidirectional fixture is reviewed.

## W1 pin mapping

Use separate USB power for each board; connect GND and signals only, never
3V3/VBUS between boards.

| Net | RP2040 | Core2350B | C7 owner |
| --- | --- | --- | --- |
| GND | GND | GND | common reference |
| D[15:0] | GP2–17 | GP2–17 | released, then DUT response |
| /AS | GP18 | GP18 | RP2040 output |
| R/W | GP19 | GP19 | RP2040 high for read |
| /UDS | GP20 | GP20 | RP2040 low |
| /LDS | GP21 | GP21 | RP2040 low |
| SELECT | GP22 | GP22 | RP2040 high |
| /ACK | GP26 input | GP26 output | DUT, active-low marker |

Every D line needs the reviewed series resistance and weak released bias. Do not
make a direct push-pull bidirectional connection without that adapter. Check
continuity with both boards unpowered.

## Analyzer mapping

Connect analyzer GND to common GND; leave CLK open.

| Physical channel | sigrok | Net | GPIO |
| --- | --- | --- | --- |
| CH1 | D0 | /AS | GP18 |
| CH2 | D1 | SELECT | GP22 |
| CH3 | D2 | R/W | GP19 |
| CH4 | D3 | /ACK | GP26 |
| CH5 | D4 | D0 | GP2 |
| CH6 | D5 | D8 | GP10 |
| CH7 | D6 | D15 | GP17 |
| CH8 | D7 | D1, including released-bias level | GP3 |

The analyzer observes D0, D1, D8 and D15. CH8 must be connected to GP3: it
shows the defined released-bias level before/after the read and verifies D1
during the response. The DUT report proves the complete preloaded word.

## Run

```sh
./run.sh all --output /tmp/c7-run-001
```

`all` builds, loads, resets the fresh DUT counters, waits for Enter, captures
and writes `report.json`, `waveform.svg`, `capture.sr` and console logs. Use
`./run.sh build` for offline compilation and `./run.sh all --dry-run` to inspect
its actions. A physical pass requires one rejected read capture, one PIO
response, `/ACK` low during `/AS`, and the observed subset of `0xA501`.
