# DiskDevice scalar test vectors

These vectors are the canonical byte-level examples for the scalar DiskDevice
operations used by the C client codec and the MS-DOS client helpers. They are
payloads after the FujiBus header; the wire device ID is `0xFC`.

All multi-byte values are little-endian. Slot `1` is the first DiskDevice
slot. The vectors deliberately use a small four-byte sector payload for the
write/read examples; production callers use the geometry returned by `Info` or
`Mount`.

## Mount request

`Mount(slot=1, readonly=true, type=Auto, sector_size_hint=0, URI=host:/images/test.adf)`:

```text
01 01 01 00 00 00 15 00
68 6f 73 74 3a 2f 69 6d 61 67 65 73 2f 74 65 73 74 2e 61 64 66
```

## ReadSector request

`ReadSector(slot=1, lba=1759, maxBytes=8)`:

```text
01 01 df 06 00 00 08 00
```

Response with four bytes of data:

```text
01 00 00 00 01 df 06 00 00 04 00 41 42 43 44
```

## WriteSector request

`WriteSector(slot=1, lba=2, data=01 02 03 04)`:

```text
01 01 02 00 00 00 04 00 01 02 03 04
```

## Info response

An inserted, changed, raw slot with 512-byte sectors and 1760 sectors:

```text
01 09 00 00 01 04 00 02 e0 06 00 00 00
```

The final `lastError` byte is optional for compatibility with the original
DiskDevice response shape used by existing BBC/MS-DOS clients. A 12-byte
response without it is valid and means `lastError=0`; a 13-byte response
includes the value.
