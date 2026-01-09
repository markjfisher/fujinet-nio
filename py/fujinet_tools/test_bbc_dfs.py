# py/fujinet_tools/test_bbc_dfs.py
from __future__ import annotations

import unittest

from .bbc_dfs import parse_dfs_catalogue_090


class TestBbcDfs090BitPacking(unittest.TestCase):
    def test_single_entry_bit_packing(self) -> None:
        # Build a minimal 2-sector catalogue with exactly 1 file entry.
        s0 = bytearray(256)
        s1 = bytearray(256)

        # Header: file_count = sector1[5] / 8
        s1[5] = 8

        # Entry 0 filename+dir lives at sector0 offset 8
        s0[8:15] = b"HELLO  "  # 7 bytes (space padded)
        s0[15] = ord("$")      # directory, unlocked

        # Choose values that exercise the top-bit packing:
        # load/exec/len: 18-bit, start sector: 10-bit.
        load = 0x34567   # top2 = 3
        exec_ = 0x21234  # top2 = 2
        length = 0x1ABCD # top2 = 1
        start = 0x2AA    # top2 = 2

        off = 8
        s1[off + 0] = load & 0xFF
        s1[off + 1] = (load >> 8) & 0xFF
        s1[off + 2] = exec_ & 0xFF
        s1[off + 3] = (exec_ >> 8) & 0xFF
        s1[off + 4] = length & 0xFF
        s1[off + 5] = (length >> 8) & 0xFF

        b14 = 0
        b14 |= ((start >> 8) & 0x03) << 0   # b1..b0
        b14 |= ((load >> 16) & 0x03) << 2   # b3..b2
        b14 |= ((length >> 16) & 0x03) << 4 # b5..b4
        b14 |= ((exec_ >> 16) & 0x03) << 6  # b7..b6
        s1[off + 6] = b14
        s1[off + 7] = start & 0xFF

        desc, entries = parse_dfs_catalogue_090(sector0=bytes(s0), sector1=bytes(s1))
        self.assertEqual(desc.file_count, 1)
        self.assertEqual(len(entries), 1)

        e = entries[0]
        self.assertEqual(e.directory, "$")
        self.assertEqual(e.name, "HELLO")
        self.assertFalse(e.locked)
        self.assertEqual(e.load_addr, load)
        self.assertEqual(e.exec_addr, exec_)
        self.assertEqual(e.length, length)
        self.assertEqual(e.start_sector, start)


if __name__ == "__main__":
    unittest.main()


