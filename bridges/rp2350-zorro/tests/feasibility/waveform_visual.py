#!/usr/bin/env python3
"""Render read-only visual evidence from an analysed feasibility report.

This is deliberately a presentation tool.  ``experiment.py`` remains the
authoritative analyser: this module uses its transaction boundaries and never
decides whether an experiment passed.  When the saved sigrok capture is
available, it draws the original digital samples around those boundaries.
"""

import argparse
import html
import json
from pathlib import Path
import re
import zipfile


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
    """Return sigrok logic bytes for drawing, or ``None`` if unavailable.

    Sigrok stores its capture in consecutively numbered ``logic-1-N`` zip
    members. The report's analysed boundaries remain the source of meaning;
    these bytes only make the existing evidence easier to inspect.
    """
    if not capture_path:
        return None
    try:
        with zipfile.ZipFile(capture_path) as archive:
            members = []
            for name in archive.namelist():
                matched = re.fullmatch(r"logic-1-(\d+)", name)
                if matched:
                    members.append((int(matched.group(1)), name))
            if not members:
                return None
            return b"".join(archive.read(name) for _, name in sorted(members))
    except (OSError, zipfile.BadZipFile, KeyError):
        return None


def time_text(sample, sample_rate):
    seconds = sample / sample_rate
    if seconds < 0.001:
        return "{:.0f} us".format(seconds * 1_000_000)
    if seconds < 1:
        return "{:.3f} ms".format(seconds * 1_000)
    return "{:.3f} s".format(seconds)


def transition_path(samples, start, end, bit, x, high_y, low_y):
    """Make an SVG step trace for one captured digital bit."""
    if not samples or start >= len(samples):
        return None
    end = min(end, len(samples))
    initial = bool(samples[start] & (1 << bit))
    y = high_y if initial else low_y
    pieces = ["M {:.2f} {:.2f}".format(x(start), y)]
    for sample in range(start + 1, end):
        state = bool(samples[sample] & (1 << bit))
        next_y = high_y if state else low_y
        if next_y != y:
            pieces.append("H {:.2f} V {:.2f}".format(x(sample), next_y))
            y = next_y
    pieces.append("H {:.2f}".format(x(end)))
    return " ".join(pieces)


def sequence_lines(label, numbers, per_line=14):
    """Wrap a decoded value sequence without deriving a verdict from it."""
    rendered = [value(number) for number in numbers]
    if not rendered:
        return []
    lines = []
    for offset in range(0, len(rendered), per_line):
        prefix = label if offset == 0 else " " * len(label)
        lines.append(prefix + ", ".join(rendered[offset:offset + per_line]))
    return lines


def write_transactions_svg(report, path):
    waveform = report.get("waveform") or {}
    events = transactions(waveform)
    if not events:
        raise ValueError("report has no analysed assertion events")

    first = min(event["assert_sample"] for event in events)
    last = max(event["release_sample"] for event in events)
    span = max(1, last - first)
    samples = capture_samples(waveform.get("capture"))
    sample_rate = float(waveform.get("sample_rate") or 1_000_000)
    capture_length = len(samples) if samples else last
    detail_padding = max(100, span // 5)
    detail_start = max(0, first - detail_padding)
    detail_end = min(capture_length, last + detail_padding)
    detail_span = max(1, detail_end - detail_start)

    width, left, plot_width = 1200, 150, 1000
    overview_top, overview_height = 82, 24
    detail_top, lane_height = 194, 42
    lane_names = [(3, "D3"), (2, "D2"), (1, "D1"), (0, "D0"), (7, "/AS")]
    detail_bottom = detail_top + lane_height * len(lane_names)

    def overview_x(sample):
        return left + sample * plot_width / max(1, capture_length)

    def detail_x(sample):
        return left + (sample - detail_start) * plot_width / detail_span

    dut = (report.get("dut_evidence") or {}).get("observed", {})
    observed = dut.get("values", []) if isinstance(dut, dict) else []
    expected = [event.get("capture_value") for event in events]
    annotation_lines = sequence_lines("Expected at /AS falls: ", expected)
    annotation_lines += sequence_lines("DUT reported: ", observed)
    if waveform.get("analysis_kind") == "sampling_window":
        for row in waveform.get("measurements", []):
            if not isinstance(row, dict):
                continue
            annotation_lines.append(
                "{}: data {} /AS by {} us; captured {}".format(
                    row.get("case", "case"), row.get("relation", "at"),
                    row.get("offset_us", "?"), value(row.get("captured"))
                )
            )
    annotation_lines.append(
        "green = /AS fall / transaction boundary; no external marker exists for the exact internal PIO sample clock"
    )
    annotation_top = detail_bottom + 68
    note_top = annotation_top + 19 * len(annotation_lines) + 12
    height = note_top + (42 if waveform.get("limits") else 10)
    title = "{} — {}".format(
        report.get("experiment", "experiment"),
        (report.get("build_identity") or {}).get("manifest", {}).get("title", "waveform evidence"))
    out = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" viewBox="0 0 {} {}">'.format(width, height, width, height),
        '<style>text{font-family:monospace;font-size:14px;fill:#202124}.small{font-size:12px}.label{font-weight:bold}.overview{fill:#f1f3f4;stroke:#9aa0a6}.overview-event{fill:#f9ab00}.active{fill:#fde293;fill-opacity:.48}.data-lane{fill:#f8fbff}.as-lane{fill:#fff8f8}.data{stroke:#1967d2;stroke-width:1.7;fill:none}.as{stroke:#b00020;stroke-width:2;fill:none}.capture{stroke:#188038;stroke-width:2}.phase{fill:#e8f0fe;stroke:#1a73e8}.capture-phase{fill:#e6f4ea;stroke:#188038}.note{fill:#f1f3f4;stroke:#9aa0a6}</style>',
        '<rect width="100%" height="100%" fill="white"/>',
        '<text x="20" y="25" class="label">{}</text>'.format(html.escape(title)),
        '<text x="20" y="48" class="small">Authoritative event boundaries and decoded values come from experiment.py. The detailed traces below are saved logic-analyser samples.</text>',
        '<text x="20" y="76" class="label">whole acquisition</text>',
        '<rect class="overview" x="{}" y="{}" width="{}" height="{}"/>'.format(left, overview_top, plot_width, overview_height),
    ]

    event_left = overview_x(first)
    event_right = overview_x(last)
    out.extend([
        '<rect class="overview-event" x="{:.2f}" y="{}" width="{:.2f}" height="{}"/>'.format(event_left, overview_top + 2, max(2, event_right - event_left), overview_height - 4),
        '<text x="{}" y="{}" class="small">0</text>'.format(left, overview_top + 43),
        '<text x="{}" y="{}" class="small" text-anchor="end">{}</text>'.format(left + plot_width, overview_top + 43, time_text(capture_length, sample_rate)),
        '<text x="{}" y="{}" class="small">event {}–{}</text>'.format(left, overview_top + 62, time_text(first, sample_rate), time_text(last, sample_rate)),
        '<text x="20" y="{}" class="label">detail: {}–{} ({} window)</text>'.format(detail_top - 24, time_text(detail_start, sample_rate), time_text(detail_end, sample_rate), time_text(detail_span, sample_rate)),
    ])

    # Fill precedes all digital paths so the real red /AS trace stays visible.
    out.append('<rect class="active" x="{:.2f}" y="{}" width="{:.2f}" height="{}"/>'.format(
        detail_x(first), detail_top - 8, max(1, detail_x(last) - detail_x(first)), lane_height * len(lane_names) + 16))
    for lane, (bit, name) in enumerate(lane_names):
        top = detail_top + lane * lane_height
        path_class = "as" if bit == 7 else "data"
        lane_class = "as-lane" if bit == 7 else "data-lane"
        out.extend([
            '<rect class="{}" x="{}" y="{}" width="{}" height="{}"/>'.format(lane_class, left, top, plot_width, lane_height),
            '<text x="{}" y="{}" class="label" text-anchor="end">{}</text>'.format(left - 14, top + 26, name),
        ])
        trace = transition_path(samples, detail_start, detail_end, bit, detail_x, top + 7, top + 31)
        if trace:
            out.append('<path class="{}" d="{}"/>'.format(path_class, trace))

    sampling_window = waveform.get("analysis_kind") == "sampling_window"
    simple_transactions = all(len(event.get("phases") or []) == 1 for event in events)
    if sampling_window:
        out.append('<text x="20" y="{}" class="small">captured at /AS fall</text>'.format(
            detail_bottom + 37))
    for index, event in enumerate(events):
        fall, rise = event["assert_sample"], event["release_sample"]
        out.append('<line class="capture" x1="{:.2f}" y1="{}" x2="{:.2f}" y2="{}"/>'.format(
            detail_x(fall), detail_top - 14, detail_x(fall), detail_bottom + 8))
        if sampling_window:
            # C3 can change data while /AS is low. The raw phase cells would
            # describe that later transition, not necessarily the value sampled
            # at the falling edge. Label that captured value independently and
            # give even a 10-us event enough room for its complete hex label.
            prior = events[index - 1]["assert_sample"] if index else detail_start
            following = events[index + 1]["assert_sample"] if index + 1 < len(events) else detail_end
            center = detail_x(fall)
            left_bound = detail_x((prior + fall) / 2) + 2
            right_bound = detail_x((fall + following) / 2) - 2
            cell_width = min(48, right_bound - left_bound)
            cell_left = min(max(center - cell_width / 2, left_bound), right_bound - cell_width)
            out.append('<rect class="capture-phase" x="{:.2f}" y="{}" width="{:.2f}" height="20"/>'.format(
                cell_left, detail_bottom + 22, max(1, cell_width)))
            out.append('<text x="{:.2f}" y="{}" class="small" text-anchor="middle">{}</text>'.format(
                cell_left + cell_width / 2, detail_bottom + 37, value(event.get("capture_value"))))
            continue
        if simple_transactions:
            # A pulse is narrower than its setup/released interval. Show the
            # captured value across the interval leading to the next /AS fall
            # so C1's compact, repeated transactions remain readable.
            if index + 1 < len(events):
                end = events[index + 1]["assert_sample"]
            elif index:
                end = min(detail_end, fall + (fall - events[index - 1]["assert_sample"]))
            else:
                end = rise
            cell_width = detail_x(end) - detail_x(fall)
            out.append('<rect class="phase" x="{:.2f}" y="{}" width="{:.2f}" height="20"/>'.format(detail_x(fall), detail_bottom + 22, max(1, cell_width)))
            if cell_width >= 18:
                out.append('<text x="{:.2f}" y="{}" class="small">{}</text>'.format(detail_x(fall) + 3, detail_bottom + 37, value(event.get("capture_value"))))
            continue
        for phase in event.get("phases") or []:
            start, end = phase["start_sample"], phase["end_sample"]
            phase_width = detail_x(end) - detail_x(start)
            out.append('<rect class="phase" x="{:.2f}" y="{}" width="{:.2f}" height="20"/>'.format(detail_x(start), detail_bottom + 22, max(1, phase_width)))
            if phase_width >= 28:
                out.append('<text x="{:.2f}" y="{}" class="small">{}</text>'.format(detail_x(start) + 3, detail_bottom + 37, value(phase.get("value"))))

    for index, line in enumerate(annotation_lines):
        out.append('<text x="{}" y="{}" class="small">{}</text>'.format(
            left, annotation_top + index * 19, html.escape(line)))
    limits = waveform.get("limits")
    if limits:
        out.append('<rect class="note" x="20" y="{}" width="1160" height="32" rx="4"/>'.format(note_top))
        out.append('<text x="30" y="{}" class="small">{}</text>'.format(note_top + 21, html.escape(limits)))
    out.append('</svg>')
    Path(path).write_text("\n".join(out) + "\n")


def write_idle_svg(report, path):
    """Render an idle waveform: data changes, while /AS must remain high."""
    waveform = report.get("waveform") or {}
    rows = waveform.get("measurements") or []
    if not rows:
        raise ValueError("idle report has no analysed data measurements")

    # The first value may have existed before the analyser armed. The first
    # independently timed transition therefore starts the useful zoom window.
    first = rows[1]["start_sample"] if len(rows) > 1 else rows[0]["start_sample"]
    last = rows[-1]["end_sample"]
    span = max(1, last - first)
    samples = capture_samples(waveform.get("capture"))
    sample_rate = float(waveform.get("sample_rate") or 1_000_000)
    capture_length = len(samples) if samples else last
    detail_padding = max(100, span // 5)
    detail_start = max(0, first - detail_padding)
    detail_end = min(capture_length, last + detail_padding)
    detail_span = max(1, detail_end - detail_start)

    width, left, plot_width = 1200, 150, 1000
    overview_top, overview_height = 82, 24
    detail_top, lane_height = 194, 42
    lane_names = [(3, "D3"), (2, "D2"), (1, "D1"), (0, "D0"), (7, "/AS")]
    detail_bottom = detail_top + lane_height * len(lane_names)
    dut = (report.get("dut_evidence") or {}).get("observed", {})
    dut = dut if isinstance(dut, dict) else {}
    counter_text = "DUT counters: capture_count={}; capture_irq_count={}".format(
        dut.get("capture_count", "not recorded"),
        dut.get("capture_irq_count", "not recorded"),
    )
    values = [row.get("value") for row in rows]
    annotations = sequence_lines("Decoded data while /AS is high: ", values, 16)
    annotations.extend([
        "/AS: high throughout the saved capture; no falling edge was observed.",
        counter_text,
    ])
    annotation_top = detail_bottom + 68
    note_top = annotation_top + 19 * len(annotations) + 12
    height = note_top + (42 if waveform.get("limits") else 10)

    def overview_x(sample):
        return left + sample * plot_width / max(1, capture_length)

    def detail_x(sample):
        return left + (sample - detail_start) * plot_width / detail_span

    title = "{} — {}".format(
        report.get("experiment", "experiment"),
        (report.get("build_identity") or {}).get("manifest", {}).get("title", "waveform evidence"))
    out = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" viewBox="0 0 {} {}">'.format(width, height, width, height),
        '<style>text{font-family:monospace;font-size:14px;fill:#202124}.small{font-size:12px}.label{font-weight:bold}.overview{fill:#f1f3f4;stroke:#9aa0a6}.overview-event{fill:#f9ab00}.idle-window{fill:#d9f2df;fill-opacity:.55}.data-lane{fill:#f8fbff}.as-lane{fill:#fff8f8}.data{stroke:#1967d2;stroke-width:1.7;fill:none}.as{stroke:#b00020;stroke-width:2;fill:none}.phase{fill:#e8f0fe;stroke:#1a73e8}.note{fill:#f1f3f4;stroke:#9aa0a6}</style>',
        '<rect width="100%" height="100%" fill="white"/>',
        '<text x="20" y="25" class="label">{}</text>'.format(html.escape(title)),
        '<text x="20" y="48" class="small">Authoritative decoded values come from experiment.py. The detailed traces below are saved logic-analyser samples.</text>',
        '<text x="20" y="76" class="label">whole acquisition</text>',
        '<rect class="overview" x="{}" y="{}" width="{}" height="{}"/>'.format(left, overview_top, plot_width, overview_height),
        '<rect class="overview-event" x="{:.2f}" y="{}" width="{:.2f}" height="{}"/>'.format(overview_x(first), overview_top + 2, max(2, overview_x(last) - overview_x(first)), overview_height - 4),
        '<text x="{}" y="{}" class="small">0</text>'.format(left, overview_top + 43),
        '<text x="{}" y="{}" class="small" text-anchor="end">{}</text>'.format(left + plot_width, overview_top + 43, time_text(capture_length, sample_rate)),
        '<text x="{}" y="{}" class="small">data-transition sequence {}–{}; /AS remains high</text>'.format(left, overview_top + 62, time_text(first, sample_rate), time_text(last, sample_rate)),
        '<text x="20" y="{}" class="label">detail: {}–{} ({} window)</text>'.format(detail_top - 24, time_text(detail_start, sample_rate), time_text(detail_end, sample_rate), time_text(detail_span, sample_rate)),
        '<rect class="idle-window" x="{:.2f}" y="{}" width="{:.2f}" height="{}"/>'.format(detail_x(first), detail_top - 8, max(1, detail_x(last) - detail_x(first)), lane_height * len(lane_names) + 16),
    ]
    for lane, (bit, name) in enumerate(lane_names):
        top = detail_top + lane * lane_height
        path_class = "as" if bit == 7 else "data"
        lane_class = "as-lane" if bit == 7 else "data-lane"
        out.extend([
            '<rect class="{}" x="{}" y="{}" width="{}" height="{}"/>'.format(lane_class, left, top, plot_width, lane_height),
            '<text x="{}" y="{}" class="label" text-anchor="end">{}</text>'.format(left - 14, top + 26, name),
        ])
        trace = transition_path(samples, detail_start, detail_end, bit, detail_x, top + 7, top + 31)
        if trace:
            out.append('<path class="{}" d="{}"/>'.format(path_class, trace))

    for row in rows:
        start, end = row["start_sample"], row["end_sample"]
        clipped_start, clipped_end = max(start, detail_start), min(end, detail_end)
        if clipped_start >= clipped_end:
            continue
        phase_width = detail_x(clipped_end) - detail_x(clipped_start)
        out.append('<rect class="phase" x="{:.2f}" y="{}" width="{:.2f}" height="20"/>'.format(detail_x(clipped_start), detail_bottom + 22, max(1, phase_width)))
        if phase_width >= 24:
            out.append('<text x="{:.2f}" y="{}" class="small">{}</text>'.format(detail_x(clipped_start) + 3, detail_bottom + 37, value(row.get("value"))))
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
    """Write the display selected by the report's analysis capability."""
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
