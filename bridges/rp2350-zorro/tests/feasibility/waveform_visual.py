#!/usr/bin/env python3
"""Render a read-only SVG event map from an analysed feasibility report."""

import argparse
import html
import json
from pathlib import Path


def value(value):
    return "0x{:X}".format(value) if isinstance(value, int) else "?"


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


def write_svg(report, path):
    waveform = report.get("waveform") or {}
    events = transactions(waveform)
    if not events:
        raise ValueError("report has no analysed assertion events")
    first = min(event["assert_sample"] for event in events)
    last = max(event["release_sample"] for event in events)
    span = max(1, last - first)
    width, left, plot_width = 1200, 130, 1000
    height = 260 + 42 * len(events)

    def x(sample):
        return left + (sample - first) * plot_width / span

    dut = (report.get("dut_evidence") or {}).get("observed", {})
    observed = dut.get("values", []) if isinstance(dut, dict) else []
    title = "{} — {}".format(report.get("experiment", "experiment"),
                               (report.get("build_identity") or {}).get("manifest", {}).get("title", "waveform evidence"))
    out = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" viewBox="0 0 {} {}">'.format(width, height, width, height),
        '<style>text{font-family:monospace;font-size:14px;fill:#202124}.small{font-size:12px}.label{font-weight:bold}.as{stroke:#b00020;stroke-width:2;fill:none}.data{fill:#e8f0fe;stroke:#1a73e8}.active{fill:#fde293;fill-opacity:.7}.capture{stroke:#188038;stroke-width:2}.note{fill:#f1f3f4;stroke:#9aa0a6}</style>',
        '<text x="20" y="25" class="label">{}</text>'.format(html.escape(title)),
        '<text x="20" y="48" class="small">Shaded = /AS asserted. Green marker = capture transaction boundary; it is not an electrically observed internal PIO clock.</text>',
    ]
    for line, event in enumerate(events):
        top = 75 + line * 42
        fall, rise = event["assert_sample"], event["release_sample"]
        out.extend([
            '<text x="20" y="{}" class="label">transaction {}</text>'.format(top + 15, event.get("index", line)),
            '<text x="78" y="{}">/AS</text>'.format(top + 15),
            '<path class="as" d="M {} {} H {} V {} H {} V {} H {}"/>'.format(left, top + 10, x(fall), top + 28, x(rise), top + 10, left + plot_width),
            '<rect class="active" x="{}" y="{}" width="{}" height="26"/>'.format(x(fall), top + 2, max(1, x(rise) - x(fall))),
            '<line class="capture" x1="{}" y1="{}" x2="{}" y2="{}"/>'.format(x(fall), top - 5, x(fall), top + 35),
        ])
        phases = event.get("phases") or []
        for phase in phases:
            start, end = phase["start_sample"], phase["end_sample"]
            out.append('<rect class="data" x="{}" y="{}" width="{}" height="18"/>'.format(x(start), top + 32, max(1, x(end) - x(start))))
            if x(end) - x(start) >= 25:
                out.append('<text x="{}" y="{}" class="small">{}</text>'.format(x(start) + 3, top + 46, value(phase.get("value"))))
        capture = event.get("capture_value")
        evidence = "expected {}".format(value(capture))
        if line < len(observed):
            evidence += "; DUT reported {}".format(value(observed[line]))
        elif observed:
            evidence += "; DUT has {} value(s)".format(len(observed))
        out.append('<text x="{}" y="{}" class="small">D[3:0]: {}</text>'.format(left, top + 62, html.escape(evidence)))
    limits = waveform.get("limits")
    if limits:
        out.append('<rect class="note" x="20" y="{}" width="1160" height="38" rx="4"/>'.format(height - 55))
        out.append('<text x="30" y="{}" class="small">{}</text>'.format(height - 32, html.escape(limits)))
    out.append('</svg>')
    Path(path).write_text("\n".join(out) + "\n")


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
