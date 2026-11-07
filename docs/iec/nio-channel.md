# NIO packet channel on IEC

One IEC device carries whole raw FujiBus packets between a Commodore program and
FujiNet NIO. The implementation is `IecNioFrontend`; the C client is the `c64` target
of `fujinet-nio-lib`.

## Contract (v1)

| | |
|---|---|
| Device | 30 |
| Channel | 0 (secondary `$60`). Other channels take no data and send nothing |
| Capacity | 512 bytes each way, the v1 baseline. No segmentation |
| Outstanding | One request at a time |

**Request.** `LISTEN 30`, `SECOND $60`, the packet's bytes, `UNLISTEN`. UNLISTEN ends
the packet, and the request runs then; EOI on a data byte means nothing here, because
the emulator's path does not deliver it. There is no SLIP, padding or checksum beyond
the packet's own.

**Response.** `TALK 30`, `TKSA $60`, bytes up to the length in the packet's own header
(bytes 2-3, little endian), `UNTALK`. EOI comes with the last byte. Each TALK reads
from the start of the response, so a client that lost one can read it again; it stays
until the next request or a reset.

**Nothing to send.** A read with no response waiting ends at once: the first read
returns no byte, with EOI. On a C64 that is `ST = $42`.

**Refusals.** A packet over capacity is refused from the byte that overflows it (a
write taking 0 bytes, `ST` bit 7 on the C64) and does not run. A malformed packet does
not run and leaves no response. A response over capacity becomes an `InternalError`
response, which fits.

**Empty sessions.** `LISTEN`, `SECOND`, `UNLISTEN` with no data runs nothing and leaves
any response waiting. That is how a client checks the device is there.

**Slow requests.** The device holds the bus while a request runs, as a drive does
while it seeks, so a slow request is a slow first byte and a client has no timeout to
manage. On the emulator's path the whole machine waits, up to the bridge's reply limit
(`NETIEC_TIMEOUT`, 30 s by default).

**Reset.** A bus reset drops any response and abandons the program's network sessions.

## Strings

Everything crossing FujiBus is ASCII. A C64 program keeps its strings in ASCII and
converts only at its own screen and keyboard; see the library's transport notes.
