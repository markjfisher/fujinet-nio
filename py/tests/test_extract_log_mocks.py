from __future__ import annotations

import argparse
import tempfile
import unittest
from pathlib import Path

from fujinet_tools import diskproto as dp
from fujinet_tools import fileproto as fp
from fujinet_tools.extract_log_mocks import (
    _filename_base,
    _request_filename_suffix,
    extract_log_mocks,
    extract_mocks_from_log,
)
from fujinet_tools.fujibus import parse_fuji_packet, slip_decode

_FIXTURE = Path(__file__).resolve().parents[1] / "unittest_data" / "cat01.txt"


class TestExtractLogMocks(unittest.TestCase):
    def test_resolve_path_filename_not_make_directory(self) -> None:
        req = fp.build_resolve_path_req(
            base_uri="tnfs://server/root", arg="NEXT"
        )
        suffix = _request_filename_suffix(fp.FILE_DEVICE_ID, fp.CMD_RESOLVE_PATH, req)
        self.assertEqual(suffix, "root_NEXT")
        name = _filename_base(1, fp.FILE_DEVICE_ID, fp.CMD_RESOLVE_PATH, req)
        self.assertEqual(name, "001_resolve_path_root_NEXT")

    def test_make_directory_filename(self) -> None:
        req = fp.build_mkdir_req(uri="sd0:/newdir")
        name = _filename_base(2, fp.FILE_DEVICE_ID, fp.CMD_MAKE_DIRECTORY, req)
        self.assertEqual(name, "002_make_directory_newdir")

    def test_cat01_wire_frames(self) -> None:
        if not _FIXTURE.is_file():
            self.skipTest(f"fixture missing: {_FIXTURE}")

        mocks = extract_mocks_from_log(_FIXTURE)
        self.assertEqual(len(mocks), 2)

        m0 = mocks[0]
        self.assertEqual(m0.filename_base, "001_read_sector_0")
        self.assertGreater(len(m0.request_wire), len(m0.request_payload))
        self.assertGreater(len(m0.response_wire), len(m0.response_payload))

        req_pkt = parse_fuji_packet(slip_decode(m0.request_wire))
        resp_pkt = parse_fuji_packet(slip_decode(m0.response_wire))
        self.assertIsNotNone(req_pkt)
        self.assertIsNotNone(resp_pkt)
        assert req_pkt is not None
        assert resp_pkt is not None
        self.assertEqual(req_pkt.device, 0xFC)
        self.assertEqual(req_pkt.command, 0x03)
        self.assertEqual(req_pkt.payload, m0.request_payload)
        self.assertEqual(resp_pkt.params, [0])
        self.assertEqual(resp_pkt.payload, m0.response_payload)

        r0 = dp.parse_read_sector_resp(m0.response_payload)
        self.assertEqual(r0.lba, 0)
        self.assertIn(b"lcww1", r0.data)

    def test_writes_wire_binary_files(self) -> None:
        if not _FIXTURE.is_file():
            self.skipTest(f"fixture missing: {_FIXTURE}")

        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            rc = extract_log_mocks(
                argparse.Namespace(
                    log=str(_FIXTURE),
                    output=str(out),
                    device=None,
                    command=None,
                    inner_payload_only=False,
                    dry_run=False,
                )
            )
            self.assertEqual(rc, 0)
            req = out / "001_read_sector_0_req.bin"
            resp = out / "001_read_sector_0_resp.bin"
            self.assertTrue(req.is_file())
            self.assertTrue(resp.is_file())
            self.assertGreater(req.stat().st_size, 8)
            self.assertGreater(resp.stat().st_size, 267)

    def test_inner_payload_only_mode(self) -> None:
        if not _FIXTURE.is_file():
            self.skipTest(f"fixture missing: {_FIXTURE}")

        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            rc = extract_log_mocks(
                argparse.Namespace(
                    log=str(_FIXTURE),
                    output=str(out),
                    device=None,
                    command=None,
                    inner_payload_only=True,
                    dry_run=False,
                )
            )
            self.assertEqual(rc, 0)
            self.assertEqual((out / "001_read_sector_0_req.bin").stat().st_size, 8)
            self.assertEqual((out / "001_read_sector_0_resp.bin").stat().st_size, 267)
