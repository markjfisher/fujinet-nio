# **ModemDevice v1 Protocol**

This document defines the binary protocol for **ModemDevice** in FujiNet-NIO.

- **Wire device ID**: `WireDeviceId::ModemService` (`0xFB`)
- **Command space**: 8-bit (low byte of `IORequest.command`)
- **Endianness**: little-endian for multi-byte fields
- **Streaming model**: sequential offsets (like `NetworkDevice`)

ModemDevice is designed to replace the old `fujinet-firmware` modem implementations
with a single clean device that is transport-agnostic and platform-injected.

---

## **1. Overview**

ModemDevice exposes a **byte stream** between the host and a network backend:

- Host → Modem: `Write` bytes
- Modem → Host: `Read` bytes

In **command mode**, ModemDevice interprets classic Hayes-style `AT` commands and
emits result codes (`OK`, `ERROR`, `CONNECT`, `RING`, `NO CARRIER`).

In **connected mode**, bytes are forwarded to/from a TCP socket. Optionally,
the device performs minimal Telnet negotiation and filtering.

---

## **2. Versioning**

All request payloads begin with:

- `u8 version` — currently `1`

If `version != 1`, the device returns `StatusCode::InvalidRequest`.

---

## **3. Commands**

Command IDs are defined in `include/fujinet/io/devices/modem_commands.h`.

### **3.1 Write (0x01)**

Host writes bytes to the modem stream.

**Request payload**

- `u8  version`
- `u32 offset` — must match the next expected host write offset
- `u16 len`
- `u8[len] data`

**Response payload**

- `u8  version`
- `u8  flags` (reserved, currently 0)
- `u16 reserved` (0)
- `u32 offset` (echo)
- `u16 written` (== len on success)

**Errors**

- `InvalidRequest` on offset mismatch, malformed payload, or wrong version.

---

### **3.2 Read (0x02)**

Host reads bytes emitted by the modem.

**Request payload**

- `u8  version`
- `u32 offset` — must match the next expected host read offset
- `u16 maxBytes`

**Response payload**

- `u8  version`
- `u8  flags` (reserved, currently 0)
- `u16 reserved` (0)
- `u32 offset` (echo)
- `u16 len`
- `u8[len] data`

`len` may be 0 if no modem output is currently available.

---

### **3.3 Status (0x03)**

Query modem state bits and cursors.

**Request payload**

- `u8 version`

**Response payload**

- `u8  version`
- `u8  flags`
- `u16 reserved` (0)
- `u16 listenPort` (0 if not listening)
- `u32 hostRxAvail` (bytes available for `Read`)
- `u32 hostTxCursor` (next required host `Write` offset)
- `u32 netRxCursor` (network read cursor)
- `u32 netTxCursor` (network write cursor)

**Flag bits**

- bit0: command mode
- bit1: connected
- bit2: listening
- bit3: pending inbound connection present
- bit4: auto-answer enabled
- bit5: telnet mode enabled
- bit6: command echo enabled
- bit7: numeric result codes enabled

---

### **3.4 Control (0x04)**

Out-of-band control operations (optional; the AT command set can do most of these).

**Request payload**

- `u8 version`
- `u8 op`
- op-specific data

**Response payload**

- `u8  version`
- `u8  flags` (reserved, currently 0)
- `u16 reserved` (0)

**Supported ops**

- `0x01` Hangup
- `0x02` Dial: `lp_u16 string host[:port]` (default port 23)
- `0x03` Listen: `u16 port`
- `0x04` Unlisten
- `0x05` Answer pending inbound
- `0x06` SetAutoAnswer: `u8 enable`
- `0x07` SetTelnet: `u8 enable`
- `0x08` SetEcho: `u8 enable`
- `0x09` SetNumericResult: `u8 enable`
- `0x0A` Reset (returns to default idle state)

---

## **4. AT Command Set (command mode)**

Command mode accepts ASCII commands terminated by `CR` or `LF`.

Currently implemented:

- `AT`
- `ATDT<host[:port]>` (also `ATDP`, `ATDI`)
- `ATH` / `+++ATH`
- `ATA`
- `ATPORT<port>`
- `ATS0=0` / `ATS0=1` (auto-answer off/on)
- `ATNET0` / `ATNET1` (telnet off/on)
- `ATE0` / `ATE1` (echo off/on)
- `ATV0` / `ATV1` (numeric result codes on/off)
- `AT+TERM=<VT52|VT100|ANSI|DUMB>`
- `ATO`

Result codes:

- Text: `OK`, `ERROR`, `RING`, `CONNECT 9600`, `NO CARRIER`
- Numeric (when `ATV0`): `0`, `4`, `2`, `1`, `3`

### Baud rate support

ModemDevice tracks an informational `modemBaud` used for:

- the `CONNECT <baud>` message (text mode)
- numeric CONNECT result codes (numeric mode)

Supported baud rates (matching legacy FujiNet mapping):

- 300, 600, 1200, 1800, 2400, 4800, 9600, 19200

Numeric CONNECT result code mapping:

- 300 → 1
- 1200 → 5
- 2400 → 10
- 4800 → 18
- 9600 → 13
- 19200 → 85

AT commands:

- `ATB300` / `ATB600` / ... / `ATB19200` set modem baud (if not locked)
- `AT+BAUDLOCK=0|1` controls baud locking

Control ops:

- `0x0B` SetBaud: `u32 baud`
- `0x0C` BaudLock: `u8 enable`


