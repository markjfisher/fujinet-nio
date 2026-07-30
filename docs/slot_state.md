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
not move or renumber any other entry. Applications fetch only the exact indexes
needed for a page or direct lookup; a 256-entry in-memory table is unnecessary.

The format belongs to `config-nio`. AppStore supplies namespaced byte storage
and deliberately does not interpret slots, URIs, or mappings.

## Resolving a catalogue entry

A client mounting catalogue slot 69 reads `config-nio/slot-069`, then sends the
resolved URI to the Disk device. The catalogue number is not a Disk unit.

Disk units are the bounded active drives required by the host adapter. For
example, mounting catalogue slot 69 into BBC drive 0 resolves slot 69 once and
installs its URI in active Disk unit 1 (the protocol uses one-based units).
Sector I/O subsequently addresses active unit 1.

## Lazy mounting

Disk `Mount` request flag bit 1 requests a lazy mount. FujiNet validates and
records the URI for the active unit but defers opening and probing the image
until the host first accesses that drive. Runtime mount state is persisted so
active drives can be restored after restart.

Thus hundreds of catalogue choices do not consume `DiskService` slots or image
objects. Memory use is bounded primarily by the active-drive ceiling; catalogue
storage grows only for populated AppStore keys.

The former `mounts` list in `fujinet.yaml` is not loaded as the user catalogue.
Early-development installations may delete that obsolete configuration
manually; no migration is performed.
