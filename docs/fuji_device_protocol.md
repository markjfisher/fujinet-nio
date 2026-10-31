# FujiDevice Binary Protocol

FujiDevice (`WireDeviceId::FujiNet`, currently `0x70`) provides small,
instance-level control operations. Persistent slot choices are owned by
SlotCatalogService, while active disk mappings are owned by DiskDevice.

See:

- [Slot catalogue and active disk mounts](slot_state.md)
- [FileDevice protocol](file_device_protocol.md)
- [Slot Catalog Service protocol](slot_catalog_service_protocol.md)
- [DiskDevice protocol](disk_device_protocol.md)

## Commands

| Command | ID | Purpose |
|--------:|---:|---------|
| `GetInfo` | `0x01` | Report the firmware version and build profile |
| `GetHsio` | `0x02` | Read the Atari SIO advertised index |
| `SetHsio` | `0x03` | Set the Atari SIO advertised index |
| `Reset`   | `0xFF` | Request a FujiNet reset/restart |
| `GetSsid` | `0xFE` | Reserved; not currently implemented |

## GetInfo (`0x01`)

Describes this FujiNet. It needs no network or other device, so it answers
quickly on any board.

Request payload: version `u8`, the highest version the client understands
(currently `1`). An empty payload or version `0` is `InvalidRequest`; bytes
after the version are ignored.

Response payload:

| Field | Type | Notes |
| --- | --- | --- |
| version | `u8` | the lower of the requested version and the firmware's; `1` today |
| firmware version | `u8` length + text | at most 32 bytes, e.g. `0.1.1` |
| build profile | `u8` length + text | at most 64 bytes, e.g. `S3 + FujiBus over GPIO (e.g. RS232)` |

This command follows the
[extending-commands rules](protocol_reference.md#extending-commands): a
version-1 reply always has exactly these fields, new fields come with a new
version, and a client asking for a newer version than the firmware knows gets
the newest reply the firmware can give, marked with its version.
Firmware without this command answers `Unsupported`. The station MAC address
comes from the Wi-Fi service's `GET_ADAPTER_INFO`
([Wi-Fi service protocol](wifi_service_protocol.md)). On the console, `core.info`
shows the same version and profile; from a host, `./scripts/fujinet --port <port>
fuji info` asks for it.

## GetHsio (`0x02`) and SetHsio (`0x03`)

Get requests contain the highest supported version (currently `1`); Set requests
contain version and index. The index is `0`–`9` or `255` for off. Successful
responses contain version `1` and the advertised index. An unknown command or
an ingress without Atari SIO support returns `Unsupported`. Settings are
transient and the console's `sio.status` and `sio.hsio` show the same state.

Changing the advertised index leaves the baud of the current transaction
unchanged. Drivers that cache speeds must clear their entries before probing
again. Cached speeds of other devices on the shared bus can also need resetting;
the Atari U1MB helpers in fujinet-nio-lib clear device and disk slots explicitly.
The host sends a standard-speed `3F` probe and the FujiNet changes its UART
after the probe reply, or rejects the probe when disabled. The alternative
listener detects standard-speed requests while the current speed is high.
This does not persist the index, and physical hardware testing is required for
the U1MB's undocumented cache layout and transition timing.

## Reset (`0xFF`)

The request and successful response have no payload.

`IOResponse.status` is returned through the FujiBus status metadata:

- `Ok`: reset accepted.
- `Unsupported`: no reset handler is available or the command is unknown.

On an MCU the reset handler may restart the device before returning a response.

FujiDevice does not expose slot catalogue or disk mount commands. Clients use
SlotCatalogService for catalogue entries and DiskDevice `Mount`, `Unmount`, and
`ListMounts` for active disk units.
