# NetIEC

IEC bus transactions from an emulator's bus master, over a byte stream. It is the wire
protocol of the `opencbm-netiec` bridge that VICE loads as its real-device library, so
that bridge works against this firmware unchanged. `NetIecServer` is the protocol and
`NetIecIngress` runs it over a `Channel`: on POSIX, a TCP listener on `channel.tcp_port`,
1541 by default in the `c64-netiec` profiles.

Every frame is `[command u8][payload length u16le][payload]`. The client sends one
request and reads exactly one reply, whose command is the request's with bit 7 set.
An unknown request gets `0xFF` with the request's command as its payload.

| Request | Payload | Reply payload |
|---|---|---|
| `0x01` HELLO | version, client name | version (1), `u32le` mask of devices that answer, server name |
| `0x02` ATN | the bytes sent under ATN: a primary and usually its secondary | `0` acknowledged, `1` not present |
| `0x03` WRITE | flags (bit 0: EOI on the last byte), data | `u16le` bytes the listener took |
| `0x04` READ | `u16le` most bytes wanted | flags (bit 0: EOI, the talker is done), data |
| `0x05` RESET | none | none |

A request does all its work before its reply: the reply to the UNLISTEN that ends an
OPEN, or a packet, comes after it has run. Bus work runs on the core loop, which the
channel wakes when bytes arrive; the bridge makes a round trip per bus byte, so a loop
that slept between requests would cost about a millisecond a byte.

HELLO starts a connection, which may be the same machine reconnecting: devices keep
their state, and only an unfinished transaction is dropped. A reset of the machine
arrives as its own RESET.

What the bridge does and does not deliver is in the workspace's
[behaviour research](../../../../_bmad-output/planning-artifacts/research/technical-c64-iec-bus-behaviour-2026-11-05/research.md)
(available in the parent workspace layout). In short, VICE sends one byte per WRITE and
READ, never sets EOI on a WRITE, and ignores the ATN reply, so an absent device shows
up as a refused WRITE.
