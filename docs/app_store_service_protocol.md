# AppStore Service Protocol

`AppStoreService` (`WireDeviceId::AppStoreService`, `0xF1`) exposes generic,
namespaced opaque byte storage. It is separate from FileDevice because keys
and namespaces are application state, not filesystem paths.

The backing files live below `/FujiNet/app-store/v1` on the default persistent
filesystem. That path and the individual filenames are implementation details;
clients use this service rather than FileDevice or constructed paths.

All multi-byte values are little-endian and every payload begins with version
`1`. Commands are:

| Command | ID |
|---|---:|
| `Stat` | `0x01` |
| `Read` | `0x02` |
| `Write` | `0x03` |
| `Delete` | `0x04` |
| `List` | `0x05` |

The common request prefix is:

```
u8 version
u16 namespaceLen
u8[] namespace
u16 keyLen
u8[] key
```

Namespaces and keys are UTF-8 byte strings of at most 255 bytes. Namespace is
non-empty. Key is non-empty except for `List`.

## Stat

Request is the common prefix. Response:

```
u8 version
u8 flags              // bit 0: exists
u16 reserved
u64 sizeBytes
u64 modifiedUnixTime
```

## Read

Request appends `u32 offset, u16 maxBytes`. Response:

```
u8 version
u8 flags              // bit 0: EOF, bit 1: exists
u16 reserved
u32 offset
u16 dataLen
u8[] data
```

## Write

Request appends `u32 offset, u16 dataLen, u8[] data`. Response:

```
u8 version
u8 flags
u16 reserved
u32 offset
u16 writtenLen
```

Writes are offset writes. Callers replacing a variable-length value must
delete it first if a shorter write must truncate the old value.

## Delete

Request is the common prefix. Response is
`u8 version, u8 flags, u16 reserved`, with flags bit 0 set when an existing key
was removed.

## List

The common prefix uses an empty key, followed by `u16 startIndex,
u16 maxPayloadBytes`. Response:

```
u8 version
u8 flags              // bit 0: more
u16 reserved
u16 startIndex
u16 keyCount
u16 keysLen
repeat keyCount:
    u16 keyLen
    u8[] key
```

AppStore does not interpret reserved application schemas such as hosts,
mappings, or slots. Their owning services/applications define those contracts.
