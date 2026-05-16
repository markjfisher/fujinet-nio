from __future__ import annotations

import unittest

from .fujibus import (
    build_fuji_packet,
    build_fuji_packet_decoded,
    build_fuji_response_wire,
    parse_fuji_packet,
    slip_decode,
)


class TestFujiBusPacketBuild(unittest.TestCase):
    def test_no_params_roundtrip(self) -> None:
        inner = bytes([0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01])
        wire = build_fuji_packet(0xFC, 0x03, inner)
        pkt = parse_fuji_packet(slip_decode(wire))
        self.assertIsNotNone(pkt)
        assert pkt is not None
        self.assertEqual(pkt.device, 0xFC)
        self.assertEqual(pkt.command, 0x03)
        self.assertEqual(pkt.params, [])
        self.assertEqual(pkt.payload, inner)
        self.assertTrue(pkt.checksum_ok)

    def test_response_status_param_roundtrip(self) -> None:
        inner = bytes([0xAA, 0xBB, 0xCC])
        wire = build_fuji_response_wire(0xFE, 0x02, status=0, payload=inner)
        pkt = parse_fuji_packet(slip_decode(wire))
        self.assertIsNotNone(pkt)
        assert pkt is not None
        self.assertEqual(pkt.params, [0])
        self.assertEqual(pkt.payload, inner)

    def test_multi_param_roundtrip_matches_cpp_layout(self) -> None:
        wire = build_fuji_packet(
            1,
            2,
            b"",
            params=[(0x11, 1), (0x2233, 2), (0x44556677, 4)],
        )
        pkt = parse_fuji_packet(slip_decode(wire))
        self.assertIsNotNone(pkt)
        assert pkt is not None
        self.assertEqual(pkt.params, [0x11, 0x2233, 0x44556677])

    def test_slip_specials_in_payload(self) -> None:
        inner = bytes([0x00, 0xC0, 0xDB, 0xFF])
        wire = build_fuji_packet(3, 4, inner, params=[(0xAA, 1)])
        self.assertEqual(wire[0], 0xC0)
        self.assertEqual(wire[-1], 0xC0)
        pkt = parse_fuji_packet(slip_decode(wire))
        self.assertIsNotNone(pkt)
        assert pkt is not None
        self.assertEqual(pkt.payload, inner)

    def test_decoded_length_includes_header(self) -> None:
        decoded = build_fuji_packet_decoded(0xFC, 0x03, b"\x01\x02")
        self.assertEqual(len(decoded), 6 + 2)
        self.assertEqual(decoded[2] | (decoded[3] << 8), len(decoded))


if __name__ == "__main__":
    unittest.main()
