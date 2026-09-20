# C9 — Reset and recovery

C9 verifies the implemented recovery boundary: every `all` run reloads both
finite firmware images, sends DUT `reset`, then emits twenty selected W1 writes.
The report must contain only this run's ordered values; no previous run can
satisfy the report. Four sentinel values follow a PIO-only released interval,
so re-arm/recovery remains visible in the SVG.

It is intentionally bounded. Disconnect and power-cycle fault injection remain
physical actions: repeat `./run.sh all` after reconnecting either USB endpoint;
the loader verifies the enrolled identity and the fresh DUT counter reset before
it can arm a waveform.

## W1 mapping

Use separate USB power for each board; connect GND and signals only, never
3V3/VBUS between boards.

| Net | RP2040 | Core2350B | C9 owner |
| --- | --- | --- | --- |
| GND | GND | GND | common reference |
| D[15:0] | GP2–17 | GP2–17 | RP2040 selected writes |
| /AS | GP18 | GP18 | RP2040 output |
| R/W | GP19 | GP19 | RP2040 low for write |
| /UDS | GP20 | GP20 | RP2040 low |
| /LDS | GP21 | GP21 | RP2040 low |
| SELECT | GP22 | GP22 | RP2040 high |

## Analyzer mapping

Connect analyzer GND to common GND; leave CLK open.

| Physical channel | sigrok | Net | GPIO |
| --- | --- | --- | --- |
| CH1 | D0 | /AS | GP18 |
| CH2 | D1 | SELECT | GP22 |
| CH3 | D2 | R/W | GP19 |
| CH4 | D3 | /UDS | GP20 |
| CH5 | D4 | /LDS | GP21 |
| CH6 | D5 | D0 | GP2 |
| CH7 | D6 | D8 | GP10 |
| CH8 | D7 | D15 | GP17 |

The analyzer observes D0, D8 and D15 plus every W1 write qualifier. The DUT
report proves each complete captured word.

## Run

```sh
./run.sh all --output /tmp/c9-run-001
```

Run it again after the specified fault/reconnect with a new output directory.
Each successful report must have exactly twenty fresh values and matching fresh
firmware provenance; do not treat stale output folders as recovery evidence.
