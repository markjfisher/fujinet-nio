#!/usr/bin/env python3
"""Render read-only SPI-link evidence from an L0 report and Sigrok capture.

This renderer never decides whether an experiment passed.  ``link_experiment``
owns that decision; this module only makes its captured six-channel evidence
inspectable.
"""
import configparser
import argparse
import html
import json
import re
import struct
import zipfile
from pathlib import Path

MAGIC = 0x4C4E4B31
SIGNALS = ("SCLK", "MOSI", "MISO", "CS", "READY", "DATA_AVAILABLE")


def capture_words(path):
    try:
        with zipfile.ZipFile(path) as archive:
            meta = configparser.ConfigParser()
            meta.read_string(archive.read("metadata").decode())
            unit = int(meta.get("device 1", "unitsize", fallback="1"))
            parts = sorted((name for name in archive.namelist()
                            if re.fullmatch(r"logic-1-\d+", name)),
                           key=lambda name: int(name.rsplit("-", 1)[1]))
            raw = b"".join(archive.read(name) for name in parts)
        return [int.from_bytes(raw[index:index + unit], "little")
                for index in range(0, len(raw), unit)]
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, configparser.Error):
        return []


def frame_text(data):
    if len(data) < 16 or struct.unpack_from("<I", data)[0] != MAGIC:
        return "zero slot" if not any(data) else "unrecognised"
    version, scenario, status = data[4:7]
    sequence = struct.unpack_from("<I", data, 8)[0]
    length, checksum = struct.unpack_from("<HH", data, 12)
    return "L{} seq {} {} B status {} CRC {:04X}".format(
        scenario, sequence, length, status, checksum)


def byte_stream(words, start, end, bit):
    rising = [sample for sample in range(start + 1, end)
              if not (words[sample - 1] & 1) and words[sample] & 1]
    bits = [(words[sample] >> bit) & 1 for sample in rising]
    return bytes(sum(value << (7 - offset) for offset, value in enumerate(bits[index:index + 8]))
                 for index in range(0, len(bits) - 7, 8))


def transactions(words):
    """Return only complete active-low CS windows in capture order.

    A trigger may start acquisition after CS has already fallen. That leading
    partial transfer has no observed falling edge and must not shift all later
    fall/rise pairs. Pair edges as a small state machine instead of zipping two
    independently collected lists.
    """
    result = []
    start = None
    leading_partial = not bool(words[0] & (1 << 3))
    for sample in range(1, len(words)):
        before = bool(words[sample - 1] & (1 << 3))
        current = bool(words[sample] & (1 << 3))
        if before and not current:
            start = sample
        elif not before and current and start is not None:
            mosi = byte_stream(words, start, sample, 1)
            miso = byte_stream(words, start, sample, 2)
            mosi_label, miso_label = frame_text(mosi), frame_text(miso)
            if mosi_label.startswith("L"):
                role = "request"
            elif miso_label.startswith("L"):
                if not any(item["role"] == "request" for item in result):
                    role = "unpaired echo" if leading_partial else "drain"
                else:
                    role = "echo"
            else:
                role = "zero slot"
            result.append({"start_sample": start, "end_sample": sample, "role": role,
                           "mosi": mosi_label, "miso": miso_label})
            start = None
    return result


def ready_low_intervals(words, rate):
    """Measure complete READY-low intervals from the captured logic stream."""
    intervals = []
    start = None
    for sample in range(1, len(words)):
        before = bool(words[sample - 1] & (1 << 4))
        current = bool(words[sample] & (1 << 4))
        if before and not current:
            start = sample
        elif not before and current and start is not None:
            intervals.append({"start_sample": start, "end_sample": sample,
                              "duration_ms": (sample - start) * 1000 / rate})
            start = None
    return intervals


def annotate_timing(rows, report, words, rate):
    """Attach manifest-declared pause targets to their captured request/echo.

    The renderer presents these measured annotations only. Experiment pass/fail
    remains owned by link_experiment.py. ``READY`` also covers fixed endpoint
    processing, so injected pause is shown relative to the zero-pause baseline.
    """
    cases = report.get("round_trip_cases") or []
    requests = [row for row in rows if row["role"] == "request"]
    intervals = ready_low_intervals(words, rate)
    measurements = []
    for case, request in zip(cases, requests):
        target = case.get("pause_ms")
        if not isinstance(target, int):
            continue
        following = next((row for row in rows
                          if row["role"] in {"echo", "unpaired echo"} and
                          row["start_sample"] > request["end_sample"]), None)
        if following is None:
            continue
        matching = [interval for interval in intervals
                    if interval["start_sample"] >= request["end_sample"] and
                    interval["end_sample"] <= following["start_sample"]]
        if not matching:
            continue
        measurements.append((case, request, following,
                             max(matching, key=lambda value: value["duration_ms"])))
    baseline = next((interval["duration_ms"] for case, _request, _echo, interval
                     in measurements if case["pause_ms"] == 0), None)
    for case, request, following, interval in measurements:
        target = case["pause_ms"]
        request["timing_detail"] = "Case {}: requested receiver pause {} ms".format(
            case.get("id", "unnamed"), target)
        total = interval["duration_ms"]
        if target == 0:
            following["timing_detail"] = "READY low {:.3f} ms (baseline endpoint overhead)".format(total)
        elif baseline is None:
            following["timing_detail"] = "READY low {:.3f} ms; requested {} ms".format(total, target)
        else:
            injected = total - baseline
            following["timing_detail"] = (
                "READY low {:.3f} ms = {:.3f} ms baseline + {:.3f} ms injected; requested {} ms"
                .format(total, baseline, injected, target))




def displayed_rows(rows, maximum=None):
    """Select bounded first/last evidence rows without discarding decoded data.

    Long batch captures remain complete in ``report.json`` and the raw Sigrok
    archive.  The SVG is a human review aid, so its table and markers show the
    beginning and end of the captured run rather than hundreds of repetitions.
    """
    if not isinstance(maximum, int) or maximum <= 0 or len(rows) <= maximum:
        return list(enumerate(rows, 1)), 0
    first_count = maximum // 2
    last_count = maximum - first_count
    selected = list(enumerate(rows[:first_count], 1))
    selected.extend((len(rows) - last_count + index + 1, row)
                    for index, row in enumerate(rows[-last_count:]))
    return selected, len(rows) - len(selected)


def _path(words, start, end, bit, x, high, low):
    # A long high-rate acquisition can contain millions of transitions. At SVG
    # scale they collapse to a solid block, so show the lane's idle state and
    # retain individual decoded start/end windows in the compact evidence table.
    changes = sum(bool(words[sample - 1] & (1 << bit)) != bool(words[sample] & (1 << bit))
                  for sample in range(start + 1, end + 1))
    if changes > 1000:
        state = bool(words[start] & (1 << bit))
        return "M {:.2f} {:.2f} H {:.2f}".format(x(start), high if state else low, x(end))
    state = bool(words[start] & (1 << bit))
    y = high if state else low
    pieces = ["M {:.2f} {:.2f}".format(x(start), y)]
    last_x = None
    for sample in range(start + 1, end + 1):
        next_state = bool(words[sample] & (1 << bit))
        if next_state != state:
            at = round(x(sample), 2)
            # Keep every transition even when horizontal compression maps
            # several edges to one pixel. Omitting a same-x falling edge can
            # falsely extend the preceding high state across an idle gap.
            pieces.append("H {:.2f} V {:.2f}".format(at, high if next_state else low))
            last_x = at
            state = next_state
    pieces.append("H {:.2f}".format(x(end)))
    return " ".join(pieces)


def render(report, capture, output):
    words = capture_words(capture)
    if not words:
        raise ValueError("capture has no readable logic samples")
    rows = transactions(words)
    if not rows:
        raise ValueError("capture has no CS transaction windows")
    rate = int((report.get("analyzer") or {}).get("sample_rate_hz", 1_000_000))
    annotate_timing(rows, report, words, rate)
    first, last = rows[0]["start_sample"], rows[-1]["end_sample"]
    span = max(1, last - first)
    leading_partial = not bool(words[0] & (1 << 3))
    # Do not show the incomplete triggered slot as if it aligned with a decoded
    # transaction band. Future captures retain it in the pre-trigger buffer.
    start = first if leading_partial else max(0, first - span // 12)
    end = min(len(words) - 1, last + span // 12)
    left, right = 220, 1380
    width = right - left
    x = lambda sample: left + (sample - start) * width / max(1, end - start)
    lane_top, lane_height = 145, 64
    shown_rows, omitted_rows = displayed_rows(
        rows, (report.get("analyzer") or {}).get("svg_max_transactions"))
    table_row_height = 78 if any("timing_detail" in row for _index, row in shown_rows) else 58
    height = 720 + len(shown_rows) * (table_row_height + 12)
    purpose = str(report.get("purpose") or "SPI request and echo evidence.")
    lines = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="1440" height="{}" viewBox="0 0 1440 {}">'.format(height, height),
        '<style>text{font-family:monospace;fill:#202124}.title{font-size:18px;font-weight:bold}.small{font-size:13px}.lane{font-size:16px;font-weight:bold}.wave{fill:none;stroke:#1967d2;stroke-width:1.6}.control{fill:none;stroke:#6f42c1;stroke-width:1.6}.cs{fill:none;stroke:#b00020;stroke-width:2}.window{fill:#e8f0fe;fill-opacity:.65;stroke:#1a73e8}.box{fill:#f8f9fa;stroke:#9aa0a6}.head{fill:#e8eaed}</style>',
        '<defs><pattern id="link-drain" width="8" height="8" patternUnits="userSpaceOnUse" patternTransform="rotate(45)"><rect width="8" height="8" fill="#fff0d0"/><path d="M 0 0 V 8" stroke="#b45309" stroke-width="2"/></pattern><pattern id="link-request" width="8" height="8" patternUnits="userSpaceOnUse"><rect width="8" height="8" fill="#d9f2df"/><path d="M 3 0 V 8" stroke="#16713b" stroke-width="2"/></pattern><pattern id="link-echo" width="8" height="8" patternUnits="userSpaceOnUse"><rect width="8" height="8" fill="#e0eaff"/><path d="M 0 0 L 8 8 M 8 0 L 0 8" stroke="#2457a6" stroke-width="1.3"/></pattern></defs>',
        '<rect width="100%" height="100%" fill="white"/>',
        '<text x="30" y="34" class="title">{} — {}</text>'.format(html.escape(str(report.get("experiment", "Link"))), html.escape(str(report.get("title", "SPI link evidence")))),
        '<text x="30" y="59" class="small">{}</text>'.format(html.escape(purpose)),
        '<text x="30" y="80" class="small">Transaction windows are decoded from the captured MOSI and MISO bits; they do not determine pass/fail.</text>',
        '<text x="30" y="101" class="small">Key: D = stale-response drain; R = request; E = echoed response; E* = response to an incomplete triggered request.</text>',
    ]
    colors = {"request": "url(#link-request)", "echo": "url(#link-echo)",
              "unpaired echo": "url(#link-echo)", "drain": "url(#link-drain)",
              "zero slot": "#d5d8dc"}
    markers = {"request": "R", "echo": "E", "unpaired echo": "E*",
               "drain": "D", "zero slot": "·"}
    for capture_index, row in shown_rows:
        begin, finish = x(row["start_sample"]), x(row["end_sample"])
        lines.extend([
            '<rect x="{:.2f}" y="115" width="{:.2f}" height="{}" fill="{}"/>'.format(begin, max(1, finish - begin), lane_height * len(SIGNALS), colors[row["role"]]),
            '<text x="{:.2f}" y="121" class="small" text-anchor="middle">{}</text>'.format((begin + finish) / 2, markers[row["role"]]),
        ])
    for bit, signal in enumerate(SIGNALS):
        top = lane_top + bit * lane_height
        high, low = top + 13, top + 42
        wave_class = "cs" if signal == "CS" else ("control" if signal in ("READY", "DATA_AVAILABLE") else "wave")
        lines += [
            '<rect x="{}" y="{}" width="{}" height="{}" fill="#fafcff"/>'.format(left, top, width, lane_height - 6),
            '<text x="195" y="{}" class="lane" text-anchor="end">{} <tspan class="small">(CH{})</tspan></text>'.format(top + 33, signal, bit + 1),
            '<path class="{}" d="{}"/>'.format(wave_class, _path(words, start, end, bit, x, high, low)),
        ]
    duration_ms = (last - first) * 1000 / rate
    detail = "Detail window: {:.3f} ms; {} SPI transaction windows".format(duration_ms, len(rows))
    if omitted_rows:
        detail += "; SVG shows first/last {} ({} omitted; raw capture/report retain all)".format(
            len(shown_rows), omitted_rows)
    lines.append('<text x="770" y="548" class="small" text-anchor="middle">{}</text>'.format(html.escape(detail)))
    y = 575
    for capture_index, row in shown_rows:
        lines += [
            '<rect class="box" x="30" y="{}" width="1380" height="{}"/>'.format(y, table_row_height),
            '<rect class="head" x="30" y="{}" width="190" height="{}"/>'.format(y, table_row_height),
            '<text x="45" y="{}" class="small">Window {} · {}</text>'.format(y + 23, capture_index, html.escape(row["role"])),
            '<text x="235" y="{}" class="small">MOSI: {}</text>'.format(y + 22, html.escape(row["mosi"])),
            '<text x="235" y="{}" class="small">MISO: {}</text>'.format(y + 44, html.escape(row["miso"])),
        ]
        if "timing_detail" in row:
            lines.append('<text x="235" y="{}" class="small">{}</text>'.format(
                y + 66, html.escape(row["timing_detail"])))
        y += table_row_height + 12
    lines.append('</svg>')
    Path(output).write_text("\n".join(lines) + "\n")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    capture = Path(report["capture"])
    output = args.output or args.report.with_name("waveform.svg")
    report["waveform_svg"] = str(output)
    report["waveform_transactions"] = render(report, capture, output)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(output)


if __name__ == "__main__":
    main()
