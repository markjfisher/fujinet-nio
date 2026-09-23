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

## What generates the bus traffic

C10 has **no Amiga traffic generator**. The RP2350 and analyzer only observe
the bus; `run.sh all` does not and must not cause an Amiga bus cycle. The
operator must supply the workload after acquisition is armed.

There are two useful capture modes:

- **Boot/AutoConfig observation:** booting an Amiga with a correctly designed
  prototype board can show the system's AutoConfig enumeration accesses. This
  is useful passive evidence that the adapter is electrically quiet and that
  its configuration response appears where expected. It is not a repeatable
  packet/mailbox test and must not be the only C10 evidence.
- **Controlled access observation:** a small Amiga diagnostic program performs
  a fixed, documented sequence of volatile reads and writes through the
  prototype board's assigned Zorro aperture. This is the intended source for
  bounded C10 traces. It must use the board's reviewed AutoConfig identity and
  mapped base address; it should record every access and pattern it issues.

A production `fujinet-nio.device` driver is neither required nor appropriate
for C10: Story 3.1 depends on the ABI this evidence will help approve. The
controlled diagnostic is a separate, minimal test artifact and cannot be
implemented until the prototype board's AutoConfig and register/aperture design
is available. If the arriving prototype does not yet decode a safe aperture,
C10 is limited to boot/AutoConfig passive captures.

## Required hardware and mapping

Do **not** reuse W1 GPIO numbers as a Zorro connector pinout. Before C10:

1. Record the breakout/A500-adapter revision and authoritative timing source.
2. Record the reviewed buffer, OE, level/power and high-impedance design.
3. Replace the provisional Core2350B mapping in `experiment.json` with the
   reviewed actual mapping and rebuild the DUT image.
4. Use the passive input path only. No RP2040 data/control lead attaches to the
   Zorro bus for C10.

## Analyzer mapping

The table is the current **provisional** C10 manifest allocation, not a Zorro
connector pinout. Replace its Net and GPIO values with the reviewed buffered
mapping before loading C10 on a real bus.

| Physical channel | sigrok | Net | GPIO (provisional) |
| --- | --- | --- | --- |
| CH1 | D0 | /AS or chosen transaction boundary | GP18 |
| CH2 | D1 | selection/decode evidence | GP22 |
| CH3 | D2 | R/W | GP19 |
| CH4 | D3 | /UDS or reviewed upper-lane strobe | GP20 |
| CH5 | D4 | /LDS or reviewed lower-lane strobe | GP21 |
| CH6 | D5 | D0 or reviewed low data bit | GP2 |
| CH7 | D6 | D8 or reviewed middle data bit | GP10 |
| CH8 | D7 | D15 or reviewed high data bit | GP17 |

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
