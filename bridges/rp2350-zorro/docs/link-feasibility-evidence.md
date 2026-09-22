# Story 2.3 link-feasibility evidence ledger

This ledger preserves the reviewed physical outcomes of L0–L5 and the
constraints they contribute to a later bridge ABI decision. It is an index, not
a replacement for the authoritative local evidence: each retained run directory
contains `report.json`, `console.log`, `capture.sr`, and, where decoding was
possible, `waveform.svg`.

The frames, sequence numbers, CRCs and GPIO names below are **lab-envelope**
properties. They are evidence for an eventual transport contract; they are not a
proposed FujiBus packet format or production register ABI.

## Reviewed physical runs

| Case | Reviewed local run | What the run establishes |
| --- | --- | --- |
| L0 | `build/link-feasibility/L0/20260921T232904Z-2f4cde47` | One 16-byte deterministic RP2350 request was accepted and echoed exactly across the six-wire SPI fixture. |
| L1 | `build/link-feasibility/L1/20260922T112249Z-801be986` | Twelve round trips passed for 0, 1, 16, 64, 128 and 240-byte payloads, including zero, FF, alternating, fixed-random and SLIP-like byte patterns. |
| L2 | `build/link-feasibility/L2/20260922T112400Z-e8e6b85b` | The current 256-byte test slot carries at most 240 payload bytes. A 241-byte local request was rejected; a deliberately truncated 32-byte slot was rejected by the peer; later transfers recovered. |
| L3 | `build/link-feasibility/L3/20260922T132021Z-a6947dbb` | The fixture completed declared 0, 1, 10 and 100 ms simulated receiver-pause cases and a follow-up transfer. It demonstrates bounded waiting/recovery at this clock and wiring, not a measured production queue capacity. |
| L4 | `build/link-feasibility/L4/20260922T165254Z-5a314722` | Six ESP-originated frames, sequences 1–6 and lengths 1/64/240, were received and validated by the RP2350 without a corresponding RP request. |
| L5 | `build/link-feasibility/L5/20260922T170810Z-c31384ff` | Six scheduled bidirectional cases passed: each RP request shared a slot with the expected ESP frame, then its echo was received in the next re-armed slot. |
| L8 | `build/link-feasibility/L8/20260922T203250Z-455e33e2` | The 3×3 50-transfer matrix and a 5/6/7/8 MHz boundary sweep passed repeatedly through the RP2350's actual 6.818 MHz divider rate for 64- and 240-byte payloads. At the actual 7.5 MHz rate, returned-frame checksum failures were intermittent: 1/6 64-byte and 3/6 240-byte trials failed; 16-byte trials passed 3/3. |

## Findings that constrain the ABI design

| Finding | Evidence and implication for Story 2.4 |
| --- | --- |
| Candidate payload capacity is at least 240 bytes in the current fixed-slot fixture. | L1/L2 pass at 240 bytes and L2 rejects 241 bytes. This is a lab implementation limit, not yet a required FujiBus capacity or a final frame-size choice. |
| A corrupt or incomplete slot must be explicit and must not become a later valid packet. | L2's 32-byte partial transfer produced an explicit peer rejection, followed by valid recovery. The ABI needs an equally explicit incomplete-transfer/reset disposition. |
| Independent ESP-originated work needs a reset/start generation rule. | L4 was made deterministic by loading the RP2350 first and then restarting the ESP queue, yielding sequences 1–6. A production design needs a generation/initialization rule; a boot order alone is not sufficient as an ABI rule. |
| `DATA_AVAILABLE` alone cannot identify a newly prepared response. | L5 initially saw the level remain high after the previously advertised ESP frame had been consumed. The lab fix requires a READY low-to-high re-arm boundary before the next slot is clocked. The production ABI must choose an unambiguous ownership/generation mechanism: a documented READY transition with a minimum observable interval, a generation token, an acknowledgement/credit, or an equivalent design. |
| A level transition needs a measurable minimum duration. | L5's reviewed run held READY low for at least 100 us between slot generations. L8 now uses a 500 us re-arm interval and its reviewed matrix/boundary run did not reproduce the earlier missed-generation timeout. Its 7.5 MHz failures are immediate returned-frame checksum mismatches after an accepted request, a distinct condition from re-arm timing. This is fixture evidence, not a proposed production timing value. |
| The current passing bidirectional behavior is scheduled, not proof of arbitrary simultaneous packet ownership. | L5 validates one RP request plus one already-queued ESP frame, followed by a separate echo slot. The ABI still needs explicit concurrency, priority and deadlock rules. |
| Receiver unavailability is observable in this fixture. | L3's declared simulated pauses held the master until the ESP endpoint became ready and then recovered. Actual queue depth, sustained pressure behavior, throughput and latency remain L6–L9 work. |
| The current breadboard fixture has an intermittent high-speed return-path boundary. | In L8, 64- and 240-byte batches passed 3/3 at the RP2350's actual 6.818 MHz divider rate. At actual 7.5 MHz, the ESP accepted requests (`peer=ok`) but the RP2350 observed `bad_checksum` on returned echoes: 64 bytes failed 1/6 trials and 240 bytes failed 3/6. The 16-byte case passed 3/3, so that rate is not established as reliable. Treat <=6.818 MHz as the demonstrated repeated-large-payload rate on this long Dupont/breadboard fixture; re-test the boundary with a short controlled harness, the higher-rate analyzer, and actual buffered Zorro hardware. This does not set a production link-rate limit. |

## Evidence limits and follow-up

The reports prove functional behavior on the declared two-board 3.3 V fixture
and retain analyzer captures. They do not establish a production clock rate,
latency bound, queue capacity, reset containment, disconnect handling, or a
final ownership model. L8 records a fixture-specific repeated-payload boundary
through 6.818 MHz and intermittent corruption at 7.5 MHz; its long Dupont leads
and breadboard contacts are part of that observation. Revisit it with the
higher-rate analyzer, a controlled harness, and actual buffered Zorro hardware
before making an architecture or ABI decision.

L6 injects a partial slot then force-reloads the RP2350 SRAM image; L7 requests
a controlled ESP32 restart at a completed slot boundary; L8 batches declared
payload sizes/clocks without USB pacing; L9 repeatedly injects and recovers from
partial slots. These procedures deliberately do not claim arbitrary clock-edge
reset or physical disconnect evidence.

Each new `all` link report records SHA-256 and byte size for the RP2350 ELF and ESP32
ELF/bin, the repository revision and dirty-file list, and separate RP2350 and
ESP32 console transcripts. Preserve the relevant local output directories: those
fields identify the run, while the raw capture and waveform explain its physical
behavior. Do not rely on the ledger alone for a release or ABI decision.
