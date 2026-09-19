#!/usr/bin/env python3
"""Print a compact, read-only view of a feasibility report.json."""

import argparse
import json
import re
import sys
from pathlib import Path


def compact_values(values):
    if not isinstance(values, list):
        return None
    if values == list(range(len(values))) and values:
        return f"{values[0]}..{values[-1]} ({len(values)})"
    return ", ".join(map(str, values))


def timing(values):
    values = [row.get("hold_us") for row in values if isinstance(row, dict)]
    values = [value for value in values if isinstance(value, (int, float))]
    if not values:
        return None
    return "min {:.3g}, max {:.3g}, mean {:.3g} us".format(
        min(values), max(values), sum(values) / len(values)
    )


def format_rate(rate):
    for divisor, suffix in ((1000000, "MHz"), (1000, "kHz")):
        if rate >= divisor:
            return "{:g} {}".format(rate / divisor, suffix)
    return "{:g} Hz".format(rate)


def marked_edge_artifacts(waveform):
    limits = waveform.get("limits", "").lower()
    return "first/last" in limits and ("lead-in" in limits or "release" in limits)


def idle_lines(waveform, manifest):
    lines = []
    values = compact_values(waveform.get("values"))
    if values:
        lines.append("Values: " + values)
    falls = waveform.get("observed_falls")
    if isinstance(falls, list):
        lines.append("/AS falls: " + str(len(falls)))
    if waveform.get("strobe") is not None:
        lines.append("Strobe: " + str(waveform["strobe"]))
    measurements = waveform.get("measurements", [])
    if marked_edge_artifacts(waveform) and len(measurements) >= 3:
        measurements, label = measurements[1:-1], "Interior holds (first/last excluded)"
    else:
        label = "Holds"
    result = timing(measurements)
    if result:
        lines.append(label + ": " + result)
    if manifest.get("data_period_us") is not None:
        lines.append("Configured period: {} us".format(manifest["data_period_us"]))
    return lines


def burst_lines(waveform, manifest):
    lines = []
    if waveform.get("assertions") is not None:
        lines.append("Pulses: " + str(waveform["assertions"]))
    values = compact_values(waveform.get("values"))
    if values:
        lines.append("Values: " + values)
    rows = waveform.get("measurements", [])
    limits = waveform.get("limits", "").lower()
    for report_key, label in (("setup_us", "Setup"), ("low_us", "Width"), ("hold_us", "Hold")):
        selected = rows
        if report_key == "setup_us" and "first setup" in limits:
            selected = rows[1:]
        values = [row.get(report_key) for row in selected if isinstance(row, dict)]
        values = [value for value in values if isinstance(value, (int, float))]
        if values:
            lines.append(label + ": min {:.3g}, max {:.3g} us".format(min(values), max(values)))
    if manifest.get("period_us") is not None:
        lines.append("Configured period: {} us".format(manifest["period_us"]))
    return lines


def held_active_lines(waveform, manifest):
    lines = []
    transactions = waveform.get("transactions", [])
    if transactions:
        transaction = transactions[0]
        lines.append("Asserted transactions: " + str(len(transactions)))
        if transaction.get("capture_value") is not None:
            lines.append("Capture transaction value: " + str(transaction["capture_value"]))
        phases = transaction.get("phases", [])
        values = compact_values([phase.get("value") for phase in phases])
        if values:
            lines.append("Values while /AS low: " + values)
        holds = [phase.get("hold_us") for phase in phases if isinstance(phase, dict)]
        holds = [hold for hold in holds if isinstance(hold, (int, float))]
        if holds:
            lines.append("Held-low phases: min {:.3g}, max {:.3g} us".format(min(holds), max(holds)))
    return lines


def sampling_window_lines(waveform, manifest):
    lines = []
    rows = waveform.get("measurements", [])
    if waveform.get("assertions") is not None:
        lines.append("Sampling cases: " + str(waveform["assertions"]))
    for row in rows:
        if not isinstance(row, dict):
            continue
        case = row.get("case", "case")
        relation = row.get("relation", "")
        captured = row.get("captured")
        offset = row.get("offset_us")
        if captured is not None and offset is not None:
            lines.append("{}: {} {} us; captured {}".format(case, relation, offset, captured))
    return lines


def repetition_lines(waveform, manifest):
    lines = ["Pulses: " + str(waveform["assertions"])] if waveform.get("assertions") is not None else []
    for group in waveform.get("groups", []):
        if isinstance(group, dict):
            lines.append("{}: D={} × {}; low {} us; gap {} us".format(
                group.get("id", "group"), group.get("value", "?"), group.get("count", "?"),
                group.get("pulse_us", "?"), group.get("gap_us", "?")))
    return lines


def width_control_lines(waveform, manifest):
    lines = []
    if waveform.get("assertions") is not None:
        lines.append("/AS assertions: {} ({} accepted)".format(
            waveform["assertions"], waveform.get("accepted_assertions", "?")))
    values = compact_values(waveform.get("values"))
    if values:
        lines.append("Selected-write values: " + values)
    signals = waveform.get("analyzer_signals", {})
    if isinstance(signals, dict):
        data_bits = signals.get("data_bits", {})
        observed = ["/AS", "SELECT", "R/W", "/UDS", "/LDS"]
        if isinstance(data_bits, dict):
            observed.extend(sorted(data_bits))
        lines.append("Analyzer subset: " + ", ".join(observed))
    rows = waveform.get("measurements", [])
    ignored = [row.get("id") for row in rows if isinstance(row, dict) and not row.get("accepted")]
    if ignored:
        lines.append("Ignored controls: " + ", ".join(map(str, ignored)))
    return lines


FORMATTERS = {"idle": idle_lines, "burst": burst_lines, "pulse": burst_lines,
              "held_active": held_active_lines, "sampling_window": sampling_window_lines,
              "repetition": repetition_lines, "width_control": width_control_lines}


def host_test_summary(report):
    results = []
    for command in report.get("commands", []):
        if not isinstance(command, dict) or not command.get("argv"):
            continue
        if Path(str(command["argv"][0])).name != "ctest":
            continue
        match = re.search(r"(\d+)% tests passed out of (\d+)", command.get("output", ""))
        if match:
            total = int(match[2])
            passed = round(total * int(match[1]) / 100)
            argv = list(map(str, command["argv"]))
            try:
                preset = argv[argv.index("--preset") + 1]
            except (ValueError, IndexError):
                preset = None
            label = {"host": "Debug", "host-release": "Release"}.get(preset)
            if label is None and preset:
                label = "preset " + preset
            result = "{}/{} passed".format(passed, total)
            results.append((label + " " if label else "") + result)
    if not results:
        return None
    return "; ".join(results)


def format_report(report):
    build = report.get("build_identity") or {}
    manifest = build.get("manifest") or report.get("manifest") or {}
    waveform = report.get("waveform") or {}
    lines = []
    experiment = report.get("experiment") or manifest.get("id")
    title = manifest.get("title")
    if experiment or title:
        lines.append("Experiment: " + " — ".join(str(x) for x in (experiment, title) if x))
    for label, value in (
        ("Stimulus status", report.get("stimulus_status") or report.get("status")),
        ("Experiment status", report.get("experiment_status")),
        ("Evidence scope", report.get("evidence_scope") or waveform.get("evidence_scope")),
    ):
        if value is not None:
            lines.append(f"{label}: {value}")
    dut = report.get("dut_evidence")
    if isinstance(dut, dict):
        if dut.get("status") is not None:
            lines.append("DUT evidence: " + str(dut["status"]))
        if dut.get("reason"):
            lines.append("DUT reason: " + str(dut["reason"]))
        if isinstance(dut.get("observed"), dict):
            counters = ", ".join(
                f"{key}={value}" for key, value in dut["observed"].items()
                if key != "values" or value
            )
            lines.append("DUT observed: " + counters)
    if build.get("revision"):
        lines.append("Git revision: " + str(build["revision"]))
    firmware = report.get("firmware_sha256") or build.get("firmware_sha256")
    if firmware:
        lines.append("Firmware SHA-256: " + str(firmware))
    if waveform.get("sample_rate") is not None:
        lines.append("Analyzer sample rate: " + format_rate(waveform["sample_rate"]))
    if report.get("waveform_visual"):
        lines.append("Waveform map: " + str(report["waveform_visual"]))
    host_tests = host_test_summary(report)
    if host_tests:
        lines.append("Host tests: " + host_tests)
    kind = waveform.get("analysis_kind")
    formatter = FORMATTERS.get(kind)
    if formatter:
        lines.extend(formatter(waveform, manifest))
    elif kind:
        lines.append("Analysis kind: " + str(kind))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="path to experiment report.json")
    args = parser.parse_args(argv)
    try:
        report = json.loads(args.report.read_text())
    except (OSError, json.JSONDecodeError) as error:
        parser.error("cannot read report: " + str(error))
    if not isinstance(report, dict):
        parser.error("report root must be a JSON object")
    print(format_report(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
