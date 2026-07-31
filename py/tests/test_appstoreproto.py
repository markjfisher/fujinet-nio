from __future__ import annotations

import unittest

from fujinet_tools import appstoreproto as ap


def u16(x: int) -> bytes:
    return bytes([x & 0xFF, (x >> 8) & 0xFF])


def u32(x: int) -> bytes:
    return bytes([(x >> shift) & 0xFF for shift in (0, 8, 16, 24)])


def u64(x: int) -> bytes:
    return bytes([(x >> (8 * i)) & 0xFF for i in range(8)])


class TestAppStoreProto(unittest.TestCase):
    def test_build_write_request(self) -> None:
        req = ap.build_write_req("config-ng", "colour.preference", 6, b"blue")
        expected = (
            bytes([ap.APPSTORE_VERSION])
            + u16(9)
            + b"config-ng"
            + u16(17)
            + b"colour.preference"
            + u32(6)
            + u16(4)
            + b"blue"
        )
        self.assertEqual(req, expected)
        self.assertEqual(ap.APPSTORE_DEVICE_ID, 0xF1)
        self.assertEqual(ap.CMD_WRITE, 0x03)

    def test_build_list_request_uses_empty_key(self) -> None:
        req = ap.build_list_req("prefs", 3, 512)
        expected = (
            bytes([ap.APPSTORE_VERSION])
            + u16(5)
            + b"prefs"
            + u16(0)
            + u16(3)
            + u16(512)
        )
        self.assertEqual(req, expected)
        self.assertEqual(ap.CMD_LIST, 0x05)

    def test_parse_stat_response(self) -> None:
        resp = (
            bytes([ap.APPSTORE_VERSION, 0x01])
            + u16(0)
            + u64(1234)
            + u64(1710000000)
        )
        parsed = ap.parse_stat_resp(resp)
        self.assertTrue(parsed.exists)
        self.assertEqual(parsed.size_bytes, 1234)
        self.assertEqual(parsed.mtime_unix, 1710000000)

    def test_parse_read_response(self) -> None:
        resp = (
            bytes([ap.APPSTORE_VERSION, 0x03])
            + u16(0)
            + u32(4)
            + u16(5)
            + b"world"
        )
        parsed = ap.parse_read_resp(resp)
        self.assertTrue(parsed.exists)
        self.assertTrue(parsed.eof)
        self.assertEqual(parsed.offset, 4)
        self.assertEqual(parsed.data, b"world")

    def test_parse_list_response(self) -> None:
        keys = u16(5) + b"alpha" + u16(4) + b"zeta"
        resp = (
            bytes([ap.APPSTORE_VERSION, 0x01])
            + u16(0)
            + u16(0)
            + u16(2)
            + u16(len(keys))
            + keys
        )
        parsed = ap.parse_list_resp(resp)
        self.assertTrue(parsed.more)
        self.assertEqual(parsed.start_index, 0)
        self.assertEqual(parsed.key_count, 2)
        self.assertEqual(parsed.keys, ["alpha", "zeta"])

    def test_empty_key_is_rejected_for_key_commands(self) -> None:
        with self.assertRaises(ValueError):
            ap.build_stat_req("prefs", "")


if __name__ == "__main__":
    unittest.main()
