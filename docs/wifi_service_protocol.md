# Wi-Fi Management Service Protocol

The Wi-Fi management service is wire device `0xF3` and is registered when the
platform can expose the management endpoint. Hardware capabilities describe
which live operations are available; they do not by themselves hide the
endpoint on POSIX or emulator platforms.
Payload integers are little-endian. Every request and response starts with
protocol version `1`. Password bytes are accepted only by `SET_CONFIG`; they
are never present in any response.

## Commands

| Command | Value | Request |
| --- | ---: | --- |
| `GET_STATUS` | `0x01` | version only |
| `GET_CONFIG` | `0x02` | version only |
| `SET_CONFIG` | `0x03` | version, field flags, selected fields |
| `SCAN` | `0x04` | version, offset `u16`, limit `u8` |

`SET_CONFIG` field flags are `enabled=0x01`, `ssid=0x02`, `bssid=0x04`,
`password=0x08`, `persist=0x10`, and `reconnect=0x20`. Enabled is one byte;
strings are `u8` length followed by bytes. SSID is limited to 32 bytes,
password to 64 bytes, and BSSID is the canonical six-byte colon-separated
text form (`aa:bb:cc:dd:ee:ff`). Updates are validated before configuration
mutation. `persist` saves the complete configuration; `reconnect` connects or
disconnects after the update.

`GET_CONFIG` returns version, enabled, password-present, SSID, and configured
BSSID. `GET_STATUS` returns version, link state, configured-enabled,
current-BSSID-present, scan-supported, signed RSSI, current BSSID, capability
flags (`u16`), backend kind (`u8`), and four bounded IPv4 strings: address,
subnet, gateway, and DNS. The capability flags and backend kind are appended
after the four strings as a version-1 extension so older clients can parse the
original status fields. Link state values are
`0=disconnected`, `1=connecting`, `2=connected`, and `3=failed`.

Capability flags are config `0x0001`, status `0x0002`, connect `0x0004`,
disconnect `0x0008`, scan `0x0010`, BSSID selection `0x0020`, host-managed
`0x0040`, and simulated `0x0080`. Backend kinds are unavailable `0`, ESP32
`1`, POSIX host-managed `2`, and POSIX simulated `3`.

`SCAN` returns version, more flag, count, and records. Each record contains an
SSID, six BSSID bytes, signed RSSI, channel, and authentication mode. The
offset and limit make responses safe for 8-bit clients; implementations cap a
single response at 32 records.

Malformed payloads return `InvalidRequest`; unavailable live-link operations
return `NotReady`; scan failures return `IOError`; invalid BSSID text returns
`InvalidRequest`; unknown commands return `Unsupported`.

## POSIX Backends

The POSIX FujiNet process always registers this service. Set
`FN_POSIX_WIFI_BACKEND` to `simulated` (the deterministic default), `host`, or
`unavailable`. Host mode observes the selected host interface without taking
control of the host network; set `FN_POSIX_WIFI_INTERFACE` to select an
interface. Simulated mode provides three stable access points and a test IPv4
connection, making PTY and BBC emulator tests independent of host Wi-Fi.

## What Was Implemented

The implementation follows the original plan with these explicit decisions:

- `WifiService` remains separate from `NetworkDevice`, while configuration,
  persistence, reconnect checks, BSSID validation, and scan access are shared
  through `WifiController` by both the wire service and console diagnostics.
- Registration is based on service/backend availability rather than only
  `managesItsOwnLink`. This is intentional: POSIX PTY and BBC emulator runs
  must expose the same service as ESP32. POSIX capabilities identify whether
  the backend is host-managed, simulated, or unavailable.
- Scan offset `0` refreshes the service cache. Later offsets consume that cache,
  so paginated clients see one consistent scan without repeatedly starting a
  radio scan.
- Capability flags and backend kind are appended after the original version-1
  status fields. This is a wire-layout choice for forward parsing within the
  unreleased protocol, not compatibility with an external released client.
- POSIX host mode observes the host network and does not attempt to disconnect
  or reconfigure it. POSIX simulated mode is deterministic and is the default
  for emulator/PTY testing.
- BBC library calls use the existing `fn_bbc_device_call_raw` ABI. No new
  assembly wrapper was needed for these helpers because the existing ROM
  device-call ABI already carries arbitrary device and command payloads.
  The BBC implementation uses one 512-byte static scan response buffer because
  that ABI has no caller-supplied response scratch argument; this is the only
  persistent Wi-Fi library buffer and is required to preserve the public API.
- Authentication mode is represented as a compact numeric wire value. Console
  diagnostics print that value rather than duplicating platform-specific
  ESP32 authentication labels.
