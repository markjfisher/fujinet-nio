from __future__ import annotations

import unittest

from fujinet_tools import fileproto as fp


def u16(x: int) -> bytes:
    return bytes([x & 0xFF, (x >> 8) & 0xFF])


def u32(x: int) -> bytes:
    return bytes([(x >> shift) & 0xFF for shift in (0, 8, 16, 24)])


def u64(x: int) -> bytes:
    return bytes([(x >> (8 * i)) & 0xFF for i in range(8)])


class TestAppStoreFileProto(unittest.TestCase):
    def test_build_write_request(self) -> None:
        req = fp.build_appstore_write_req("config-ng", "colour.preference", 6, b"blue")
        expected = (
            bytes([fp.FILEPROTO_VERSION])
            + u16(9)
            + b"config-ng"
            + u16(17)
            + b"colour.preference"
            + u32(6)
            + u16(4)
            + b"blue"
        )
        self.assertEqual(req, expected)
        self.assertEqual(fp.CMD_APPSTORE_WRITE, 0x22)

    def test_build_list_request_uses_empty_key(self) -> None:
        req = fp.build_appstore_list_req("prefs", 3, 512)
        expected = (
            bytes([fp.FILEPROTO_VERSION])
            + u16(5)
            + b"prefs"
            + u16(0)
            + u16(3)
            + u16(512)
        )
        self.assertEqual(req, expected)
        self.assertEqual(fp.CMD_APPSTORE_LIST, 0x24)

    def test_parse_stat_response(self) -> None:
        resp = (
            bytes([fp.FILEPROTO_VERSION, 0x01])
            + u16(0)
            + u64(1234)
            + u64(1710000000)
        )
        parsed = fp.parse_appstore_stat_resp(resp)
        self.assertTrue(parsed.exists)
        self.assertEqual(parsed.size_bytes, 1234)
        self.assertEqual(parsed.mtime_unix, 1710000000)

    def test_parse_read_response(self) -> None:
        resp = (
            bytes([fp.FILEPROTO_VERSION, 0x03])
            + u16(0)
            + u32(4)
            + u16(5)
            + b"world"
        )
        parsed = fp.parse_appstore_read_resp(resp)
        self.assertTrue(parsed.exists)
        self.assertTrue(parsed.eof)
        self.assertEqual(parsed.offset, 4)
        self.assertEqual(parsed.data, b"world")

    def test_parse_list_response(self) -> None:
        keys = u16(5) + b"alpha" + u16(4) + b"zeta"
        resp = (
            bytes([fp.FILEPROTO_VERSION, 0x01])
            + u16(0)
            + u16(0)
            + u16(2)
            + u16(len(keys))
            + keys
        )
        parsed = fp.parse_appstore_list_resp(resp)
        self.assertTrue(parsed.more)
        self.assertEqual(parsed.start_index, 0)
        self.assertEqual(parsed.key_count, 2)
        self.assertEqual(parsed.keys, ["alpha", "zeta"])

    def test_empty_key_is_rejected_for_key_commands(self) -> None:
        with self.assertRaises(ValueError):
            fp.build_appstore_stat_req("prefs", "")


if __name__ == "__main__":
    unittest.main()
