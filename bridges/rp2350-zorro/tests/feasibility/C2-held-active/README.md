# C2-held-active: one capture during a held `/AS`

**C2 is a two-board W0 experiment.** The RP2040 holds `D0..D3=3` for 200 us,
asserts `/AS` once, then changes the data through `A`, `5`, and `C` while `/AS`
stays low. It releases `/AS` after 830 us total. The Core2350B must report one
capture and one capture IRQ, with the single value `3`. Any second capture while
the strobe remains low fails the experiment.

The saved run includes `capture.sr`, the machine-readable `report.json`, both
USB console logs, and `waveform.svg`. The SVG begins with the whole acquisition
and highlights the small detected event window, then shows the original saved
logic-analyser D0–D3 and `/AS` traces around that window. It labels the decoded
held-active phases and the DUT-reported capture. The falling `/AS` edge is the
capture transaction boundary. W0 has no physical pin marking the exact internal
PIO `IN PINS` clock, so the diagram states that limitation rather than claiming a
false nanosecond sample position; `D=3` remains stable for 210 us after assertion.

Wiring is unchanged: RP2040 GP2–GP5 to Core2350B GP2–GP5, RP2040 GP6 (`/AS`) to
Core2350B GP1, shared ground, analyzer CH1–CH4 on D0–D3 and CH8 on `/AS`.

```sh
./run.sh build
./run.sh doctor
./run.sh load --usb-path 7-1.3.3.4.4 --dut-usb-path 7-1.3.3.4.3
./run.sh run --output /tmp/c2-run-001
python3 ../report_summary.py /tmp/c2-run-001/report.json
```

The C2 fixture is flash-installed like C1. Since this bench already runs the
C1 flash fixture, C2 loads without BOOTSEL. A fresh board still running only the
Debug Probe needs one initial RP2040 BOOTSEL install; later loads use the same
command with both boards connected normally. C2 uses APIO source, host EPIO
tests and the shared DUT observer; it contains no `.pio` text source or
production bridge ABI.
