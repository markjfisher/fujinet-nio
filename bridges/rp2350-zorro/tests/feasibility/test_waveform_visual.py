#!/usr/bin/env python3
import importlib.util
import json
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
        self.assertNotIn("detected transaction window", text)
        self.assertNotIn("Transaction boundaries, sampling", text)
        self.assertIn("Detail window: 0 us–1.000 ms (1.000 ms window)", text)
        self.assertIn(">D3<", text)
        self.assertIn(">/AS<", text)
        self.assertIn(">DUT reported:<", text)
        self.assertIn(">0x3<", text)
        self.assertIn("0xA", text)
        self.assertNotIn("not externally visible", text)

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
        self.assertIn("Detail window: 0 us–300 us (300 us window)", text)
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

    def test_sampling_window_svg_labels_before_after_cases(self):
        report = {
            "experiment": "C3",
            "waveform": {"analysis_kind": "sampling_window", "sample_rate": 1_000_000,
                         "measurements": [
                             {"case": "pre-10", "relation": "before", "offset_us": 10, "captured": 1},
                             {"case": "post-10", "relation": "after", "offset_us": 10, "captured": 2},
                             {"case": "pre-50", "relation": "before", "offset_us": 50, "captured": 5},
                             {"case": "post-50", "relation": "after", "offset_us": 50, "captured": 6},
                         ],
                         "transactions": [
                             {"assert_sample": 100, "release_sample": 120, "capture_value": 1,
                              "phases": [{"value": 1, "start_sample": 100, "end_sample": 120}]},
                             {"assert_sample": 200, "release_sample": 220, "capture_value": 2,
                              "phases": [{"value": 2, "start_sample": 200, "end_sample": 210},
                                         {"value": 3, "start_sample": 210, "end_sample": 220}]},
                             {"assert_sample": 350, "release_sample": 360, "capture_value": 5,
                              "phases": [{"value": 5, "start_sample": 350, "end_sample": 360}]},
                             {"assert_sample": 440, "release_sample": 500, "capture_value": 6,
                              "phases": [{"value": 6, "start_sample": 440, "end_sample": 490},
                                         {"value": 7, "start_sample": 490, "end_sample": 500}]},
                         ]},
        }
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "waveform.svg"
            visual.write_svg(report, output)
            text = output.read_text()
        self.assertIn(">pre-10:<", text)
        self.assertIn(">data before /AS by 10 us; captured 0x1<", text)
        self.assertIn("captured at /AS fall", text)
        self.assertIn('class="capture-phase"', text)
        for captured in ("0x1", "0x2", "0x5", "0x6"):
            self.assertIn(">" + captured + "<", text)

    def test_repetition_svg_describes_each_declared_group(self):
        report = {
            "experiment": "C4",
            "waveform": {"analysis_kind": "repetition", "sample_rate": 1_000_000,
                         "groups": [
                             {"id": "low-20", "value": 3, "count": 4,
                              "pulse_us": 20, "gap_us": 100},
                             {"id": "gap-20", "value": 5, "count": 4,
                              "pulse_us": 100, "gap_us": 20},
                         ],
                         "transactions": [
                             {"assert_sample": 100, "release_sample": 120,
                              "capture_value": 3,
                              "phases": [{"value": 3, "start_sample": 100,
                                          "end_sample": 120}]},
                             {"assert_sample": 220, "release_sample": 320,
                              "capture_value": 5,
                              "phases": [{"value": 5, "start_sample": 220,
                                          "end_sample": 320}]},
                         ]},
        }
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "waveform.svg"
            visual.write_svg(report, output)
            text = output.read_text()
        self.assertIn(">low-20:<", text)
        self.assertIn(">D=3 × 4; low 20 us; internal gap 100<", text)
        self.assertIn(">us<", text)
        self.assertIn(">gap-20:<", text)
        self.assertIn(">D=5 × 4; low 100 us; internal gap 20<", text)

    def test_width_control_svg_shows_control_subset_and_ignored_assertions(self):
        manifest = json.loads((HERE / "C5-width-control/experiment.json").read_text())
        report = {
            "experiment": "C5",
            "build_identity": {"manifest": manifest},
            "dut_evidence": {"observed": {"values": [10]}},
            "waveform": {"analysis_kind": "width_control", "sample_rate": 1_000_000,
                         "analyzer_coverage": {"summary": "3/16 data bits + 5 controls observed"},
                         "transactions": [
                             {"id": "unselected", "accepted": False,
                              "assert_sample": 100, "release_sample": 200,
                              "capture_value": 0x1357,
                              "phases": [{"value": 0x1357, "start_sample": 100,
                                          "end_sample": 200}]},
                             {"id": "four-bit", "accepted": True,
                              "assert_sample": 300, "release_sample": 400,
                              "capture_value": 10,
                              "phases": [{"value": 10, "start_sample": 300,
                                          "end_sample": 400}]},
                         ],
                         "limits": "subset only"},
        }
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "waveform.svg"
            visual.write_svg(report, output)
            text = output.read_text()
        self.assertIn(">D15<", text)
        self.assertIn(">SELECT<", text)
        self.assertIn('class="ignored"', text)
        self.assertIn("Expected selected writes", text)
        self.assertIn(">Ignored controls:<", text)
        self.assertIn(">unselected<", text)
        self.assertIn(">Analyzer mapping:<", text)
        self.assertIn("D15←CH8, D8←CH7, D0←CH6", text)
        self.assertIn(">Analyzer coverage:<", text)
        self.assertIn(">3/16 data bits + 5 controls observed<", text)
        self.assertNotIn("Transaction values are hexadecimal.", text)
        self.assertIn(">A<", text)
        self.assertIn(">R<", text)
        self.assertIn(">Labels<", text)
        self.assertIn("solid dark-green /AS boundary", text)
        self.assertIn(".ignored{stroke:#e67300;stroke-width:2.4;stroke-dasharray:7 3}", text)
        self.assertIn('<text x="600.0" y="477" class="small" text-anchor="middle">Detail window:', text)
        self.assertIn('<rect class="table" x="560" y="491" width="620"', text)
        self.assertIn('<rect class="table" x="20" y="593" width="524"', text)

    def test_manifest_metrics_render_pressure_reconciliation(self):
        manifest = json.loads((HERE / "C6-pressure/experiment.json").read_text())
        report = {
            "experiment": "C6", "build_identity": {"manifest": manifest},
            "dut_evidence": {"observed": {
                "values": [0x1000, 0x1001, 0xd001],
                "pressure_pause_count": 1, "pressure_pause_us": 2000,
                "unobserved_assertion_count": 11}},
            "waveform": {"analysis_kind": "width_control", "sample_rate": 1_000_000,
                         "transactions": [
                             {"accepted": True, "assert_sample": 100, "release_sample": 150,
                              "capture_value": 0x1000,
                              "phases": [{"value": 0x1000, "start_sample": 100, "end_sample": 150}]},
                             {"accepted": True, "assert_sample": 300, "release_sample": 350,
                              "capture_value": 0xd001,
                              "phases": [{"value": 0xd001, "start_sample": 300, "end_sample": 350}]}]},
        }
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "waveform.svg"
            visual.write_svg(report, output)
            text = output.read_text()
        self.assertIn(">Drain pauses:<", text)
        self.assertIn(">Drain pause:<", text)
        self.assertIn(">Unobserved assertions:<", text)
        self.assertIn(">11<", text)

    def test_manifest_drives_wide_lane_mapping_and_window_scale(self):
        report = {
            "experiment": "wide",
            "manifest": {"title": "Generic wide capture", "waveform_view": {
                "lanes": [
                    {"label": "D[47:0]", "role": "data", "bits": [
                        {"label": "D47", "channel": "D47", "bus_bit": 47},
                        {"label": "D23", "channel": "D23", "bus_bit": 23},
                        {"label": "D0", "channel": "D0", "bus_bit": 0},
                    ]},
                    {"label": "READY", "role": "control", "channel": "D41", "polarity": "active-low"},
                    {"label": "/AS", "role": "strobe", "channel": "D40", "polarity": "active-low"},
                ],
                "transactions": {"boundary_label": "/AS", "expected_label": "Expected writes"},
            }},
            "waveform": {"sample_rate": 1_000_000,
                         "transactions": [{"classification": "accepted", "assert_sample": 20_000,
                                           "release_sample": 20_100, "capture_value": 0x1234,
                                           "phases": [{"value": 0x1234, "start_sample": 20_000,
                                                       "end_sample": 20_100}]},
                                          {"classification": "uncertain", "assert_sample": 20_300,
                                           "release_sample": 20_400, "capture_value": 0x5678,
                                           "phases": [{"value": 0x5678, "start_sample": 20_300,
                                                       "end_sample": 20_400}]}]},
        }
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "waveform.svg"
            visual.write_svg(report, output)
            text = output.read_text()
        for label in ("D47", "D23", "D0", "READY", "/AS"):
            self.assertIn(">" + label + "<", text)
        self.assertIn('data-polarity="active-low"', text)
        self.assertIn(">Analyzer mapping:<", text)
        self.assertIn(">D47←CH48, D23←CH24, D0←CH1,<", text)
        self.assertIn(">READY←CH42, /AS←CH41<", text)
        self.assertIn('class="uncertain"', text)
        self.assertIn(">?<", text)
        self.assertIn(".uncertain{stroke:#b8860b;stroke-width:2.4;stroke-dasharray:1 3}", text)
        self.assertIn("Detail window: 19.900 ms–20.500 ms (600 us window)", text)
        self.assertNotIn("5.000 s", text)

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
        self.assertIn("Detail window: 0 us–400 us (400 us window)", text)
        self.assertNotIn("detected idle sequence", text)
        self.assertIn("/AS: high throughout the saved capture", text)
        self.assertIn("capture_count=0; capture_irq_count=0", text)
        self.assertIn("Decoded data while /AS is high", text)
        self.assertNotIn("No DUT capture count or IRQ was observed.", text)


if __name__ == "__main__":
    unittest.main()
