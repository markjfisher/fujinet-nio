# py/fujinet_tools/bbc_dfs.py
from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional


@dataclass
class DfsDiskDescriptor:
    title: str
    cycle_bcd: int
    file_count: int
    boot_option: int
    disc_sectors: int


@dataclass
class DfsFileEntry:
    directory: str
    name: str
    locked: bool
    load_addr: int
    exec_addr: int
    length: int
    start_sector: int

    @property
    def full_name(self) -> str:
        # DFS convention: D.NAME where D is a single character directory.
        d = self.directory if self.directory else "$"
        return f"{d}.{self.name}"


def _decode_title(sector0: bytes, sector1: bytes) -> str:
    # Acorn DFS disc descriptor title:
    # - sector0[0..7] + sector1[0..3] (12 chars), padded with NULs (DFS 0.90) or spaces (later).
    t = (sector0[0:8] + sector1[0:4]).decode("latin-1", "replace")
    t = t.rstrip("\x00 ").strip()
    if t:
        return t

    # Compatibility: some older/third-party tools (and our early blank creator) put an 8-char title
    # at sector1[0..7]. Fall back if canonical title is empty.
    t2 = sector1[0:8].decode("latin-1", "replace").rstrip("\x00 ").strip()
    return t2


def _bcd_to_int(x: int) -> int:
    # Each nibble 0..9, represent decimal digits (00..99).
    return ((x >> 4) & 0x0F) * 10 + (x & 0x0F)


def parse_dfs_catalogue_090(*, sector0: bytes, sector1: bytes) -> tuple[DfsDiskDescriptor, List[DfsFileEntry]]:
    """
    Parse the standard Acorn DFS catalogue (2 sectors) for DFS 0.90 style discs.

    Reference (disc descriptor + file descriptor field packing):
      https://beebwiki.mdfs.net/Acorn_DFS_disc_format
    """
    if len(sector0) != 256 or len(sector1) != 256:
        raise ValueError("DFS catalogue sectors must be exactly 256 bytes each")

    title = _decode_title(sector0, sector1)

    cycle_raw = sector1[4]
    cycle = _bcd_to_int(cycle_raw)

    file_off = sector1[5]
    file_count = file_off // 8
    if file_off % 8 != 0:
        # Non-standard but we can still parse the floor() number of entries.
        file_count = file_off // 8
    if file_count > 31:
        file_count = 31

    # sector1[6]:
    # - bits 0..1: disc size high bits (10-bit total)
    # - bits 4..5: boot option
    boot_option = (sector1[6] >> 4) & 0x03
    disc_sectors = sector1[7] | ((sector1[6] & 0x03) << 8)

    desc = DfsDiskDescriptor(
        title=title,
        cycle_bcd=cycle,
        file_count=file_count,
        boot_option=boot_option,
        disc_sectors=disc_sectors,
    )

    entries: List[DfsFileEntry] = []
    for i in range(file_count):
        n0 = 8 + i * 8
        n1 = 8 + i * 8
        if n0 + 8 > 256 or n1 + 8 > 256:
            break

        raw_name = sector0[n0 : n0 + 7]
        dir_attr = sector0[n0 + 7]

        name = raw_name.decode("latin-1", "replace").rstrip(" ").rstrip("\x00")
        name = name.strip()
        if not name:
            # Empty entry - ignore.
            continue

        directory = chr(dir_attr & 0x7F)
        if directory == "\x00":
            directory = "$"

        locked = bool(dir_attr & 0x80)

        # File descriptor fields (sector1 bytes 8..15 per entry):
        # load: bytes 8-9 + top2 bits in bits 2-3 of byte 14
        # exec: bytes 10-11 + top2 bits in bits 6-7 of byte 14
        # len : bytes 12-13 + top2 bits in bits 4-5 of byte 14
        # start sector: byte 15 + top2 bits in bits 0-1 of byte 14
        b8 = sector1[n1 + 0]
        b9 = sector1[n1 + 1]
        b10 = sector1[n1 + 2]
        b11 = sector1[n1 + 3]
        b12 = sector1[n1 + 4]
        b13 = sector1[n1 + 5]
        b14 = sector1[n1 + 6]
        b15 = sector1[n1 + 7]

        load_addr = b8 | (b9 << 8) | (((b14 >> 2) & 0x03) << 16)
        exec_addr = b10 | (b11 << 8) | (((b14 >> 6) & 0x03) << 16)
        length = b12 | (b13 << 8) | (((b14 >> 4) & 0x03) << 16)
        start_sector = b15 | ((b14 & 0x03) << 8)

        entries.append(
            DfsFileEntry(
                directory=directory,
                name=name,
                locked=locked,
                load_addr=load_addr,
                exec_addr=exec_addr,
                length=length,
                start_sector=start_sector,
            )
        )

    return desc, entries


def find_entry(entries: List[DfsFileEntry], name: str) -> Optional[DfsFileEntry]:
    # Accept "D.NAME" or "NAME" (defaults to '$' directory).
    s = (name or "").strip()
    if not s:
        return None
    if "." in s:
        d, n = s.split(".", 1)
        d = (d or "$").strip()[:1]
        n = n.strip()
    else:
        d, n = "$", s

    for e in entries:
        if e.directory == d and e.name == n:
            return e
    return None


