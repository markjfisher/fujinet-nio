# Legacy Network Device Adapter

## Problem Statement

Legacy firmware clients (old fujinet-firmware) communicate with network devices using:
- **Device IDs**: `0x71-0x78` (8 network devices)
- **Commands**: `'O'` (OPEN), `'C'` (CLOSE), `'R'` (READ), `'W'` (WRITE), `'S'` (STATUS)
- **Payload Format**: URL strings in data frames
- **No Handles**: Each device ID manages one connection

New fujinet-nio uses:
- **Device ID**: `0xFD` (`NetworkService`)
- **Commands**: `0x01` (Open), `0x04` (Close), `0x02` (Read), `0x03` (Write), `0x05` (Info)
- **Payload Format**: Binary protocol with length-prefixed strings
- **Handles**: Returned from OPEN, used in subsequent operations

## Solution: Legacy Network Adapter

Create a `LegacyNetworkAdapter` that:
1. Implements `IRequestHandler` interface
2. Intercepts requests for device IDs `0x71-0x78`
3. Maps legacy device ID → NetworkDevice handle
4. Converts legacy commands to new protocol format
5. Converts legacy payloads to new protocol format
6. Routes converted requests to `NetworkService` (`0xFD`)

## Implementation Location

Use `RoutingManager::setOverrideHandler()` to install the adapter as an override handler. The adapter will:
- Check if `request.deviceId` is in range `0x71-0x78`
- If yes, convert and forward to `NetworkService`
- If no, forward to `IODeviceManager` (normal routing)

## Handle Mapping

Each legacy device ID (`0x71-0x78`) can have one active handle. The mapping is:
- `legacyDeviceId` (`0x71-0x78`) → `handle` (returned from NetworkService OPEN)

When a legacy client opens device `0x71`:
1. Convert `'O'` command + URL payload → `NetworkCommand::Open` + binary payload
2. Forward to `NetworkService` (`0xFD`)
3. Store returned handle for device `0x71`
4. Return success (legacy clients don't see handles)

When a legacy client reads/writes/closes device `0x71`:
1. Look up handle for device `0x71`
2. Convert command + legacy payload → new protocol format
3. Forward to `NetworkService` with the handle
4. Convert response back to legacy format

## Command Conversion

| Legacy | New Protocol | Notes |
|--------|--------------|-------|
| `'O'` (0x4F) | `NetworkCommand::Open` (0x01) | Convert URL string → binary protocol |
| `'C'` (0x43) | `NetworkCommand::Close` (0x04) | Use stored handle |
| `'R'` (0x52) | `NetworkCommand::Read` (0x02) | Use stored handle, aux1/aux2 = byte count |
| `'W'` (0x57) | `NetworkCommand::Write` (0x03) | Use stored handle, payload = data |
| `'S'` (0x53) | `NetworkCommand::Info` (0x05) | Use stored handle, return status |

## Payload Conversion

### Legacy OPEN (`'O'`)
- **Input**: URL string in data frame (e.g., `"N:http://example.com/"`)
- **Output**: Binary protocol format:
  ```
  u8 version (1)
  u8 method (1 = GET, from aux1)
  u8 flags (from aux2)
  // For legacy POST/PUT: adapter also sets NetworkDevice flag bit2 (body_unknown_len)
  u16 urlLen
  u8[] url (without "N:" prefix)
  u16 headerCount (0)
  u32 bodyLenHint (0)
  u16 respHeaderCount (0)
  ```

### Legacy READ (`'R'`)
- **Input**: aux1/aux2 = byte count (little-endian 16-bit)
- **Output**: Binary protocol format:
  ```
  u16 handle (from mapping)
  u32 offset (adapter-maintained sequential read cursor)
  u16 maxBytes (from aux1/aux2)
  ```

### Legacy WRITE (`'W'`)
- **Input**: Data frame with bytes to write
- **Output**: Binary protocol format:
  ```
  u16 handle (from mapping)
  u32 offset (adapter-maintained sequential write cursor)
  u16 dataLen
  u8[] data
  ```

### Legacy CLOSE (`'C'`)
- **Input**: None
- **Output**: Binary protocol format:
  ```
  u16 handle (from mapping)
  ```

### Legacy STATUS (`'S'`)
- **Input**: None
- **Output**: Binary protocol format:
  ```
  u16 handle (from mapping)
  ```
- **Response Conversion**: Convert binary status response → legacy status format

## Response Conversion

Legacy clients expect:
- **OPEN**: `COMPLETE` (no payload, or error code)
- **READ**: `COMPLETE` + data frame
- **WRITE**: `COMPLETE` (no payload)
- **CLOSE**: `COMPLETE` (no payload)
- **STATUS**: `COMPLETE` + status bytes

New protocol returns:
- **Open**: Handle in payload
- **Read**: Data in payload
- **Write**: Bytes written count
- **Close**: Success/error
- **Info**: Status metadata

The adapter must convert new protocol responses back to legacy format.

## Implementation Files

- `include/fujinet/io/legacy/legacy_network_adapter.h`
- `src/lib/legacy_network_adapter.cpp`

## Integration

In `bootstrap.cpp` or `main_*.cpp`:
```cpp
// After NetworkDevice is registered
auto adapter = std::make_unique<LegacyNetworkAdapter>(core.deviceManager());
core.routingManager().setOverrideHandler(adapter.get());
// Keep adapter alive
```

## Status

**IMPLEMENTED** (routing-layer override).

For legacy POST/PUT where the body length is not known at OPEN time, the adapter:

- Opens NetworkDevice with `bodyLenHint==0` and Open flag `body_unknown_len=1` (bit2)
- Streams body via `Write()` calls
- Commits the body by issuing a zero-length `Write()` at the current write offset on the first legacy `STATUS` or `READ` (matching typical legacy behavior)
