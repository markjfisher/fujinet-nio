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
            "dut_evidence": {"status": "observed", "observed": {
                "capture_count": 0, "capture_irq_count": 0}},
        })
        self.assertIn("DUT observed: capture_count=0, capture_irq_count=0", output)

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
