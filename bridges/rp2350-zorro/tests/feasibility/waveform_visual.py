#!/usr/bin/env python3
"""Render read-only SVG evidence from an analysed feasibility report.

``experiment.py`` is the authority for waveform validation.  This module only
uses the transaction boundaries, classifications and lane description already
recorded in a report.  The optional ``waveform_view`` manifest field is copied
into ``waveform.visualization`` by the analyser, so an SVG remains interpretable
without finding the source checkout that created it.
"""

import argparse
import configparser
import html
import json
from pathlib import Path
import re
import zipfile


class Capture:
    """Logic samples decoded from sigrok's byte-packed capture members."""

    def __init__(self, values):
        self.values = values

    def __len__(self):
        return len(self.values)


def value(number):
    return "0x{:X}".format(number) if isinstance(number, int) else "?"


def transactions(waveform):
    if isinstance(waveform.get("transactions"), list):
        return waveform["transactions"]
    result = []
    for index, row in enumerate(waveform.get("measurements", [])):
        if isinstance(row, dict) and "fall_sample" in row and "rise_sample" in row:
            result.append(dict(index=index, assert_sample=row["fall_sample"],
                               release_sample=row["rise_sample"],
                               capture_value=row.get("value"), phases=[dict(
                                   value=row.get("value"),
                                   start_sample=row["fall_sample"],
                                   end_sample=row["rise_sample"])]))
    return result


def capture_samples(capture_path):
    """Return decoded sample words, including future multi-byte analyzers.

    A current fx2lafw capture stores one byte per sample.  Sigrok's metadata
    specifies ``unitsize`` for wider analyzers, whose sample words are decoded
    little-endian here.  No renderer policy depends on a maximum channel count.
    """
    if not capture_path:
        return None
    try:
        with zipfile.ZipFile(capture_path) as archive:
            members = []
            for name in archive.namelist():
                matched = re.fullmatch(r"logic-1-(\d+)", name)
                if matched:
                    members.append((int(matched[1]), name))
            if not members:
                return None
            unit_size = 1
            if "metadata" in archive.namelist():
                meta = configparser.ConfigParser()
                meta.read_string(archive.read("metadata").decode())
                unit_size = int(meta.get("device 1", "unitsize", fallback="1"))
            if unit_size <= 0:
                return None
            raw = b"".join(archive.read(name) for _, name in sorted(members))
            if len(raw) % unit_size:
                return None
            return Capture([int.from_bytes(raw[offset:offset + unit_size], "little")
                            for offset in range(0, len(raw), unit_size)])
    except (OSError, ValueError, zipfile.BadZipFile, KeyError, configparser.Error):
        return None


def time_text(sample, sample_rate):
    seconds = sample / sample_rate
    if seconds < 0.001:
        return "{:.0f} us".format(seconds * 1_000_000)
    if seconds < 1:
        return "{:.3f} ms".format(seconds * 1_000)
    return "{:.3f} s".format(seconds)


def report_manifest(report):
    build = report.get("build_identity") or {}
    return build.get("manifest") or report.get("manifest") or {}


def channel_bit(channel):
    """Resolve a sigrok logical channel name, or an explicit sample bit."""
    if isinstance(channel, dict):
        explicit = channel.get("sample_bit")
        if isinstance(explicit, int) and explicit >= 0:
            return explicit
        channel = channel.get("channel")
    matched = re.fullmatch(r"D(\d+)", str(channel))
    return int(matched[1]) if matched else None


def default_view(report, waveform):
    """Read old reports without experiment-ID branches.

    New manifests declare ``waveform_view``.  This fallback derives the W0/W1
    layouts from data GPIO width, recorded analyzer mapping and channel order,
    so historical evidence remains readable.
    """
    manifest = report_manifest(report)
    signals = waveform.get("analyzer_signals") or manifest.get("analyzer_signals")
    if isinstance(signals, dict):
        data_bits = signals.get("data_bits") or {}
        bits = []
        for label, channel in sorted(data_bits.items(), key=lambda item: -int(str(item[0])[1:])):
            bits.append({"label": label, "channel": channel,
                         "bus_bit": int(str(label)[1:])})
        controls = [
            {"label": "/LDS", "role": "control", "channel": signals.get("lds"), "polarity": "active-low"},
            {"label": "/UDS", "role": "control", "channel": signals.get("uds"), "polarity": "active-low"},
            {"label": "R/W", "role": "control", "channel": signals.get("rw"), "polarity": "active-high"},
            {"label": "SELECT", "role": "control", "channel": signals.get("select"), "polarity": "active-high"},
        ]
        return {"lanes": ([{"label": "Data", "role": "data", "bits": bits}] + controls +
                          [{"label": "/AS", "role": "strobe", "channel": signals.get("as"), "polarity": "active-low"}]),
                "transactions": {"expected_label": "Expected selected writes",
                                 "rejected_summary_label": "Ignored controls"}}
    channels = manifest.get("analyzer_channels") or []
    data_width = len(manifest.get("data_gpio") or [])
    bits = [{"label": "D{}".format(index), "channel": channel,
             "bus_bit": index}
            for index, channel in reversed(list(enumerate(channels[:data_width])))]
    # Hand-authored/old reports may lack their manifest. Keep those reports
    # inspectable with the original W0 capture convention; this is a schema
    # fallback, not an experiment-name decision.
    if not bits:
        bits = [{"label": "D{}".format(index), "channel": "D{}".format(index),
                 "bus_bit": index}
                for index in range(3, -1, -1)]
    strobe = channels[-1] if channels else None
    if strobe is None:
        strobe = "D7"
    return {"lanes": [{"label": "Data", "role": "data", "bits": bits},
                       {"label": "/AS", "role": "strobe", "channel": strobe,
                        "polarity": "active-low"}],
            "transactions": {"expected_label": "Expected at /AS falls"}}


def view_config(report, waveform):
    configured = (waveform.get("visualization") or
                  report_manifest(report).get("waveform_view"))
    return configured if isinstance(configured, dict) else default_view(report, waveform)


def lanes(config):
    """Expand manifest buses into physical, ordered sample lanes."""
    result = []
    for declaration in config.get("lanes", []):
        if not isinstance(declaration, dict):
            continue
        if isinstance(declaration.get("bits"), list):
            for bit in declaration["bits"]:
                if not isinstance(bit, dict):
                    continue
                label = bit.get("label")
                if not label and isinstance(bit.get("bus_bit"), int):
                    label = "D{}".format(bit["bus_bit"])
                result.append(dict(label=str(label or declaration.get("label", "data")),
                                   role=declaration.get("role", "data"),
                                   polarity=bit.get("polarity", declaration.get("polarity", "active-high")),
                                   channel=bit.get("channel"),
                                   sample_bit=channel_bit(bit),
                                   bus_label=declaration.get("label")))
        else:
            result.append(dict(label=str(declaration.get("label", "signal")),
                               role=declaration.get("role", "control"),
                               polarity=declaration.get("polarity", "active-high"),
                               channel=declaration.get("channel"),
                               sample_bit=channel_bit(declaration)))
    if not result:
        raise ValueError("report has no visual lane configuration")
    return result


def transaction_options(config):
    options = config.get("transactions") or {}
    return options if isinstance(options, dict) else {}


def event_state(event):
    classification = event.get("classification")
    if classification in ("accepted", "rejected", "uncertain"):
        return classification
    if event.get("accepted") is True:
        return "accepted"
    if event.get("accepted") is False:
        return "rejected"
    return "accepted"


def transition_path(samples, start, end, bit, x, high_y, low_y):
    if samples is None or bit is None or start >= len(samples):
        return None
    end = min(end, len(samples))
    initial = bool(samples.values[start] & (1 << bit))
    y = high_y if initial else low_y
    pieces = ["M {:.2f} {:.2f}".format(x(start), y)]
    for sample in range(start + 1, end):
        state = bool(samples.values[sample] & (1 << bit))
        next_y = high_y if state else low_y
        if next_y != y:
            pieces.append("H {:.2f} V {:.2f}".format(x(sample), next_y))
            y = next_y
    pieces.append("H {:.2f}".format(x(end)))
    return " ".join(pieces)


def transaction_value(number, options):
    """Use a manifest-selected compact form where the SVG declares hex."""
    if options.get("value_format") == "hex" and isinstance(number, int):
        return "{:X}".format(number)
    return value(number)


def sequence_lines(label, numbers, per_line=14, renderer=value):
    rendered = [renderer(number) for number in numbers]
    if not rendered:
        return []
    lines = []
    for offset in range(0, len(rendered), per_line):
        prefix = label if offset == 0 else " " * len(label)
        lines.append(prefix + ", ".join(rendered[offset:offset + per_line]))
    return lines


def visual_window(first, last, capture_length):
    span = max(1, last - first)
    padding = max(100, span // 5)
    start = max(0, first - padding)
    end = last + padding
    if capture_length:
        end = min(capture_length, end)
    return start, max(start + 1, end)


def semantic_annotations(kind, waveform):
    lines = []
    if kind == "sampling_window":
        for row in waveform.get("measurements", []):
            if isinstance(row, dict):
                lines.append("{}: data {} /AS by {} us; captured {}".format(
                    row.get("case", "case"), row.get("relation", "at"),
                    row.get("offset_us", "?"), value(row.get("captured"))))
    elif kind == "repetition":
        for group in waveform.get("groups", []):
            if isinstance(group, dict):
                lines.append("{}: D={} × {}; low {} us; internal gap {} us".format(
                    group.get("id", "group"), group.get("value", "?"), group.get("count", "?"),
                    group.get("pulse_us", "?"), group.get("gap_us", "?")))
    return lines


def transaction_annotations(report, waveform, config, events):
    options = transaction_options(config)
    observed = ((report.get("dut_evidence") or {}).get("observed") or {}).get("values", [])
    accepted = [event.get("capture_value") for event in events if event_state(event) == "accepted"]
    rejected = [event for event in events if event_state(event) == "rejected"]
    uncertain = [event for event in events if event_state(event) == "uncertain"]
    if rejected or uncertain:
        expected_label = options.get("expected_label", "Expected accepted transactions")
    else:
        expected_label = options.get("expected_label", "Expected at /AS falls")
    renderer = lambda number: transaction_value(number, options)
    lines = sequence_lines(expected_label + ": ", accepted, renderer=renderer)
    lines += sequence_lines(options.get("report_label", "DUT reported") + ": ", observed,
                            renderer=renderer)
    if rejected or uncertain:
        labels = []
        if accepted:
            labels.append("{} accepted".format(len(accepted)))
        if rejected:
            labels.append("{} rejected".format(len(rejected)))
        if uncertain:
            labels.append("{} uncertain".format(len(uncertain)))
        lines.append("Transaction classification: " + ", ".join(labels))
    if rejected:
        summary = options.get("rejected_summary_label", "Rejected transactions")
        lines.append(summary + ": " + ", ".join(str(event.get("id", "rejected")) for event in rejected))
    lines.extend(semantic_annotations(waveform.get("analysis_kind"), waveform))
    boundary = options.get("boundary_label")
    if not boundary:
        strobe = next((lane["label"] for lane in lanes(config) if lane["role"] == "strobe"), "transaction")
        boundary = strobe
    return lines, [
        "green = accepted {} boundary; orange = rejected; yellow = uncertain".format(boundary),
        "No external marker exists for the exact internal PIO sample clock.",
    ]


def title(report):
    manifest = report_manifest(report)
    return "{} — {}".format(report.get("experiment", manifest.get("id", "experiment")),
                              manifest.get("title", "waveform evidence"))


def svg_header(width, height, heading):
    return [
        '<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" viewBox="0 0 {} {}">'.format(width, height, width, height),
        '<style>text{font-family:monospace;font-size:14px;fill:#202124}.small{font-size:12px}.tiny{font-size:10px}.label{font-weight:bold}.overview{fill:#f1f3f4;stroke:#9aa0a6}.overview-event{fill:#f9ab00}.active{fill:#fde293;fill-opacity:.48}.idle-window{fill:#d9f2df;fill-opacity:.55}.data-lane{fill:#f8fbff}.as-lane{fill:#fff8f8}.control-lane{fill:#f7fbf7}.data{stroke:#1967d2;stroke-width:1.7;fill:none}.as{stroke:#b00020;stroke-width:2;fill:none}.control{stroke:#6f42c1;stroke-width:1.7;fill:none}.capture{stroke:#188038;stroke-width:2}.ignored{stroke:#f29900;stroke-width:2;stroke-dasharray:4 3}.uncertain{stroke:#f9ab00;stroke-width:2;stroke-dasharray:2 3}.phase{fill:#e8f0fe;stroke:#1a73e8}.capture-phase{fill:#e6f4ea;stroke:#188038}.note{fill:#f1f3f4;stroke:#9aa0a6}.table{fill:#f8f9fa;stroke:#9aa0a6}.table-head{fill:#e8eaed}.table-key{font-weight:bold}</style>',
        '<rect width="100%" height="100%" fill="white"/>',
        '<text x="20" y="25" class="label">{}</text>'.format(html.escape(heading)),
        '<text x="20" y="48" class="small">Transaction boundaries, sampling values and classifications come from experiment.py. Detailed traces are saved analyzer samples.</text>',
    ]


def lane_classes(lane):
    if lane["role"] == "strobe":
        return "as-lane", "as"
    if lane["role"] == "data":
        return "data-lane", "data"
    return "control-lane", "control"


def lane_mapping(all_lanes):
    """Show the manifest's logical-name to analyzer-channel correspondence."""
    mapped = ["{}←{}".format(lane["label"], lane["channel"])
              for lane in all_lanes if lane.get("channel") is not None]
    return "Analyzer mapping: " + ", ".join(mapped) if mapped else None


def coverage_summary(waveform):
    coverage = waveform.get("analyzer_coverage") or {}
    summary = coverage.get("summary") if isinstance(coverage, dict) else None
    return "Analyzer coverage: " + str(summary) if summary else None


def wrap_text(text, columns):
    """Wrap display prose without splitting a word or changing report data."""
    words, lines, current = str(text).split(), [], []
    length = 0
    for word in words:
        proposed = length + (1 if current else 0) + len(word)
        if current and proposed > columns:
            lines.append(" ".join(current))
            current, length = [word], len(word)
        else:
            current.append(word)
            length = proposed
    if current:
        lines.append(" ".join(current))
    return lines or [""]


def table_rows(lines, value_columns=38):
    """Turn report text into wrapped key/value display rows."""
    rows = []
    for line in lines:
        key, separator, detail = str(line).partition(": ")
        if not separator:
            rows.extend([(None, chunk) for chunk in wrap_text(key, value_columns + 16)])
            continue
        chunks = wrap_text(detail, value_columns)
        rows.append((key, chunks[0]))
        rows.extend([(None, chunk) for chunk in chunks[1:]])
    return rows


def draw_table(out, x, y, width, title_text, lines):
    """Draw a compact two-column table and return its bottom y coordinate."""
    rows = table_rows(lines)
    row_height, header_height = 18, 26
    height = header_height + row_height * len(rows) + 8
    out.extend([
        '<rect class="table" x="{}" y="{}" width="{}" height="{}" rx="4"/>'.format(x, y, width, height),
        '<rect class="table-head" x="{}" y="{}" width="{}" height="{}" rx="4"/>'.format(x, y, width, header_height),
        '<text x="{}" y="{}" class="label">{}</text>'.format(x + 10, y + 18, html.escape(title_text)),
    ])
    key_x, value_x = x + 10, x + 210
    for index, (key, detail) in enumerate(rows):
        row_y = y + header_height + 15 + index * row_height
        if key:
            out.append('<text x="{}" y="{}" class="small table-key">{}</text>'.format(key_x, row_y, html.escape(key + ":")))
        out.append('<text x="{}" y="{}" class="small">{}</text>'.format(value_x if key else value_x, row_y, html.escape(detail)))
    return y + height


def table_height(lines):
    return 26 + 18 * len(table_rows(lines)) + 8


def draw_note(out, x, y, width, lines, columns=145):
    wrapped = []
    for line in lines:
        wrapped.extend(wrap_text(line, columns))
    height = 10 + 17 * len(wrapped)
    out.append('<rect class="note" x="{}" y="{}" width="{}" height="{}" rx="4"/>'.format(x, y, width, height))
    for index, line in enumerate(wrapped):
        out.append('<text x="{}" y="{}" class="small">{}</text>'.format(x + 10, y + 21 + index * 17, html.escape(line)))
    return y + height


def write_transactions_svg(report, path):
    waveform = report.get("waveform") or {}
    config = view_config(report, waveform)
    events = transactions(waveform)
    if not events:
        raise ValueError("report has no analysed assertion events")
    all_lanes = lanes(config)
    first = min(event["assert_sample"] for event in events)
    last = max(event["release_sample"] for event in events)
    samples = capture_samples(waveform.get("capture"))
    sample_rate = float(waveform.get("sample_rate") or 1_000_000)
    timeline_start, timeline_end = visual_window(first, last, len(samples) if samples else None)
    detail_padding = max(100, (last - first) // 8)
    detail_start = max(timeline_start, first - detail_padding)
    detail_end = min(timeline_end, last + detail_padding)
    detail_end = max(detail_start + 1, detail_end)
    timeline_span, detail_span = timeline_end - timeline_start, detail_end - detail_start

    width, left, plot_width = 1200, 100, 1080
    overview_top, overview_height = 82, 24
    detail_top, lane_height = 194, 42
    detail_bottom = detail_top + lane_height * len(all_lanes)

    def overview_x(sample):
        return left + (sample - timeline_start) * plot_width / timeline_span

    def detail_x(sample):
        return left + (sample - detail_start) * plot_width / detail_span

    annotations, legend = transaction_annotations(report, waveform, config, events)
    provenance = []
    mapping = lane_mapping(all_lanes)
    if mapping:
        provenance.append(mapping)
    coverage = coverage_summary(waveform)
    if coverage:
        provenance.insert(0, coverage)
    options = transaction_options(config)
    if options.get("value_format") == "hex":
        legend.insert(0, "Transaction values are hexadecimal.")
    table_top = detail_bottom + 62
    left_width, table_gap = 524, 16
    right_x, right_width = left + left_width + table_gap, plot_width - left_width - table_gap
    table_bottom = table_top + max(table_height(provenance), table_height(annotations))
    note_lines = legend + ([waveform["limits"]] if waveform.get("limits") else [])
    note_height = 10 + 17 * sum(len(wrap_text(line, 145)) for line in note_lines)
    height = table_bottom + 14 + note_height + 10
    out = svg_header(width, height, title(report))
    out.extend([
        '<text x="20" y="76" class="label">detected transaction window</text>',
        '<rect class="overview" x="{}" y="{}" width="{}" height="{}"/>'.format(left, overview_top, plot_width, overview_height),
        '<rect class="overview-event" x="{:.2f}" y="{}" width="{:.2f}" height="{}"/>'.format(overview_x(first), overview_top + 2, max(2, overview_x(last) - overview_x(first)), overview_height - 4),
        '<text x="{}" y="{}" class="small">{}</text>'.format(left, overview_top + 43, time_text(timeline_start, sample_rate)),
        '<text x="{}" y="{}" class="small" text-anchor="end">{}</text>'.format(left + plot_width, overview_top + 43, time_text(timeline_end, sample_rate)),
        '<text x="{}" y="{}" class="small">events {}–{}</text>'.format(left, overview_top + 62, time_text(first, sample_rate), time_text(last, sample_rate)),
        '<text x="20" y="{}" class="label">detail: {}–{} ({} window)</text>'.format(detail_top - 24, time_text(detail_start, sample_rate), time_text(detail_end, sample_rate), time_text(detail_span, sample_rate)),
        '<rect class="active" x="{:.2f}" y="{}" width="{:.2f}" height="{}"/>'.format(detail_x(first), detail_top - 8, max(1, detail_x(last) - detail_x(first)), lane_height * len(all_lanes) + 16),
    ])
    for index, lane in enumerate(all_lanes):
        top = detail_top + index * lane_height
        lane_class, trace_class = lane_classes(lane)
        out.extend([
            '<rect class="{}" data-channel="{}" data-polarity="{}" x="{}" y="{}" width="{}" height="{}"/>'.format(lane_class, html.escape(str(lane.get("channel", ""))), html.escape(lane["polarity"]), left, top, plot_width, lane_height),
            '<text x="{}" y="{}" class="label" text-anchor="end">{}</text>'.format(left - 14, top + 26, html.escape(lane["label"])),
        ])
        trace = transition_path(samples, detail_start, detail_end, lane["sample_bit"], detail_x, top + 7, top + 31)
        if trace:
            out.append('<path class="{}" d="{}"/>'.format(trace_class, trace))

    sampling_window = waveform.get("analysis_kind") == "sampling_window"
    simple_transactions = all(len(event.get("phases") or []) == 1 for event in events)
    if sampling_window:
        out.append('<text x="{}" y="{}" class="small">{}</text>'.format(left, detail_bottom + 37, html.escape(transaction_options(config).get("sample_label", "captured at /AS fall"))))
    for index, event in enumerate(events):
        fall, rise = event["assert_sample"], event["release_sample"]
        state = event_state(event)
        class_name = {"accepted": "capture", "rejected": "ignored", "uncertain": "uncertain"}[state]
        out.append('<line class="{}" x1="{:.2f}" y1="{}" x2="{:.2f}" y2="{}"/>'.format(class_name, detail_x(fall), detail_top - 14, detail_x(fall), detail_bottom + 8))
        if sampling_window:
            prior = events[index - 1]["assert_sample"] if index else detail_start
            following = events[index + 1]["assert_sample"] if index + 1 < len(events) else detail_end
            center = detail_x(fall)
            left_bound = detail_x((prior + fall) / 2) + 2
            right_bound = detail_x((fall + following) / 2) - 2
            cell_width = min(48, right_bound - left_bound)
            cell_left = min(max(center - cell_width / 2, left_bound), right_bound - cell_width)
            out.append('<rect class="capture-phase" x="{:.2f}" y="{}" width="{:.2f}" height="20"/>'.format(cell_left, detail_bottom + 22, max(1, cell_width)))
            out.append('<text x="{:.2f}" y="{}" class="small" text-anchor="middle">{}</text>'.format(cell_left + cell_width / 2, detail_bottom + 37, value(event.get("capture_value"))))
            continue
        if simple_transactions:
            end = (events[index + 1]["assert_sample"] if index + 1 < len(events)
                   else min(detail_end, fall + (fall - events[index - 1]["assert_sample"]) if index else rise))
            cell_width = detail_x(end) - detail_x(fall)
            out.append('<rect class="phase" x="{:.2f}" y="{}" width="{:.2f}" height="20"/>'.format(detail_x(fall), detail_bottom + 22, max(1, cell_width)))
            if cell_width >= 18:
                label = (options.get("rejected_label", "rejected")
                         if state == "rejected" else
                         options.get("uncertain_label", "uncertain")
                         if state == "uncertain" else value(event.get("capture_value")))
                if state == "accepted":
                    label = transaction_value(event.get("capture_value"), options)
                out.append('<text x="{:.2f}" y="{}" class="tiny" text-anchor="middle">{}</text>'.format(detail_x(fall) + cell_width / 2, detail_bottom + 36, html.escape(label)))
            continue
        for phase in event.get("phases") or []:
            start, end = phase["start_sample"], phase["end_sample"]
            phase_width = detail_x(end) - detail_x(start)
            out.append('<rect class="phase" x="{:.2f}" y="{}" width="{:.2f}" height="20"/>'.format(detail_x(start), detail_bottom + 22, max(1, phase_width)))
            if phase_width >= 28:
                out.append('<text x="{:.2f}" y="{}" class="small">{}</text>'.format(detail_x(start) + 3, detail_bottom + 37, value(phase.get("value"))))
    draw_table(out, left, table_top, left_width, "Observed analyzer coverage", provenance)
    draw_table(out, right_x, table_top, right_width, "Transaction evidence", annotations)
    draw_note(out, 20, table_bottom + 14, 1160, note_lines)
    out.append('</svg>')
    Path(path).write_text("\n".join(out) + "\n")


def write_idle_svg(report, path):
    """Idle is the one distinct layout: values change without transactions."""
    waveform = report.get("waveform") or {}
    config = view_config(report, waveform)
    all_lanes = lanes(config)
    rows = waveform.get("measurements") or []
    if not rows:
        raise ValueError("idle report has no analysed data measurements")
    first = rows[1]["start_sample"] if len(rows) > 1 else rows[0]["start_sample"]
    last = rows[-1]["end_sample"]
    samples = capture_samples(waveform.get("capture"))
    sample_rate = float(waveform.get("sample_rate") or 1_000_000)
    timeline_start, timeline_end = visual_window(first, last, len(samples) if samples else None)
    detail_start, detail_end = timeline_start, timeline_end
    timeline_span = timeline_end - timeline_start
    width, left, plot_width = 1200, 150, 1000
    overview_top, overview_height = 82, 24
    detail_top, lane_height = 194, 42
    detail_bottom = detail_top + lane_height * len(all_lanes)

    def x(sample):
        return left + (sample - timeline_start) * plot_width / timeline_span

    dut = (report.get("dut_evidence") or {}).get("observed", {})
    counter_text = "DUT counters: capture_count={}; capture_irq_count={}".format(
        dut.get("capture_count", "not recorded") if isinstance(dut, dict) else "not recorded",
        dut.get("capture_irq_count", "not recorded") if isinstance(dut, dict) else "not recorded")
    annotations = sequence_lines("Decoded data while /AS is high: ", [row.get("value") for row in rows], 16)
    mapping = lane_mapping(all_lanes)
    if mapping:
        annotations.append(mapping)
    coverage = coverage_summary(waveform)
    if coverage:
        annotations.append(coverage)
    strobe_label = next((lane["label"] for lane in all_lanes if lane["role"] == "strobe"), "strobe")
    annotations.extend(["{}: high throughout the saved capture; no active assertion was observed.".format(strobe_label), counter_text])
    annotation_top = detail_bottom + 68
    note_top = annotation_top + 19 * len(annotations) + 12
    height = note_top + (42 if waveform.get("limits") else 10)
    out = svg_header(width, height, title(report))
    out.extend([
        '<text x="20" y="76" class="label">detected idle sequence</text>',
        '<rect class="overview" x="{}" y="{}" width="{}" height="{}"/>'.format(left, overview_top, plot_width, overview_height),
        '<rect class="overview-event" x="{:.2f}" y="{}" width="{:.2f}" height="{}"/>'.format(x(first), overview_top + 2, max(2, x(last) - x(first)), overview_height - 4),
        '<text x="{}" y="{}" class="small">{}</text>'.format(left, overview_top + 43, time_text(timeline_start, sample_rate)),
        '<text x="{}" y="{}" class="small" text-anchor="end">{}</text>'.format(left + plot_width, overview_top + 43, time_text(timeline_end, sample_rate)),
        '<text x="{}" y="{}" class="small">data-transition sequence {}–{}; {} remains high</text>'.format(left, overview_top + 62, time_text(first, sample_rate), time_text(last, sample_rate), strobe_label),
        '<text x="20" y="{}" class="label">detail: {}–{} ({} window)</text>'.format(detail_top - 24, time_text(detail_start, sample_rate), time_text(detail_end, sample_rate), time_text(timeline_span, sample_rate)),
        '<rect class="idle-window" x="{:.2f}" y="{}" width="{:.2f}" height="{}"/>'.format(x(first), detail_top - 8, max(1, x(last) - x(first)), lane_height * len(all_lanes) + 16),
    ])
    for index, lane in enumerate(all_lanes):
        top = detail_top + index * lane_height
        lane_class, trace_class = lane_classes(lane)
        out.extend([
            '<rect class="{}" data-channel="{}" data-polarity="{}" x="{}" y="{}" width="{}" height="{}"/>'.format(lane_class, html.escape(str(lane.get("channel", ""))), html.escape(lane["polarity"]), left, top, plot_width, lane_height),
            '<text x="{}" y="{}" class="label" text-anchor="end">{}</text>'.format(left - 14, top + 26, html.escape(lane["label"])),
        ])
        trace = transition_path(samples, detail_start, detail_end, lane["sample_bit"], x, top + 7, top + 31)
        if trace:
            out.append('<path class="{}" d="{}"/>'.format(trace_class, trace))
    for row in rows:
        start, end = max(row["start_sample"], detail_start), min(row["end_sample"], detail_end)
        if start < end:
            phase_width = x(end) - x(start)
            out.append('<rect class="phase" x="{:.2f}" y="{}" width="{:.2f}" height="20"/>'.format(x(start), detail_bottom + 22, max(1, phase_width)))
            if phase_width >= 24:
                out.append('<text x="{:.2f}" y="{}" class="small">{}</text>'.format(x(start) + 3, detail_bottom + 37, value(row.get("value"))))
    for index, line in enumerate(annotations):
        out.append('<text x="{}" y="{}" class="small">{}</text>'.format(left, annotation_top + index * 19, html.escape(line)))
    limits = waveform.get("limits")
    if limits:
        if (report.get("dut_evidence") or {}).get("status") == "observed":
            limits = limits.replace(" No DUT capture count or IRQ was observed.", "")
        out.append('<rect class="note" x="20" y="{}" width="1160" height="32" rx="4"/>'.format(note_top))
        out.append('<text x="30" y="{}" class="small">{}</text>'.format(note_top + 21, html.escape(limits)))
    out.append('</svg>')
    Path(path).write_text("\n".join(out) + "\n")


def write_svg(report, path):
    """Write the semantic layout selected by analysis capability, never ID."""
    if (report.get("waveform") or {}).get("analysis_kind") == "idle":
        write_idle_svg(report, path)
    else:
        write_transactions_svg(report, path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        report = json.loads(args.report.read_text())
        output = args.output or args.report.with_name("waveform.svg")
        write_svg(report, output)
        print(output)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
