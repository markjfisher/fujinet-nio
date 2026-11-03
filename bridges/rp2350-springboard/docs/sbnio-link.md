# CS1 link and the NIO side

The Palm talks to the bridge firmware over the Springboard's second chip
select (CS1) and the firmware talks to NIO over a USB serial port. This page
describes both halves and the rules between them. [`include/sbnio.h`](../include/sbnio.h)
is the contract: it defines every register, offset, size and result code, and
this page deliberately names them rather than repeating their values. The
Palm library builds against that file in place
(`fujinet-nio-lib`'s `palmos-springboard` target includes
`bridges/rp2350-springboard/include/sbnio.h`), so a change to it is a change
to the Palm side too; bump `SBN_ABI_VERSION` when a value, layout or meaning
changes.

```
Palm app  --CS1 reads/writes-->  firmware (this bridge)  --USB CDC, packet link-->  NIO
          raw FujiBus packets                              raw FujiBus packets      (POSIX build)
```

A request and its reply are each one raw FujiBus packet
([native packet contract](../../../docs/native-packet-contract.md)): the bridge
neither parses nor changes them.

## Two windows

- **CS0** is only the module's ROM image. The Palm executes its apps in place
  from it. It carries no registers.
- **CS1** is the link: the base address is the module's `CsBase + CsSize`
  from `HsCardAttrGet`, and it is used with plain 16-bit `MOVE.W` cycles in
  both directions.

The two directions decode differently:

- **Reads** use A1-A15, so the window is 64 KiB of 16-bit words that repeats
  above that. Registers start at offset 0 and the reply buffer at
  `SBN_R_RESP`. Everything a read returns comes from a table the firmware
  keeps current, so **reads have no side effects**: the Palm can repeat or
  speculate on them freely.
- **Writes** use A16-A23 to choose a register, `SBN_W(reg)` bytes above the
  base, and all 16 data bits are captured; A1-A15 are ignored. A write is
  therefore always one word, and a request byte pair costs one bus cycle.

The firmware drives the data lines only to answer a read, through the read
engine described in the [README](../README.md), and never during a write.

## Making a request

1. Write `SBN_W_BEGIN` with the request length in bytes. This discards any
   half-built request.
2. Write the packet to `SBN_W_DATA`, two bytes per word, first byte in D15-D8;
   an odd last byte is padded with 0. Accumulate `SBN_CHECK_STEP` over the same
   words, starting from 0.
3. Write the sum to `SBN_W_CHECK`.
4. Write `SBN_W_DOORBELL` with a sequence number, 1-255 (the firmware console
   uses 0). The low byte is the sequence.

The firmware copies the sequence to `SBN_R_ACK_SEQ` at once. It then checks the
request: a length of 0, over `SBN_REQ_MAX`, or different from what BEGIN said
(`SBN_RES_BAD_LEN`), or words that do not match the CHECK value
(`SBN_RES_BAD_CHECK`) end the exchange without anything going to NIO. The
check word exists because a dropped or mistimed write is otherwise invisible
to the Palm; a bad check means resend the whole request from BEGIN.

## Reading the reply

The firmware writes the reply bytes, then `SBN_R_RESP_LEN`, `SBN_R_RESULT`
and `SBN_R_RESP_CHECK`, and the sequence number to `SBN_R_RESP_SEQ` **last**.
The Palm polls `SBN_R_RESP_SEQ` until it equals its sequence, then reads the
result, the length and the words from `SBN_R_RESP` (first byte in D15-D8) and
verifies the check word over them. A mismatch is a bad read, not a bad
reply: read it again.

Only one request is in flight. A doorbell while one is waiting abandons the
earlier one: it is dropped without a reply, and if NIO answers it later the
answer is discarded. `SBN_W_CONTROL` with `SBN_CTL_ABORT` ends a waiting
request with `SBN_RES_ABORTED`; `SBN_CTL_LOOPBACK_ON` and `_OFF` switch a
test mode in which the firmware answers every request with the request itself
and nothing goes to NIO. `SBN_R_STATUS` reports whether NIO's port is open
(`SBN_ST_HOST`), a request is waiting (`SBN_ST_BUSY`) and loopback is on.

### Results

| Result | Meaning | Did it run? |
| --- | --- | --- |
| `SBN_RES_OK` | the reply is NIO's response packet | yes |
| `SBN_RES_NO_HOST` | NIO's port is not open, or the link did not come up in 3 s | no |
| `SBN_RES_BAD_LEN`, `SBN_RES_BAD_CHECK` | the request was damaged on the bus | no; resend |
| `SBN_RES_REFUSED` | NIO's link refused it, e.g. over its capacity | no |
| `SBN_RES_TOO_BIG` | the reply is longer than `SBN_RESP_MAX`; it is not delivered | yes |
| `SBN_RES_UNANSWERED` | NIO took it and sent no answer | maybe |
| `SBN_RES_TIMEOUT` | nothing came back in 30 s | maybe |
| `SBN_RES_ABORTED` | the Palm aborted it | maybe |

Where it says maybe, the request may have run: do not resend it blindly.

## Size limits

The register map allows requests and replies of up to `SBN_REQ_MAX` and
`SBN_RESP_MAX` bytes, and the whole reply must fit the 64 KiB read window.
NIO's packet link has its own capacity, reported to the firmware in each
SyncAck (4096 bytes by default, `channel.packet_link.capacity` in
`fujinet.yaml`). The firmware refuses a request over it, and a Palm must not
ask for an answer larger than it; split larger transfers into several
requests, as the link rules say. The Palm cannot read the capacity over CS1
today: it is in the firmware console's `status`.

## The NIO side

NIO sees an ordinary FujiBus peer, so it needs no knowledge of the Visor. The
firmware is the **controller** of NIO's [packet link](../../../docs/packet_link.md):
records with a CRC and a generation, one request outstanding, explicit
completions. NIO has to run a profile that carries `FujiBusNative` over a
packet link on a serial port, the POSIX `fujibus-link-serial-debug` preset:

```sh
./build.sh -cp fujibus-link-serial-debug
FN_SERIAL_PORT=$(bridges/rp2350-springboard/scripts/sbcon.py --port "NIO link") \
    ./build/fujibus-link-serial-debug/fujinet-nio
```

The port is the first of the firmware's two USB serial ports ("NIO link"); the
second ("Console") is the text console. The port's baud rate does not matter.

What the firmware does with the link (`src/packet_link.c`):

- It opens the link when NIO opens the port, and starts a generation with Sync
  after staying quiet for longer than the peer's record timeout (the last
  SyncAck's, or 100 ms before the first), doubling the pause after each Sync
  that gets no SyncAck, up to 5 s.
- It sends a request only in a ready link, and sends it again, unchanged, only
  after an error that says it did not run (Corrupt, or NotSynchronised after
  NIO restarted), at most twice.
- After Unanswered, a timeout or an abort in flight, it resyncs with a new
  generation and never resends the request.
- It gives up on a request that finds no peer within 3 s (`SBN_RES_NO_HOST`).

NIO does not notice the board re-enumerating on USB after a flash or reset:
restart NIO when that happens. `sbcon.py "send 450106004c00"` injects a
`Clock.GetTime` request from the firmware console, as sequence 0, and shows
NIO's answer without a Palm.

`tests/test_packet_link.c` runs the controller against a scripted peer, and
pins the record bytes to what NIO's Python client
(`fujinet_tools.packetlink`) produces.
