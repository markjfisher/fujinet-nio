# Slot catalogue and active disk mounts

FujiNet clients distinguish the user's persistent **slot catalogue** from the
small set of **active disk units** exposed to a host computer.

## Persistent catalogue

The catalogue is application state, not `fujinet.yaml` configuration. Clients
manage it through the typed Slot Catalog service (`0xF2`):

- `Get`, `Put`, and `Delete` address one one-byte index
- `Range` retrieves sparse pages or formatted populated entries
- `Put` resolves a relative target against current HostService state and stores
  the canonical URI

An absent entry is an empty slot. Deleting an entry leaves that index empty and
does not move or renumber any other entry.

Internally, `SlotCatalog` persists sparse records on top of AppStore. The
current implementation uses namespace `config-nio` and keys `slot-NNN`, but
that schema is private to the service and clients must not depend on it. On the
first catalogue operation after startup, it lists the namespace once and
builds a 32-byte occupancy bitmap. Typed Put/Delete operations update the
bitmap while it is resident. The bitmap is transient, may be discarded and
rebuilt at any time, and is never a second persistent index.

The Slot Catalog `Range` command returns the presence bitmap for an
inclusive index range plus the populated entries that fit in the requested
payload. This lets a configuration UI fetch a complete display page in one
request, and lets an informational CLI enumerate only populated slots. See
[Slot Catalog Service Protocol](slot_catalog_service_protocol.md).

The BBC `config-nio` client requests eight indexes at a time and retains its
current and previous display pages, so normal back-and-forth paging is served
from BBC memory. `FSLOTS` requests formatted batches and prints only populated
entries. Neither client enumerates all 256 entries at startup.

AppStore itself remains generic namespaced byte storage. Only SlotCatalog
knows its backing keys and record encoding.

## Resolving a catalogue entry

A client mounting catalogue slot 69 uses Slot Catalog `Get`, then sends the
returned canonical URI to the Disk device. The catalogue number is not a Disk
unit.

Slot records contain canonical URIs. A target-side command such as BBC
`*FIN 69 elite.ssd` sends that target to Slot Catalog `Put`; the service
resolves it against the current HostService host/path before storing it. A
configuration UI uses the same operation. Changing the current host later
therefore does not silently change an existing catalogue entry.

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

## Runtime mounts in configuration clients

`DiskService` runtime state is authoritative for what is currently available to
the host. The Disk `ListMounts` command (`0x0D`) reports persisted mounts,
restored mounts, and pending lazy boot mounts as formatted `unit: mode URI`
entries. A client may combine this response with `config-nio/mappings` when it
draws its drive-assignment screen.

An active boot mount, including the initial lazy boot disk or a BBC `*FBOOT`
mount, does not acquire a synthetic catalogue slot and does not modify the
desired mapping table. It should be shown as a runtime/boot entry by the UI;
catalogue mappings continue to be shown with their `S<n>` index. This keeps
boot infrastructure state from colliding with a user's real slot 0.

## Lazy mounting

Disk `Mount` request flag bit 1 requests a lazy mount. FujiNet validates and
records the URI for the active unit but defers opening and probing the image
until the host first accesses that drive. Runtime mount state is persisted so
active drives can be restored after restart.

Thus hundreds of catalogue choices do not consume `DiskService` slots or image
objects. Apart from the fixed 32-byte occupancy bitmap and bounded response
buffers, memory use is governed by the active-drive ceiling; persistent
catalogue storage grows only for populated entries.

The former `mounts` list in `fujinet.yaml` is not loaded as the user catalogue.
Early-development installations may delete that obsolete configuration
manually; no migration is performed.
