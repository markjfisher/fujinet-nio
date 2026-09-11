# FujiNet-NIO Protocol Reference  
**FujiBus + SLIP Protocol Specification**  
*Version 1.0 — 2025-12*

This document describes the **binary protocol** used between a host machine  
(Atari, C64, Coleco, emulator, etc.) and the FujiNet-NIO core.  

It covers:

- SLIP framing
- FujiBus header format
- Descriptor encoding
- Parameter encoding rules
- Payload semantics
- Checksums
- End-to-end packet examples
- Differences from legacy FujiNet firmware

This document is authoritative for transport implementers and virtual device authors.

---

# 1. Overview

FujiNet uses a **two-layer protocol stack**:

```
[ SLIP Frame ]
    ↓
[ FujiBus Packet ]
    ↓
[ Device-specific semantics ]
```

SLIP defines serial message boundaries. FujiBus defines the binary packet format.
The C++ codec also exposes raw packets for callers with explicit packet boundaries;
framing is selected by API, never detected from packet bytes.

Both layers are used identically across:
- ESP32 USB CDC
- Serial RS232 adapters
- POSIX PTY channels
- Emulators and test harnesses

---

# 2. SLIP Framing

SLIP (RFC 1055) is used to delimit packets and escape reserved bytes.

FujiNet-NIO defines:

```
SlipByte::End     = 0xC0
SlipByte::Escape  = 0xDB
SlipByte::EscEnd  = 0xDC
SlipByte::EscEsc  = 0xDD
```

The SLIP frame format:

```
C0 …payload… C0
```

### 2.1. Escaping Rules

| Byte | Meaning | Encoded As |
|------|---------|------------|
| 0xC0 | END     | DB DC       |
| 0xDB | ESC     | DB DD       |
| other | literal | literal    |

### 2.2. Decoding Rules

To decode a SLIP frame:

1. Find the first `0xC0` → start of frame  
2. Read bytes until the next `0xC0`  
3. Process escape sequences  
4. The resulting byte stream is the FujiBus packet  

Multiple frames may occur in the same stream.

---

# 3. FujiBus Packet Format

A raw FujiBus packet (also the result of SLIP decoding) has this structure:

```
+-------------------------+
| Header (6 bytes)        |
+-------------------------+
| Descriptor Block (N)    |
+-------------------------+
| Parameter Fields (M)    |
+-------------------------+
| Payload (optional)      |
+-------------------------+
```

---

# 4. FujiBus Header

The header is **exactly 6 bytes**, little-endian where applicable. This schematic
describes wire fields, not a native struct to copy. The C++ codec reads/writes
byte offsets: device 0, command 1, length 2–3, checksum 4, descriptor 5:

```
struct FujiBusHeader {
    uint8_t  device;    // Destination device ID
    uint8_t  command;   // Command code
    uint16_t length;    // Total packet length INCLUDING header
    uint8_t  checksum;  // Checksum over the full packet with checksum field = 0
    uint8_t  descr;     // First descriptor byte
};
```

### 4.1. Header Example

```
01 02 06 00 09 00
│  │  │   │  │
│  │  │   │  └─ First descriptor byte
│  │  │   └──── Checksum
│  │  └──────── Length = 0x0006
│  └─────────── Command = 0x02
└────────────── Device = 0x01
```

---

# 5. Descriptor Encoding

Descriptors describe **how many parameter fields follow**, and their **byte widths**.

Descriptor byte layout:

```
bit7       bit6..3             bit2..0
continued  reserved (ignored)  field descriptor index (0-7)
```

### 5.1. Descriptor Flags
- **bit 7 = 1** → Additional descriptor follows after this one.
- **bit 7 = 0** → This is the final descriptor.
- Bits 6–3 are ignored when decoding. Index zero contributes no fields,
  including in a continued descriptor. A missing continuation byte is invalid.

### 5.2. Descriptor Tables

Descriptors are translated using fixed lookup tables:

#### Number of Fields
```
numFieldsTable[8] = {0,1,2,3,4,1,2,1}
```

#### Field Sizes
```
fieldSizeTable[8]  = {0,1,1,1,1,2,2,4}
```

Examples:

| Descriptor (lower 3 bits) | Meaning |
|---------------------------|---------|
| `0` | No parameters |
| `1` | One 1-byte param |
| `2` | Two 1-byte params |
| `3` | Three 1-byte params |
| `4` | Four 1-byte params |
| `5` | One 2-byte param |
| `6` | Two 2-byte params |
| `7` | One 4-byte param |

### 5.3. Multi-Descriptor Packets

If a packet contains many parameters, they may span multiple descriptor bytes.

Example:

```
85 82 01
```

Means:

- `0x85` → (bit7=1) → more descriptors follow
- `0x82` → (bit7=1) → more descriptors follow
- `0x01` → last descriptor

---

# 6. Parameters

Parameters are encoded after all descriptors.

Each parameter is written in **little-endian** form:

```
1-byte param:  [AA]
2-byte param:  [AA BB]
4-byte param:  [AA BB CC DD]
```

FujiBusPacket internally stores parameters using:

```
struct PacketParam {
    uint32_t value;
    uint8_t  size;   // 1,2,4
};
```

The order of parameters is exactly the order they appear on the wire.

---

# 7. Payload

Any remaining bytes after parameters are treated as **payload**.

```
payload = decoded_bytes[offset ... end]
```

Payloads may contain:
- Filename data
- Network packets
- Disk read/write buffers
- JSON or text data
- Device-specific binary encodings

Payload is fully opaque to the transport layer.

---

# 8. Checksum

Checksum algorithm:

```
sum = 0
for each byte:
    sum += byte
    sum = (sum >> 8) + (sum & 0xFF)   // fold carry into low byte
return sum
```

Before computing checksum:
- Temporarily set header.checksum = 0
- Compute across the entire FujiBus packet (NOT including SLIP boundaries)

Validation requires:

```
computed == header.checksum
```

---

# 9. IORequest Mapping

The Transport layer converts parsed FujiBusPacket fields into an `IORequest`:

```
IORequest {
    RequestID   id        = generated incrementing integer
    DeviceID    deviceId  = header.device
    RequestType type      = derived from command mapping
    uint8_t     command   = header.command
    vector<uint8_t> payload = remaining payload
}
```

Virtual devices *must* echo back:
- `id`
- `deviceId`

They may rewrite:
- `status`
- `command`
- `payload`

---

# 10. IOResponse Mapping

The transport layer SLIP-encodes and FujiBus-encodes:

```
IOResponse {
    id
    deviceId
    status
    command
    payload
}
```

Transport→FujiBus mapping:

```
device     = response.deviceId
command    = response.command (or request.command)
length     = header + additional descriptors + params + payload
checksum   = computed
descriptor = emitted based on param sizes
param[0]   = response.status (one U8 parameter)
```

---

# 11. End-to-End Example

## Request sent from host:

```
C0
01 10 0B 00 03 01
AA          // one 1-byte parameter
DE AD BE EF // payload
C0
```

Breakdown:
- SLIP frame (`C0 ... C0`)
- Device = 0x01
- Command = 0x10
- Length = 0x000B
- Descriptor = 0x01 → one 1-byte parameter
- Param = 0xAA
- Payload = DE AD BE EF

---

## Response returned by FujiNet:

```
C0
01 10 0B 00 58 01
00          // success status as U8 param[0]
DE AD BE EF // updated payload
C0
```

Where:
- Command is the response command; status is carried separately in U8 param[0].
- Payload may change based on device behavior.
- Both examples contain 11 raw bytes. The request sum is `0x3FF`, folded to
  `0xFF + 0x03 = 0x102`, then `0x02 + 0x01 = 0x03`.
  The success response sum is `0x355`, folded to `0x58`.

---

# 12. Differences vs. Legacy FujiNet

### FujiNet-NIO improves:

- **Strict separation** of protocol and device logic  
- Full C++20 implementation with strong typing  
- Clear namespace: `fujinet::io::protocol`  
- Complete SLIP + FujiBus tested independently  
- Multi-transport support  
- Deterministic parsing suitable for fuzzing  

Legacy firmware mixed:
- RS232/SIO semantics
- Device interpretation  
- Transport mechanics  
into tightly-coupled code.

NIO fixes that.

---

# 13. Error Handling Rules

### 13.1. Invalid Checksum  
Transport must drop the packet silently.

### 13.2. Unknown Device  
Transport must produce an `IOResponse`:

```
status = StatusCode::DeviceNotFound
```

### 13.3. Unsupported Command  
Device returns:

```
status = StatusCode::Unsupported
```

### 13.4. Partial SLIP frames  
Transport must buffer until complete.

### 13.5. Oversized packets  
The raw codec accepts total packet lengths from 6 through 65535 bytes inclusive.
`serializeRaw()` returns an empty buffer if header + all descriptors + parameters
+ payload exceeds 65535, checked with bounded arithmetic before building output.
`fromRaw()` returns null for lengths outside this range or any mismatch between
the encoded length and supplied buffer. Channel/transport limits may be smaller.

For compatibility, legacy `serialize()` still serializes oversized packets with
a wrapped uint16 length; `fromSerialized()` rejects the resulting length mismatch.
This historical output is not a valid raw packet contract.

---

# 14. Future Protocol Extensions

### Planned:
- Multi-command batches
- Secure FujiBus variants (device authentication)
- Streaming mode (for modem / network passthrough)
- Retry/correlation semantics
- Extended-length descriptors (> 64 KB packets)

---

# 15. Reference Implementations

### C++ (official)
Located in:

```
include/fujinet/io/protocol/fuji_bus_packet.h
src/lib/fuji_bus_packet.cpp
src/lib/fujibus_transport.cpp
```

### Explicit C++ codec contract

- `serializeRaw()` emits exactly FujiBus bytes, without SLIP boundaries/escaping.
- `fromRaw(bytes)` consumes exactly one raw packet. C0/DB are literal bytes even
  in the header; SLIP-wrapped input is never automatically unwrapped. Invalid
  checksum, short header, length mismatch, or truncated descriptors/parameters
  returns null, with no partial public packet. Bytes outside the declared length
  are rejected; remaining bytes inside the length are opaque payload. No remaining
  bytes means no payload value, including after serializing an empty payload.
- `serialize()` / `fromSerialized(bytes)` remain the serial compatibility APIs.
  The parser skips noise before the first END, requires a final END, and decodes
  only the first frame. Consecutive leading ENDs produce an empty first frame and
  fail. Later frames/trailing bytes are ignored only if the full input ends in END.
  Unknown escape pairs are discarded, including DB END consuming that END; a
  dangling escape without a final END fails framing validation. These are retained
  C++ compatibility quirks, not new requirements for other SLIP implementations.
- Descriptor reserved bits and zero-count continuations are accepted, but
  re-serialization uses canonical descriptor grouping, without reserved bits.

### Caller and cross-language audit

The production C++ caller is `src/lib/fujibus_transport.cpp`: `receive()` and
`receiveResponse()` use `fromRaw()`, and `send()` uses `serializeRaw()`.
Every `IFramer` exchanges raw opaque packets with that transport. `SlipFramer`
extracts serial frames, decodes their escaping, and encodes outgoing raw packets.
The pure helpers in `include/fujinet/io/protocol/slip_codec.h` are shared with
legacy `serialize()` / `fromSerialized()` wrappers, whose framing validation
remains separate. Transport status and parameter mapping is unchanged.

`src/lib/bootstrap.cpp` still selects SlipFramer or NativeFramer at construction.
NativeFramer remains a placeholder that merges channel reads; this framing
separation does not establish native packet boundary safety. The Zorro profile
still uses its placeholder channel. `AtariSioFujiBusFramer` operates beneath
Channel, not at the IFramer boundary, and continues carrying SLIP bytes in its
SIO envelopes. Its composition regression uses the real transport and SlipFramer.

Packet and transport mapping/framing tests retain serial coverage. Independent
literal raw/SLIP expectations in `tests/fujibus_wire_fixtures.h` verify raw
framer parity, serial wire bytes, every binary-frame split (including escape
pairs), separators, and shared malformed-escape compatibility. The mapping test
uses SlipFramer directly; it has no StubFramer requiring conversion.

The unchanged sibling `fujinet-nio-lib` C encoder's
`src/common/fn_packet_header.c::fn_build_header()` and
`src/common/fn_packet_checksum_packet.c::fn_calc_packet_checksum()` and Python
`py/fujinet_tools/fujibus.py::build_fuji_packet_decoded()` corroborate the six-byte
header, explicit LE length and folded checksum. Python `build_fuji_packet()` adds
SLIP explicitly, and `build_fuji_response_wire()` encodes status as U8 param[0].
This audit concerns common wire encoding; malformed SLIP behavior is not identical
across languages (C++ discards unknown escape pairs).

### Python (testing)
```
scripts/fujinet help
```

Supports:
- SLIP encode/decode
- FujiBus header make/parse
- Interactive testing via PTY or USB

---

# 16. FAQ

### Q: Can I send a FujiBus packet without SLIP?  
Yes, with `serializeRaw()` / `fromRaw()` and explicit packet boundaries. Serial
compatibility callers can use `serialize()` / `fromSerialized()` with SLIP;
FujiBusTransport uses raw packets and delegates serial framing to SlipFramer.

### Q: Do devices interpret parameters or payload first?  
Devices may treat parameters as *commands* and payload as *data*.

### Q: Are device IDs globally assigned?  
Yes — see `WireDeviceId` in `protocol_ids.h`.

### Q: Can FujiBus packets be nested?  
No.

---

# 17. End of Document
