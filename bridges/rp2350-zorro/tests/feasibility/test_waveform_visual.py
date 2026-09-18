#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile


HERE = Path(__file__).parent
SPEC = importlib.util.spec_from_file_location("waveform_visual", HERE / "waveform_visual.py")
visual = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(visual)


class WaveformVisualTests(unittest.TestCase):
    def test_held_active_svg_shows_acquisition_context_and_raw_traces(self):
        report = {
            "experiment": "C2",
            "dut_evidence": {"observed": {"values": [3]}},
            "waveform": {"limits": "sample clock is not externally visible",
                         "sample_rate": 1_000_000,
                         "transactions": [{"index": 0, "assert_sample": 100,
                                           "release_sample": 930, "capture_value": 3,
                                           "phases": [{"value": 3, "start_sample": 100, "end_sample": 310},
                                                      {"value": 10, "start_sample": 310, "end_sample": 520},
                                                      {"value": 5, "start_sample": 520, "end_sample": 730},
                                                      {"value": 12, "start_sample": 730, "end_sample": 930}]}]},
        }
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.sr"
            samples = bytes(
                [0x80] * 100
                + [0x03] * 210
                + [0x0A] * 210
                + [0x05] * 210
                + [0x0C] * 200
                + [0x80] * 70)
            with zipfile.ZipFile(capture, "w") as archive:
                archive.writestr("logic-1-2", samples[500:])
                archive.writestr("logic-1-1", samples[:500])
            report["waveform"]["capture"] = str(capture)
            output = Path(directory) / "waveform.svg"
            visual.write_svg(report, output)
            text = output.read_text()
        self.assertIn('fill="white"', text)
        self.assertIn("whole acquisition", text)
        self.assertIn("event 100 us–930 us", text)
        self.assertIn("detail: 0 us–1.000 ms", text)
        self.assertIn(">D3<", text)
        self.assertIn(">/AS<", text)
        self.assertIn('class="overview-event"', text)
        self.assertIn("DUT reported: 0x3", text)
        self.assertIn("0xA", text)
        self.assertIn("not externally visible", text)

    def test_unavailable_capture_keeps_analysed_event_map_readable(self):
        report = {
            "experiment": "C2",
            "waveform": {"sample_rate": 1_000_000, "capture": "/not/present.sr",
                         "transactions": [{"assert_sample": 100, "release_sample": 200,
                                           "capture_value": 3, "phases": []}]},
        }
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "waveform.svg"
            visual.write_svg(report, output)
            text = output.read_text()
        self.assertIn("whole acquisition", text)
        self.assertNotIn('class="data" d=', text)

    def test_repeated_transactions_label_values_across_pulse_intervals(self):
        report = {
            "experiment": "C1",
            "waveform": {"sample_rate": 1_000_000,
                         "transactions": [
                             {"assert_sample": 100, "release_sample": 150,
                              "capture_value": 1, "phases": [{"value": 1, "start_sample": 100, "end_sample": 150}]},
                             {"assert_sample": 300, "release_sample": 350,
                              "capture_value": 10, "phases": [{"value": 10, "start_sample": 300, "end_sample": 350}]},
                         ]},
        }
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "waveform.svg"
            visual.write_svg(report, output)
            text = output.read_text()
        self.assertIn(">0x1<", text)
        self.assertIn("Expected at /AS falls", text)

    def test_idle_svg_shows_data_changes_high_strobe_and_dut_counters(self):
        report = {
            "experiment": "C0",
            "dut_evidence": {"status": "observed", "observed": {
                "capture_count": 0, "capture_irq_count": 0}},
            "waveform": {"analysis_kind": "idle", "sample_rate": 1_000_000,
                         "limits": "lead-in may be unobservable. No DUT capture count or IRQ was observed.",
                         "measurements": [
                             {"value": 0, "start_sample": 0, "end_sample": 100},
                             {"value": 1, "start_sample": 100, "end_sample": 200},
                             {"value": 2, "start_sample": 200, "end_sample": 300},
                         ]},
        }
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.sr"
            with zipfile.ZipFile(capture, "w") as archive:
                archive.writestr("logic-1-1", bytes([0x80] * 100 + [0x81] * 100 + [0x82] * 100 + [0x80] * 200))
            report["waveform"]["capture"] = str(capture)
            output = Path(directory) / "waveform.svg"
            visual.write_svg(report, output)
            text = output.read_text()
        self.assertIn("data-transition sequence", text)
        self.assertIn("/AS: high throughout the saved capture", text)
        self.assertIn("capture_count=0; capture_irq_count=0", text)
        self.assertIn("Decoded data while /AS is high", text)
        self.assertNotIn("No DUT capture count or IRQ was observed.", text)


if __name__ == "__main__":
    unittest.main()
