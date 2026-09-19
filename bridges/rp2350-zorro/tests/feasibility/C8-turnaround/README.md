# C8 — Direction change and release

C8 runs three deterministic W1 transactions: DUT read `0xA501`, generator write
`0x3C3C`, DUT read `0x5A02`. RP2040 PIO changes the data-direction mask before
each transfer. Core2350B `PIO1 SM0` first waits for both released `/AS` and
`R/W=read`, so it cannot drive during the intervening write; its PIO releases
the data outputs after each read. `PIO0 SM0` captures the one write independently.

## Required W1 wiring

Use the C7 reviewed bidirectional fixture unchanged:

| Net | RP2040 | Core2350B | owner by phase |
| --- | --- | --- | --- |
| GND | GND | GND | common |
| D[15:0] | GP2–17 | GP2–17 | DUT/read; RP2040/write; released between |
| /AS | GP18 | GP18 | RP2040 |
| R/W | GP19 | GP19 | RP2040: read=high, write=low |
| /UDS, /LDS | GP20, GP21 | GP20, GP21 | RP2040 low |
| SELECT | GP22 | GP22 | RP2040 high |
| /ACK | GP26 input | GP26 output | DUT low only for reads |

Keep the per-data-line series resistance and weak release bias. Do not use a
plain direct bidirectional breadboard connection.

## Analyzer mapping

| Analyzer | sigrok | net |
| --- | --- | --- |
| CH1 | D0 | /AS GP18 |
| CH2 | D1 | SELECT GP22 |
| CH3 | D2 | R/W GP19 |
| CH4 | D3 | /ACK GP26 |
| CH5 | D4 | D0 GP2 |
| CH6 | D5 | D8 GP10 |
| CH7 | D6 | D15 GP17 |
| CH8 | D7 | released-bias check |

Analyzer GND goes to common GND and CLK remains open. The SVG marks each read
and write boundary and limited three-bit data coverage.

## Run

```sh
./run.sh all --output /tmp/c8-run-001
```

A pass needs two DUT PIO responses, one captured write, no `/ACK` during the
write, and the declared observed data bits. This provides functional release
evidence only; use the scope for analog output-release measurements.
