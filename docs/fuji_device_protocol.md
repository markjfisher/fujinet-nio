# FujiDevice Binary Protocol

FujiDevice (`WireDeviceId::FujiNet`, currently `0x70`) provides small,
instance-level control operations. Persistent slot choices are application
state stored through FileDevice AppStore, while active disk mappings are owned
by DiskDevice.

See:

- [Slot catalogue and active disk mounts](slot_state.md)
- [FileDevice protocol](file_device_protocol.md)
- [DiskDevice protocol](disk_device_protocol.md)

## Commands

| Command | ID | Purpose |
|--------:|---:|---------|
| `Reset`   | `0xFF` | Request a FujiNet reset/restart |
| `GetSsid` | `0xFE` | Reserved; not currently implemented |

## Reset (`0xFF`)

The request and successful response have no payload.

`IOResponse.status` is returned through the FujiBus status metadata:

- `Ok`: reset accepted.
- `Unsupported`: no reset handler is available or the command is unknown.

On an MCU the reset handler may restart the device before returning a response.

FujiDevice does not expose slot catalogue or disk mount commands. Clients use
FileDevice AppStore for catalogue entries and DiskDevice `Mount`, `Unmount`,
and `ListMounts` for active disk units.
