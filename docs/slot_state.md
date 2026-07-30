# Slot catalogue and active disk mounts

FujiNet clients distinguish the user's persistent **slot catalogue** from the
small set of **active disk units** exposed to a host computer.

## Persistent catalogue

The catalogue is application state, not `fujinet.yaml` configuration.
`config-nio` stores sparse entries through File device AppStore commands:

- namespace: `config-nio`
- key: `slot-NNN`, where `NNN` is the zero-padded index `000` through `255`
- value: `u8 version` (`1`), `u8 flags` (bit 0 is read-only), followed by the
  URI bytes without a terminator

An absent key is an empty slot. Deleting a key leaves that index empty and does
not move or renumber any other entry.

AppStore remains the source of truth. `SlotCatalog` is a thin, read-only view of
these keys for clients which need to browse them efficiently. On the first
catalogue request after startup it lists the namespace once and builds a
32-byte occupancy bitmap. AppStore writes and deletes of valid `slot-NNN` keys
update that bitmap while it is resident. The bitmap is transient, may be
discarded and rebuilt at any time, and is never a second persistent index.

The File device `SlotCatalogRange` command returns the presence bitmap for an
inclusive index range plus the populated entries that fit in the requested
payload. This lets a configuration UI fetch a complete display page in one
request, and lets an informational CLI enumerate only populated slots. See
[FileDevice Binary Protocol](file_device_protocol.md#slotcatalogrange-0x25).

The BBC `config-nio` client requests eight indexes at a time and retains its
current and previous display pages, so normal back-and-forth paging is served
from BBC memory. `FSLOTS` requests formatted batches and prints only populated
entries. Neither client enumerates all 256 AppStore keys at startup.

The value format belongs to the slot catalogue contract. AppStore itself still
supplies generic namespaced byte storage and does not interpret arbitrary
application data.

## Resolving a catalogue entry

A client mounting catalogue slot 69 reads `config-nio/slot-069`, then sends the
resolved URI to the Disk device. The catalogue number is not a Disk unit.

Slot records contain canonical URIs. A target-side command such as BBC
`*FIN 69 elite.ssd` first asks HostService to resolve the relative name against
the current host/path, then stores the resulting full URI. A configuration UI
which selected the same file while browsing stores that same canonical form.
Changing the current host later therefore does not silently change an existing
catalogue entry.

Disk units are the bounded active drives required by the host adapter. For
example, mounting catalogue slot 69 into BBC drive 0 resolves slot 69 once and
installs its URI in active Disk unit 1 (the protocol uses one-based units).
Sector I/O subsequently addresses active unit 1.

## Persistent drive mappings

The catalogue and the active Disk device remain separate, but configuration
clients share the desired catalogue-to-drive assignments in the AppStore key
`config-nio/mappings`. Its fixed binary v1 value is:

```
u8 version = 1
repeat 8 times:
    u8 flags       // bit 0 valid, bit 1 read-only
    u8 catalogSlot // 0..255
```

The fixed pairs let a small client update one drive without enumerating the
catalogue or rewriting variable-length text. BBC `*FMOUNT` updates the selected
pair only after the live lazy mount succeeds; `*FUMOUNT` clears it only after
the live unmount succeeds. `config-nio` reads and writes the same value, so a
mapping made at the CLI is visible in the UI and vice versa.

This mapping value describes user intent and retains the catalogue index needed
for display and later remounting. DiskService separately persists its bounded
runtime mounts as canonical URIs so sector I/O and restart recovery do not
depend on reading the application catalogue.

## Lazy mounting

Disk `Mount` request flag bit 1 requests a lazy mount. FujiNet validates and
records the URI for the active unit but defers opening and probing the image
until the host first accesses that drive. Runtime mount state is persisted so
active drives can be restored after restart.

Thus hundreds of catalogue choices do not consume `DiskService` slots or image
objects. Apart from the fixed 32-byte occupancy bitmap and bounded response
buffers, memory use is governed by the active-drive ceiling; persistent
catalogue storage grows only for populated AppStore keys.

The former `mounts` list in `fujinet.yaml` is not loaded as the user catalogue.
Early-development installations may delete that obsolete configuration
manually; no migration is performed.
