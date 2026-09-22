# L10 — SPI DMA datapath comparison

L10 asks a narrow implementation question: with the same 256-byte test slot,
two-slot request/echo exchange, READY re-arm behavior and ESP32-S3 endpoint,
how much time belongs to RP2350 CPU-driven SPI FIFO servicing versus SPI-DREQ
DMA? It is not a PIO experiment. The RP2350 hardware SPI peripheral owns SCLK,
MOSI and MISO; two DMA channels move a complete slot between its TX/RX FIFOs
and fixed RAM buffers. ARM still owns CS, READY/DATA_AVAILABLE observation,
frame validation and the next-slot decision.

Each polling/DMA cell runs three 50-transfer trials at requested 1, 4 and 7 MHz
(the RP2350 reports the actual divisor rate; 7 MHz currently becomes about
6.818 MHz). Payloads are 16, 64 and 240 bytes. The current breadboard is known
to be intermittent at actual 7.5 MHz, so this experiment deliberately does not
use that rate.

## Wiring

| Signal | Core2350B RP2350 | ESP32-S3 lab default | Analyzer |
| --- | --- | --- | --- |
| GND | GND | GND | GND |
| SCLK | GP2 | GPIO12 | CH1 |
| MOSI | GP3 | GPIO11 | CH2 |
| MISO | GP4 | GPIO13 | CH3 |
| CS | GP5 | GPIO10 | CH4 |
| READY | GP6 input | GPIO9 output | CH5 |
| DATA_AVAILABLE | GP7 input | GPIO8 output | CH6 |

Use the unchanged W2 fixture with 3.3 V signalling and common ground. CH1–CH6
map to analyzer D0–D5. The analyzer documents representative activity; the
RP2350 phase timings are the performance evidence.

## Run

```sh
./run.sh all --output /tmp/l10-run-001
```

The report's `performance` rows group results by `datapath`, actual SPI rate and
payload size. For L10 each result also contains mean RP-side time per exchange:
`ready_wait_us_mean`, `request_transfer_us_mean`,
`response_wait_us_mean`, `response_transfer_us_mean`, and `rearm_us_mean`.
Those means include only the specified phase; they do not turn the fixed slot,
500 us READY interval, or current breadboard timing into an ABI promise.
