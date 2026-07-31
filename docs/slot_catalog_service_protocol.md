# Slot Catalog Service Protocol

`SlotCatalogService` (`WireDeviceId::SlotCatalogService`, `0xF2`) owns the
sparse user catalogue of 256 disk-image choices. It is a typed service:
clients address a one-byte index and never construct AppStore namespaces,
keys, or stored record formats.

Every payload begins with version `1`. Commands are:

| Command | ID |
|---|---:|
| `Get` | `0x01` |
| `Put` | `0x02` |
| `Delete` | `0x03` |
| `Range` | `0x04` |

Entry flags are bit 0 `valid`, bit 1 `read-only`, and bit 2 `URI truncated`.

## Get

Request: `u8 version, u8 index`.

Successful response:

```
u8 version
u8 flags
u8 index
u16 uriLen
u8[] canonicalUri
```

An empty index returns transport status `DeviceNotFound`.

## Put

Request:

```
u8 version
u8 index
u8 flags              // bit 1 may request read-only
u16 targetLen
u8[] target
```

FujiNet resolves a relative target against the current HostService
host/path. An already canonical URI remains canonical. The response is the
same entry shape as `Get`, containing the canonical URI actually stored.

## Delete

Request: `u8 version, u8 index`.

Response: `u8 version, u8 flags, u8 index`; flags bit 0 reports whether an
entry existed. No other indexes move or change.

## Range

Request:

```
u8 version
u8 lower
u8 upper
u8 cursor
u8 flags               // bit 0: URI tail, bit 1: formatted
u8 maxUriBytes
u16 maxPayloadBytes
```

Response:

```
u8 version
u8 flags               // bit 0: more, bit 1: formatted
u8 nextIndex
u8 presenceLen
u8 entryCount
u16 entriesLen
u8[] presence
u8[] entries
```

The bitmap covers every index in the inclusive range, with bit zero
representing `lower`. Binary entries are `u8 index, u8 flags, u8 uriLen,
u8[] uri`. Formatted responses contain `<index>: <uri>\n` lines. Responses
contain only complete entries; continue with `cursor=nextIndex` when `more` is
set.

The service maintains a rebuildable 32-byte occupancy bitmap so Range does not
probe all 256 backing records. See [Slot catalogue and active disk
mounts](slot_state.md) for persistence, caching, and DiskService interaction.
