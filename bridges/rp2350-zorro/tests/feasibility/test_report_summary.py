#!/usr/bin/env python3
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest


HERE = Path(__file__).parent
SPEC = importlib.util.spec_from_file_location("report_summary", HERE / "report_summary.py")
summary = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(summary)


class ReportSummaryTests(unittest.TestCase):
    def c0_report(self):
        return {
            "experiment": "C0",
            "status": "stimulus_passed",
            "stimulus_status": "passed",
            "experiment_status": "incomplete",
            "firmware_sha256": "firmware",
            "build_identity": {"revision": "revision", "manifest": {
                "title": "Idle capture suppression", "data_period_us": 210}},
            "dut_evidence": {"status": "not_observed", "reason": "No observer"},
            "commands": [
                {"argv": ["ctest", "--preset", "host"],
                 "output": "100% tests passed out of 8"},
                {"argv": ["ctest", "--preset", "host-release"],
                 "output": "100% tests passed out of 8"},
            ],
            "waveform": {"analysis_kind": "idle", "sample_rate": 1000000,
                         "evidence_scope": "stimulus-only", "values": list(range(16)),
                         "observed_falls": [], "strobe": "high throughout capture",
                         "limits": "First/last data hold includes unobservable lead-in/release.",
                         "measurements": [{"hold_us": 320}, {"hold_us": 209},
                                          {"hold_us": 210}, {"hold_us": 5000}]},
        }

    def test_c0_idle_summary_excludes_marked_edges(self):
        output = summary.format_report(self.c0_report())
        self.assertIn("Experiment: C0 — Idle capture suppression", output)
        self.assertIn("/AS falls: 0", output)
        self.assertIn("Interior holds (first/last excluded): min 209, max 210, mean 210 us", output)
        self.assertIn("Configured period: 210 us", output)
        self.assertIn("Host tests: Debug 8/8 passed; Release 8/8 passed", output)

    def test_missing_optional_fields(self):
        self.assertEqual(summary.format_report({"status": "failed"}), "Stimulus status: failed")

    def test_dut_observed_counters_are_presented(self):
        output = summary.format_report({
            "status": "passed",
            "evidence_scope": "stimulus-and-dut",
            "dut_evidence": {"status": "observed", "observed": {
                "capture_count": 0, "capture_irq_count": 0}},
        })
        self.assertIn("Evidence scope: stimulus-and-dut", output)
        self.assertIn("DUT observed: capture_count=0, capture_irq_count=0", output)

    def test_burst_summary_uses_declared_analysis_kind(self):
        output = summary.format_report({
            "status": "passed",
            "waveform": {"analysis_kind": "burst", "assertions": 2,
                         "values": [1, 2],
                         "limits": "First setup is not independently observable.",
                         "measurements": [
                             {"setup_us": 9999, "low_us": 100, "hold_us": 110},
                             {"setup_us": 100, "low_us": 100, "hold_us": 125},
                         ]},
        })
        self.assertIn("Pulses: 2", output)
        self.assertIn("Setup: min 100, max 100 us", output)
        self.assertIn("Hold: min 110, max 125 us", output)

    def test_held_active_summary_links_capture_to_held_values(self):
        output = summary.format_report({
            "status": "passed",
            "waveform_visual": "/tmp/waveform.svg",
            "waveform": {"analysis_kind": "held_active", "transactions": [{
                "capture_value": 3,
                "phases": [{"value": 3, "hold_us": 210},
                           {"value": 10, "hold_us": 210},
                           {"value": 5, "hold_us": 210},
                           {"value": 12, "hold_us": 200}],
            }]},
        })
        self.assertIn("Capture transaction value: 3", output)
        self.assertIn("Values while /AS low: 3, 10, 5, 12", output)
        self.assertIn("Waveform SVG: /tmp/waveform.svg", output)

    def test_sampling_window_summary_shows_offsets_and_capture_side(self):
        output = summary.format_report({
            "status": "passed",
            "waveform": {"analysis_kind": "sampling_window", "assertions": 2,
                         "measurements": [
                             {"case": "pre-10", "relation": "before", "offset_us": 10, "captured": 1},
                             {"case": "post-10", "relation": "after", "offset_us": 10, "captured": 2},
                         ]},
        })
        self.assertIn("Sampling cases: 2", output)
        self.assertIn("pre-10: before 10 us; captured 1", output)
        self.assertIn("post-10: after 10 us; captured 2", output)

    def test_repetition_summary_uses_manifest_described_groups(self):
        output = summary.format_report({
            "status": "passed",
            "waveform": {"analysis_kind": "repetition", "assertions": 8,
                         "groups": [
                             {"id": "low-20", "value": 3, "count": 4,
                              "pulse_us": 20, "gap_us": 100},
                             {"id": "gap-20", "value": 5, "count": 4,
                              "pulse_us": 100, "gap_us": 20},
                         ]},
        })
        self.assertIn("Pulses: 8", output)
        self.assertIn("low-20: D=3 × 4; low 20 us; gap 100 us", output)
        self.assertIn("gap-20: D=5 × 4; low 100 us; gap 20 us", output)

    def test_width_control_summary_describes_limited_analyzer_coverage(self):
        output = summary.format_report({
            "status": "passed",
            "waveform": {"analysis_kind": "width_control", "assertions": 22,
                         "accepted_assertions": 18, "values": [10, 165],
                         "analyzer_signals": {"data_bits": {
                             "D0": "D5", "D8": "D6", "D15": "D7"}},
                         "measurements": [
                             {"id": "unselected", "accepted": False},
                             {"id": "read", "accepted": False},
                         ]},
        })
        self.assertIn("/AS assertions: 22 (18 accepted)", output)
        self.assertIn("Analyzer subset: /AS, SELECT, R/W, /UDS, /LDS, D0, D15, D8", output)
        self.assertIn("Ignored controls: unselected, read", output)

    def test_unknown_analysis_kind_uses_generic_output(self):
        output = summary.format_report({"status": "passed", "waveform": {"analysis_kind": "future"}})
        self.assertIn("Stimulus status: passed", output)
        self.assertIn("Analysis kind: future", output)

    def test_malformed_and_missing_report_path(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.json"
            path.write_text("not json")
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                summary.main([str(path)])
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                summary.main([str(path.with_name("missing.json"))])


if __name__ == "__main__":
    unittest.main()
