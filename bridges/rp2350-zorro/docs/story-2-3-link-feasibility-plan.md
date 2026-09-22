# Story 2.3 — RP2350 ↔ ESP32-S3 Link Feasibility

## Status

Active feasibility / pre-ABI design work, updated after reviewed physical L0–L5
runs, the L8 sustained-performance matrix/boundary run, and the L10 DMA comparison.

The [link-feasibility evidence ledger](link-feasibility-evidence.md) indexes the
reviewed local reports and records the constraints they contribute. L8–L10 have
reviewed breadboard results, not production-rate conclusions; remaining L6–L7
evidence must still be consolidated before an ABI decision.

This work is independent of the real Zorro-II bus validation in Story 2.2.

Story 2.3 determines whether the RP2350 bridge and ESP32-S3 can exchange
complete opaque packets reliably, with bounded ownership, backpressure,
reset and recovery behavior.

The outcome feeds Story 2.4, where the production bridge ABI is selected.

Do not define the production ABI prematurely.

---

## Goal

Establish an evidence-backed transport between:

    RP2350B
       |
       | candidate physical link
       |
    ESP32-S3

that can eventually carry complete raw FujiBus packets.

This study must answer:

- Can arbitrary binary packets be transferred exactly?
- How are packet boundaries represented?
- What are the practical packet-size limits?
- How is readiness/backpressure handled?
- What happens when either endpoint resets?
- Can stale/incomplete data leak into a later packet?
- Can packets travel in both directions?
- What throughput and latency are achievable?
- Which guarantees belong in the eventual production ABI?

---

## Scope

In scope:

- RP2350B link feasibility firmware
- ESP32-S3 link feasibility firmware
- host/unit tests
- physical two-board experiments
- deterministic payload generation
- error/fault injection
- timing/throughput measurements
- reset/recovery behavior
- machine-readable evidence and summaries

Out of scope:

- Zorro register/mailbox layout
- final production bridge ABI
- FujiBus service interpretation
- DiskDevice behavior
- AutoConfig
- ESP32 application/service changes unrelated to the link
- network/storage behavior
- production DMA architecture or allocation decisions
- automatic retries after ambiguous completion

---

## Hardware

Available:

- Waveshare Core2350B
- ESP32-S3 development/breakout board with convenient GPIO headers
- USB connection to each endpoint
- logic analyzer
- breadboard / Dupont wiring
- shared 3.3 V signaling and common ground

Keep the feasibility fixture at 3.3 V.

---

## Initial Link Candidate

Start with SPI unless repository inspection gives a strong reason not to.

Possible initial wiring:

    RP2350          ESP32-S3

    SCLK   -------> SCLK
    MOSI   -------> MOSI
    MISO   <------- MISO
    CS     -------> CS

plus explicit handshake GPIOs if required, for example:

    READY
    DATA_AVAILABLE

Do not assume these signals are the final production interface.

The experiment series should determine whether explicit handshake/credit
signals are required.

---

## Architectural Boundary

The link transports opaque packets.

The RP2350 must not interpret FujiBus service/device fields.

Conceptually:

    complete packet
        |
        v
      RP2350
        |
      link
        |
        v
      ESP32

Packet contents are test payloads during feasibility work.

A test-only sequence number or checksum may be included in the lab envelope
for verification, but must not silently become part of the production
FujiBus protocol.

---

## Test Payload

Use deterministic payloads so corruption is unambiguous.

Each lab packet may contain:

    sequence
    payload_length
    deterministic payload bytes
    optional test checksum

Useful payload patterns:

- all zero
- all FF
- incrementing bytes
- alternating AA/55
- pseudo-random data from a fixed seed
- values containing 00, C0, DB and other bytes historically relevant to SLIP
- boundary sizes

Do not use production serialization as the only test oracle.

---

# Experiment Matrix

## Reviewed findings carried forward

L0–L5 have local passing two-board reports. Their raw evidence is retained under
`build/link-feasibility/` and indexed in the
[link-feasibility evidence ledger](link-feasibility-evidence.md). The findings
below update the questions that later experiments and Story 2.4 must answer;
they do **not** freeze a production ABI.

| Area | Confirmed current-fixture finding | Still open before ABI selection |
| --- | --- | --- |
| Framing/capacity | A 256-byte lab slot transfers payloads through 240 bytes; oversize and partial slots are explicitly rejected. | Required FujiBus capacity, production framing/checking, and incomplete-transfer disposition. |
| ESP-originated work | ESP can originate validated frames when the run establishes a fresh producer generation. | Reset/generation semantics independent of host flashing or boot order. |
| Readiness | The fixture uses `READY` for a queued slot and `DATA_AVAILABLE` for an advertised response. | Exact signal meanings, edge/level requirements, credit/acknowledgement or generation-token design. |
| Re-arm race | `DATA_AVAILABLE` remaining high after consumption cannot prove that a new response is ready. L5 requires a READY low-to-high re-arm boundary. L8 uses a 500 us hold without per-slot console pacing; its later 7.5 MHz failures are returned-frame checksum mismatches after an accepted request, not missed READY generations. | Whether the production design uses a measurable READY interval, explicit acknowledgement/credit, generation counter, or another mechanism; timing cost at the required rate. |
| Bidirectionality | A declared two-slot schedule passed without corruption or misattribution. | Concurrent ownership/priority/deadlock policy and behavior under sustained load. |
| Backpressure | Declared simulated receiver pauses and recovery passed. | Actual queue depth, occupancy, loss policy, latency and throughput under load. |
| High-speed fixture boundary | L8 repeatedly passed 64- and 240-byte batches through the RP2350 actual 6.818 MHz divider rate. At actual 7.5 MHz, returned echoes intermittently had a bad checksum while the ESP reported the request valid: 1/6 64-byte and 3/6 240-byte trials failed. | Repeat with the higher-rate analyzer, short controlled wiring and actual buffered Zorro hardware before setting a production clock or assigning the limit to either endpoint. |

## L0 — Fixed packet bring-up

Reviewed physical pass: one 16-byte request/echo at the declared fixture settings.

Purpose:

Prove the physical link works at all.

RP2350 sends one fixed packet.
ESP32 validates it and returns a fixed acknowledgement/echo.

Pass:

- exact bytes received
- exact length received
- one packet sent, one packet observed
- no framing ambiguity

Evidence:

- endpoint reports
- optional analyzer capture

---

## L1 — Arbitrary binary round-trip

Reviewed physical pass: twelve deterministic binary cases through 240 bytes.

Purpose:

Prove transparency.

Send a deterministic set of packets containing arbitrary byte values and
multiple lengths.

Include:

- 0-byte/empty case if permitted
- 1 byte
- small packets
- expected FujiBus-sized packets
- maximum candidate size
- 00/FF/AA/55/C0/DB-heavy payloads

Pass:

- byte-for-byte round trip
- no corruption
- no inserted/removed bytes
- packet boundaries preserved

---

## L2 — Packet-boundary and size limits

Reviewed physical pass: 240-byte maximum test payload, explicit oversize and partial rejection, then recovery.

Purpose:

Determine the real packet-delivery contract.

Exercise:

- back-to-back packets
- minimum size
- maximum size
- maximum+1 rejection
- partial/truncated transfer
- delayed second packet

Pass:

- packets never merge
- packets never split into an accepted false packet
- oversize/truncation behavior is explicit
- practical maximum capacity is measured

Deliverable:

documented candidate packet-capacity requirement for Story 2.4.

---

## L3 — Receiver pressure / backpressure

Reviewed physical pass for the declared simulated pause cases and recovery; this is not yet a queue-capacity measurement.

Purpose:

Determine what happens when one endpoint cannot consume quickly enough.

Procedure:

- sender produces packets continuously
- receiver deliberately pauses draining
- vary pause and queue depth

Measure:

- queue occupancy
- blocked sends
- dropped packets
- corrupted packets
- latency after recovery

Pass:

- overload behavior is bounded and detectable
- existing queued data is not silently corrupted
- recovery behavior is defined

Deliverable:

evidence for whether production design needs:

- READY
- credits
- queue depth
- multiple buffers
- blocking semantics

---

## L4 — Reverse direction

Reviewed physical pass: six ESP-originated frames, sequences 1–6, at lengths 1, 64 and 240 bytes.

Purpose:

Prove ESP32 → RP2350 delivery independently.

ESP32 sends packets without first receiving a corresponding RP request.

Pass:

- exact packet/length observed
- packet boundary preserved
- notification/readiness mechanism works

This is not permission for arbitrary concurrent FujiBus exchanges.

---

## L5 — Bidirectional scheduling

Reviewed physical pass: six scheduled cases after making the READY re-arm boundary explicit and observable.

Purpose:

Exercise both directions under controlled load.

Do not assume true simultaneous full-duplex packet semantics are required.

Test:

- alternating directions
- queued opposite-direction traffic
- traffic arriving while the other endpoint is busy

Pass:

- ordering rules are explicit
- no packet misattribution
- no deadlock
- no corruption

---

## L6 — RP2350 reset mid-transfer

Purpose:

Prove reset containment.

Inject reset at multiple phases:

- idle
- header/start
- mid-payload
- after payload before completion
- after completion notification

Pass:

- ESP32 reaches known state
- partial packet is discarded
- no stale completion is attributed to the next transfer
- link can be re-established deterministically

---

## L7 — ESP32 reset/disconnect mid-transfer

Mirror L6 from the opposite side.

Pass:

- RP2350 detects unavailable/reset peer
- ownership is released
- incomplete packet cannot later appear as valid
- restart does not replay an ambiguous packet automatically

---

## L8 — Sustained performance

Reviewed current-fixture result: the 3×3 matrix and boundary sweep retained in
`build/link-feasibility/L8/20260922T203250Z-455e33e2` passed repeated 64- and
240-byte batches through actual 6.818 MHz. At actual 7.5 MHz, the return path
showed intermittent checksum corruption that worsened with payload size. This
marks a long-Dupont/breadboard fixture boundary, not a chip or Zorro conclusion.
Revisit with the higher-rate analyzer, a controlled harness, and real buffered
Zorro hardware.

Measure:

- throughput
- per-packet latency
- CPU load if useful
- queue occupancy
- effect of packet size
- effect of clock rate

Use representative FujiBus packet sizes.

Do not invent a required performance target first.

Record what the link actually achieves.

---

## L9 — Recovery soak / repeated faults

Reviewed physical pass: 100 exact 64-byte transfers with nine deliberately
truncated slots. Each partial slot produced an explicit `bad_checksum` error,
was drained, and was followed by valid traffic. The complete evidence is
`build/link-feasibility/L9/20260922T220158Z-dff6d19f`.

This run also exposed and corrected a meaningful state-machine rule: an injected
partial transfer is not retired merely because the previous data-available level
was low. The initiator must wait until the peer advertises that partial transfer's
error outcome, then consume/retire it before submitting new work. Otherwise the
next request can receive the stale error response. This is input to an eventual
error-completion/clear rule, not a production signaling choice.

L9 does not cover arbitrary physical resets, counter wrap, or real disconnect.

---
## L10 — SPI-DMA datapath comparison

Purpose:

Separate RP2350 master-side SPI FIFO servicing cost from the existing test-slot,
request/echo and readiness costs before attributing the current L8 throughput to
the physical link.

Compare the existing CPU-driven SPI transfer with SPI DREQ DMA at the same W2
wiring, fixed slot, ESP endpoint and ownership behavior. Use 16, 64 and 240-byte
payloads at requested 1, 4 and 7 MHz. The 7 MHz request selects the demonstrated
actual 6.818 MHz RP2350 divider rate; do not use the intermittent 7.5 MHz
breadboard boundary as a DMA performance target.

Record per-exchange means for:

- waiting for READY;
- request-slot transfer;
- waiting for the response;
- response-slot transfer;
- READY re-arm;
- total payload rate.

Pass:

- both polling and DMA preserve exact frames for all declared cases;
- the report identifies the actual SPI divisor rate and datapath;
- phase timings show where time is spent without treating the current 500 us
  READY interval or fixed two-slot envelope as ABI requirements.

Reviewed physical result: all 54 polling/DMA batches passed in
`build/link-feasibility/L10/20260922T215630Z-6d6bc234`. At actual 6.818 MHz and
240 bytes, polling achieved 105,655 B/s and DMA 105,646 B/s, which is within
measurement noise. Both paths spent 357 us on each 256-byte slot; the 907 us
ESP echo-preparation wait and 480 us READY re-arm dominate the remaining time.
DMA therefore does not improve the present fixture's throughput, though it
remains relevant if the final engine needs CPU concurrency or descriptor queues.

This is **SPI peripheral plus DMA**, not a PIO experiment. PIO remains the
candidate timing owner for the Zorro-facing capture/response path; any production
DMA/descriptor design remains a Story 3.2 decision after ABI approval.

---

# Firmware Structure

Do not begin by integrating deeply into production FujiNet behavior.

Prefer small feasibility targets under the existing bridge project.

Conceptually:

    bridges/rp2350-zorro/
        lab/
            rp2350-link/
            esp32-link/

or another repository-consistent structure identified during implementation.

Requirements:

- reproducible builds
- RAM-load where useful during experimentation
- USB console/control protocol
- counters and machine-readable report commands
- shared deterministic test-vector definitions where practical

The feasibility firmware is lab equipment, not the production ABI.

---

## Host Tests

Where practical:

- model packet/framing state machines on host
- test queue transitions
- test reset state
- test oversize/truncation
- test deterministic vectors
- test report parsing

Physical tests remain required for timing and real endpoint behavior.

---

## Evidence Format

Follow the Story 2.2 feasibility philosophy.

Each physical run should retain:

- experiment manifest
- exact firmware/build identity for both endpoints
- separate RP2350 and ESP32 endpoint console logs
- SHA-256 identities and byte sizes for both endpoint images
- source revision and dirty-file state
- analyzer capture where relevant
- report.json
- concise summary output

A completed experiment must distinguish:

- software-only evidence
- physical link evidence
- timing evidence
- remaining untested conditions

---

## Reporting

Provide a reusable summary tool rather than experiment-specific prose.

Example:

    Experiment: L3 — Receiver pressure
    Status: passed
    RP packets sent: 128
    ESP packets accepted: 128
    Blocked sends: 37
    Dropped packets: 0
    Corrupt packets: 0
    Max queue depth: 4
    Recovery latency: ...
    Evidence scope: physical two-board link

---

# Exit Criteria for Story 2.3

Story 2.3 may produce a positive feasibility result when:

- arbitrary binary packets transfer exactly;
- packet boundaries and maximum supported sizes are known;
- backpressure behavior is measured and bounded;
- both directions operate correctly;
- reset of either endpoint cannot leak stale/partial packets into a later
  exchange;
- recovery is deterministic;
- throughput/latency are measured;
- remaining uncertainties are explicitly documented.

The output is evidence and constraints for Story 2.4.

It is not itself the production ABI.

---

# Inputs to Story 2.4

Story 2.3 should end with concrete statements such as:

- maximum required packet size
- preferred physical link and tested rate
- whether READY/credit signaling is required
- required queue depth
- packet ownership rules
- completion semantics
- reset/generation behavior
- stale-transfer rejection requirements
- measured latency/throughput
- unsupported or risky cases

Story 2.4 combines these with:

- the accepted raw FujiBus software contract from Epic 1;
- the Zorro/RP2350 evidence from Story 2.2.

Only then should register maps, mailbox layout, queues, doorbells and the
production bridge ABI be frozen.