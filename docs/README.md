# Documentation

The `docs/` directory contains two kinds of document. Decide which kind a new
document is before adding it, because they are maintained differently.

## Evergreen documents

Protocols, architecture, testing guidance, build guidance, and operational
references describe the current project. Edit these documents in place when
the implementation or contract changes. Git history preserves their earlier
forms.

Principal evergreen documents include:

| Document | Owns |
|---|---|
| `architecture.md` | NIO service and transport architecture |
| `protocol_reference.md` | FujiBus protocol reference |
| `disk_device_protocol.md` | DiskDevice wire contract |
| `driver_architecture.md` | Client driver, session, and channel architecture |
| `amiga-floppy-channel.md` | Amiga floppy/Pico channel trade study |
| `testing.md` | Test strategy and commands |
| `developer_onboarding.md` | Development environment and onboarding |

This table is an index of major ownership, not a complete list of every
protocol and service document.

## Point-in-time artifacts

Responses, implementation plans, investigations, and handoffs record what was
known or proposed at a particular moment. Do not continually rewrite them to
look current; that destroys their historical meaning.

When such a document is superseded:

1. Move durable facts into the evergreen document that owns the subject.
2. Move the artifact into `docs/archive/`.
3. Add an archive banner immediately below its title:

   > **Status: ARCHIVED — superseded YYYY-MM-DD.** Why it was retired and
   > where its durable information now lives.

4. If another repository links to its original path, leave a short pointer at
   that path so the external URL remains useful.

The rule is: rehome durable facts, then retire the snapshot.

## Status convention

Point-in-time plans that still contain open work remain outside the archive
and carry a status near the title:

- `Not started`
- `In progress (YYYY-MM-DD) — completed scope and remaining work`
- `Complete (YYYY-MM-DD) — current maintenance location`

A completed plan with no ongoing reference role belongs in the archive.

## Archive

[`archive/`](archive/) contains retired snapshots. They remain browsable as
historical project artifacts and must not be mistaken for current contracts or
implementation guidance.
