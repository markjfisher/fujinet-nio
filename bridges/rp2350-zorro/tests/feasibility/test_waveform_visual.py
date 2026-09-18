#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import tempfile
import unittest


HERE = Path(__file__).parent
SPEC = importlib.util.spec_from_file_location("waveform_visual", HERE / "waveform_visual.py")
visual = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(visual)


class WaveformVisualTests(unittest.TestCase):
    def test_held_active_svg_maps_phases_and_dut_value(self):
        report = {
            "experiment": "C2",
            "dut_evidence": {"observed": {"values": [3]}},
            "waveform": {"limits": "sample clock is not externally visible",
                         "transactions": [{"index": 0, "assert_sample": 100,
                                           "release_sample": 930, "capture_value": 3,
                                           "phases": [{"value": 3, "start_sample": 100, "end_sample": 310},
                                                      {"value": 10, "start_sample": 310, "end_sample": 520},
                                                      {"value": 5, "start_sample": 520, "end_sample": 730},
                                                      {"value": 12, "start_sample": 730, "end_sample": 930}]}]},
        }
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "waveform.svg"
            visual.write_svg(report, output)
            text = output.read_text()
        self.assertIn("transaction 0", text)
        self.assertIn("DUT reported 0x3", text)
        self.assertIn("0xA", text)
        self.assertIn("not an electrically observed internal PIO clock", text)


if __name__ == "__main__":
    unittest.main()
