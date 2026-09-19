# C10 — Representative real Zorro-II bus passive trace

C10 is deliberately **passive evidence collection**. `all` builds the C10
RP2040 marker utility so each C1–C10 target remains independently buildable,
but C10 does not load or connect the RP2040 to the real bus. It loads only the
Core2350B passive APIO capture observer, arms the analyzer for five seconds,
and asks the operator to perform the bounded reviewed host accesses.

The resulting report has `experiment_status: evidence_collected`, never
`passed`. It records raw waveform and DUT observations for the requirement
ledger. A reviewer must add the actual timing source, measurement uncertainty,
buffer/OE information and pass/fail assessment separately.

## Required hardware and mapping

Do **not** reuse W1 GPIO numbers as a Zorro connector pinout. Before C10:

1. Record the breakout/A500-adapter revision and authoritative timing source.
2. Record the reviewed buffer, OE, level/power and high-impedance design.
3. Replace the provisional Core2350B mapping in `experiment.json` with the
   reviewed actual mapping and rebuild the DUT image.
4. Use the passive input path only. No RP2040 data/control lead attaches to the
   Zorro bus for C10.

The current eight-channel analyzer allocation is a review template, not an
assumed Zorro pin map:

| Analyzer | sigrok | required reviewed net |
| --- | --- | --- |
| CH1 | D0 | /AS or chosen transaction boundary |
| CH2 | D1 | selection/decode evidence |
| CH3 | D2 | R/W |
| CH4 | D3 | upper-lane strobe |
| CH5 | D4 | lower-lane strobe |
| CH6 | D5 | one low data bit |
| CH7 | D6 | one middle data bit |
| CH8 | D7 | one high data bit |

Connect analyzer GND only to the reviewed reference point; leave CLK open
unless its manual specifies a safe use. The 8-port analyzer cannot certify a
full 16-bit trace. Use the scope for output/release edges and a larger analyzer
when simultaneous full-bus evidence is needed.

## Run

After the actual mapping is reviewed and stored, load the passive DUT with its
normal topology path and run:

```sh
./run.sh all --dut-usb-path YOUR_CORE2350_USB_PATH --output /tmp/c10-trace-001
```

At the prompt, start the bounded host accesses immediately. The runner does not
drive a bus signal. Preserve the reported `.sr`, SVG, console logs and
`report.json`, then attach requirement citations and margin calculations before
calling any real-bus requirement passed.
