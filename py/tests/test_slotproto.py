from __future__ import annotations

import unittest

from fujinet_tools import slotproto as sp


class TestSlotCatalogProto(unittest.TestCase):
    def test_service_commands_are_independent_of_file_service(self) -> None:
        self.assertEqual(sp.SLOT_CATALOG_DEVICE_ID, 0xF2)
        self.assertEqual(sp.CMD_GET, 0x01)
        self.assertEqual(sp.CMD_PUT, 0x02)
        self.assertEqual(sp.CMD_DELETE, 0x03)
        self.assertEqual(sp.CMD_RANGE, 0x04)

    def test_put_and_entry_round_trip_shape(self) -> None:
        request = sp.build_put_req(100, "chuck.ssd", sp.ENTRY_READ_ONLY)
        self.assertEqual(
            request,
            b"\x01\x64\x02\x09\x00chuck.ssd",
        )
        entry = sp.parse_entry(
            b"\x01\x03\x64\x13\x00host:/bbc/chuck.ssd"
        )
        self.assertEqual(entry.index, 100)
        self.assertEqual(entry.flags, sp.ENTRY_VALID | sp.ENTRY_READ_ONLY)
        self.assertEqual(entry.uri, "host:/bbc/chuck.ssd")


if __name__ == "__main__":
    unittest.main()
