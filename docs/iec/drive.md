# IEC drive

Drives 8-15 are `IecDriveFrontend`s, one per `DiskService` slot: device 8 is slot 0.
Drive 8 always answers, as a powered drive does, so `LOAD` reports a missing disk and
not a missing drive. The others answer once something is mounted on them. A reset
closes every channel and leaves the disk in the drive.

Behaviour follows a stock 1541 where a program can see it. The status text is the
1541's own, leading spaces included.

## What works

| | |
|---|---|
| Media | D64: 35, 40 or 42 tracks. Mounting anything else reads as `74,DRIVE NOT READY` |
| `LOAD"name",8` | Channel 0, PRG only. EOI goes with the last byte |
| `LOAD"*",8` | The first PRG on the disk. `0:*` is an ordinary pattern: the first file of any type, then type checked |
| `LOAD"$",8` | The 1541's listing, 32 bytes per line. `$0`, `$:PAT*`, `$0:PAT*` and `$:*=P` all work |
| Names | `[@][drive:]name[,type][,mode]`; `*` and `?`; a shifted space ends a name |
| Data channels 2-14 | Sequential reads of PRG, SEQ and USR files, any number open at once. `,M` reads a file never closed. `$` is track 18 as a file |
| Channel 15 | Status, with its position kept across UNTALK; commands run at UNLISTEN |
| `I` | `00, OK` |
| `UI`, `UJ`, `U9`, `U:` | Reset; status 73 |
| `CLOSE 15` | Closes every channel |

The listing's load address is `$0801` rather than a 1541's `$0401`, so `LOAD"$",8,1`
does not write over the screen.

## What does not, yet

| | |
|---|---|
| Writing | SAVE, write modes and `S`, `N`, `V`, `C`, `R` answer `26, WRITE PROTECT ON`. A refused write takes no bytes |
| REL files | Answer `64, FILE TYPE MISMATCH` |
| Direct access (`#`, `U1`, `B-R`) | `#` answers `70,NO CHANNEL`; the commands answer `31,SYNTAX ERROR` |
| Drive memory (`M-R`, `M-W`, `M-E`) | `31,SYNTAX ERROR`. Drive code cannot run here, so a fast loader fails at once |
| D71, D81, loose PRG files | Not mounted as drive media |

## Status messages

| Code | Text | When |
|---|---|---|
| 00 | ` OK` | |
| 20 | `READ ERROR` | A sector could not be read |
| 26 | ` WRITE PROTECT ON` | Any write |
| 31 | `SYNTAX ERROR` | An unknown command |
| 34 | `SYNTAX ERROR` | No file name |
| 60 | ` WRITE FILE OPEN` | Reading a file never closed, without `,M` |
| 61 | ` FILE NOT OPEN` | Reading a channel never opened |
| 62 | ` FILE NOT FOUND` | No such name |
| 64 | ` FILE TYPE MISMATCH` | The wrong type for the channel or the name |
| 66 | `ILLEGAL TRACK OR SECTOR` | A chain leaves the disk or loops |
| 70 | `NO CHANNEL` | Direct access |
| 73 | `FUJINET NIO DOS` | Power on and reset |
| 74 | `DRIVE NOT READY` | No disk, or not a D64 |

## Damaged disks

Sector chains are bounded: a directory stops at the first sector it has already read,
a file at as many sectors as the disk has. A damaged directory still lists as far as it
goes, with the status saying why it stopped. A file cut short ends with EOI on the last
good byte, and status 66 or 20.
