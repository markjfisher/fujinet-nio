#!/usr/bin/env python3
"""Inspectable, bounded generator experiments. Help/dry-run perform no operations."""

import argparse
import configparser
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shlex
import shutil
import struct
import subprocess
import sys
import termios
import time
import zipfile
import uuid
import tempfile
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "scripts"))
import bridge_setup
import bootstrap

PICOTOOL = ROOT / "build/picotool-usb/picotool"
COMMAND_LOG = []
DEFAULT_ACQUISITION_SECONDS = 0.25


class Failure(Exception):
    def __init__(self, category, message, details=None):
        self.category = category
        self.details = details or {}
        super().__init__(message)


def require(condition, message, category="waveform"):
    if not condition:
        raise Failure(category, message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n")


def acquisition_parameters(manifest, override=None):
    """Return the bounded analyser duration and exact requested sample count."""
    configured = (
        override
        if override is not None
        else manifest.get("acquisition_seconds", DEFAULT_ACQUISITION_SECONDS)
    )
    try:
        seconds = float(configured)
    except (TypeError, ValueError) as error:
        raise Failure("configuration", "acquisition duration must be a number") from error
    require(
        0.01 <= seconds <= 60,
        "acquisition duration must be between 0.01 and 60 seconds",
        "configuration",
    )
    samples = round(manifest["samplerate_hz"] * seconds)
    require(samples >= 2, "acquisition duration requests too few samples", "configuration")
    return seconds, samples


def generated_sample_count(manifest):
    """Return the generator's declared completion count for this capability."""
    if manifest.get("analysis_kind") == "width_control":
        cases = manifest.get("control_transactions")
        require(isinstance(cases, list) and cases,
                "width_control needs control_transactions", "configuration")
        accepted = [case for case in cases if case.get("accepted") is True]
        require(accepted, "width_control needs an accepted transaction", "configuration")
        require(all(isinstance(case.get("value"), int) for case in accepted),
                "width_control accepted transaction needs a value", "configuration")
        expected = manifest.get("dut", {}).get("expected", {}).get("values")
        require(expected == [case["value"] for case in accepted],
                "width_control DUT values must match accepted transactions",
                "configuration")
        return len(accepted)
    expected = manifest.get("expected_values")
    require(isinstance(expected, list) and expected,
            "experiment needs expected_values", "configuration")
    return len(expected)


def generator_run_description(manifest):
    """Return the human wiring/prompt description without naming an experiment."""
    samples = generated_sample_count(manifest)
    if manifest.get("analysis_kind") == "width_control":
        cases = manifest["control_transactions"]
        wiring = ("GP2..17 = D0..15, GP18 = /AS, GP19 = R/W, "
                  "GP20 = /UDS, GP21 = /LDS, GP22 = SELECT; "
                  "analyzer D0..D7 follows this experiment's W1 mapping; common ground.")
        description = f"{len(cases)} /AS assertions ({samples} accepted writes)"
    else:
        wiring = "GP2..5 = D0..3, GP6 = /AS, analyzer D0,D1,D2,D3,D7; common ground."
        description = f"{samples}-value burst"
    return samples, wiring, description


def analyse(path, manifest):
    """Validate sigrok framing and all finite burst edges, independently of PIO."""
    metadata_text = ""
    try:
        with zipfile.ZipFile(path) as z:
            require(
                len(z.namelist()) == len(set(z.namelist())),
                "duplicate archive entries",
                "acquisition",
            )
            meta = configparser.ConfigParser()
            metadata_text = z.read("metadata").decode()
            meta.read_string(metadata_text)
            require(
                meta.sections() == ["global", "device 1"],
                "expected one logic device",
                "acquisition",
            )
            d = meta["device 1"]
            rate = re.fullmatch(
                r"([0-9]+(?:\.[0-9]+)?)\s*(Hz|kHz|MHz)", d["samplerate"]
            )
            require(rate is not None, "unknown sample rate", "acquisition")
            hz = float(rate[1]) * {"Hz": 1, "kHz": 1000, "MHz": 1000000}[rate[2]]
            require(
                hz == manifest["samplerate_hz"], "unexpected sample rate", "acquisition"
            )
            require(
                d["unitsize"] == "1"
                and d["total probes"] == "8"
                and d["capturefile"] == "logic-1",
                "unsupported logic layout",
                "acquisition",
            )
            for channel in manifest["analyzer_channels"]:
                require(
                    d.get("probe" + str(int(channel[1:]) + 1)) == channel,
                    "missing or mislabelled " + channel,
                    "acquisition",
                )
            names = [n for n in z.namelist() if n.startswith("logic-")]
            require(
                all(re.fullmatch(r"logic-1-[1-9][0-9]*", n) for n in names),
                "invalid logic chunks",
                "acquisition",
            )
            names.sort(key=lambda n: int(n.rsplit("-", 1)[1]))
            require(
                names == ["logic-1-" + str(n + 1) for n in range(len(names))] and names,
                "missing logic chunk",
                "acquisition",
            )
            require(
                sum(z.getinfo(n).file_size for n in names) <= 20000000,
                "capture exceeds 20 MB limit",
                "acquisition",
            )
            data = b"".join(z.read(n) for n in names)
    except Failure as error:
        error.details.update(
            expected_rate=manifest["samplerate_hz"],
            expected_channels=manifest["analyzer_channels"],
            observed_metadata=metadata_text,
        )
        raise
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, configparser.Error) as e:
        raise Failure(
            "acquisition",
            "invalid capture: " + str(e),
            dict(
                expected_rate=manifest["samplerate_hz"], observed_metadata=metadata_text
            ),
        ) from e
    kind = manifest.get("analysis_kind", "burst")
    require(kind in ("burst", "idle", "held_active", "sampling_window", "repetition", "width_control"), "Unknown analysis_kind", "configuration")
    if kind == "idle":
        return analyse_idle(path, manifest, data, hz)
    if kind == "held_active":
        return analyse_held_active(path, manifest, data, hz)
    if kind == "sampling_window":
        return analyse_sampling_window(path, manifest, data, hz)
    if kind == "repetition":
        return analyse_repetition(path, manifest, data, hz)
    if kind == "width_control":
        return analyse_width_control(path, manifest, data, hz)
    rows = []
    state = dict(expected=manifest["expected_values"], sample=None)

    def check(condition, message, category="waveform"):
        if not condition:
            raise Failure(
                category, message, dict(state, partial_measurements=list(rows))
            )

    check(len(data) > 1, "empty capture", "acquisition")
    fall = [i for i in range(1, len(data)) if data[i - 1] & 128 and not data[i] & 128]
    rise = [i for i in range(1, len(data)) if not data[i - 1] & 128 and data[i] & 128]
    expected = manifest["expected_values"]
    state.update(observed_falls=fall, observed_rises=rise)
    check(
        len(fall) == len(rise) == len(expected),
        f"expected {len(expected)} complete pulses, got {len(fall)} falls/{len(rise)} rises",
    )
    check(
        data[0] & 128 and data[-1] & 128, "capture starts or ends with asserted strobe"
    )

    def duration(name, index):
        value = manifest.get(name + "_us")
        if value is None and name in ("setup", "hold"):
            value = manifest.get("setup_hold_us")
        require(value is not None, "Missing " + name + " timing expectation", "configuration")
        if isinstance(value, list):
            require(
                index < len(value),
                "Missing " + name + " timing expectation for pulse " + str(index),
                "configuration",
            )
            value = value[index]
        require(
            isinstance(value, (int, float)) and value > 0,
            "Invalid " + name + " timing expectation",
            "configuration",
        )
        return value

    def near(samples, us, label):
        state.update(
            expected_us=us, observed_us=samples * 1000000 / hz, measurement=label
        )
        check(
            abs(samples * 1000000 / hz - us) <= 2,
            label + " timing outside 2 us tolerance",
        )

    for n, (f, r, value) in enumerate(zip(fall, rise, expected)):
        state.update(
            sample=f, pulse=n, expected_value=value, observed_value=data[f] & 15
        )
        check(f < r and (n == 0 or rise[n - 1] < f), "invalid edge ordering")
        near(r - f, duration("pulse", n), "pulse")
        wrong = next((i for i in range(f, r) if data[i] & 15 != value), None)
        if wrong is not None:
            state.update(sample=wrong, observed_value=data[wrong] & 15)
        check(wrong is None, f"data mismatch at pulse {n}")
        before, after = f, r
        while before and data[before - 1] & 15 == value:
            before -= 1
        while after < len(data) and data[after] & 15 == value:
            after += 1
        if n:
            near(f - before, duration("setup", n), "setup")
            if "period_us" in manifest:
                near(f - fall[n - 1], manifest["period_us"], "period")
        if n < len(expected) - 1:
            near(after - r, duration("hold", n), "hold")
        trailing_hold = manifest.get(
            "trailing_hold_us",
            manifest.get("setup_us", manifest.get("setup_hold_us")),
        )
        require(
            isinstance(trailing_hold, (int, float)) and trailing_hold > 0,
            "Missing or invalid trailing hold timing expectation",
            "configuration",
        )
        check(
            len(data) - r >= round(trailing_hold * hz / 1000000),
            "truncated trailing hold",
        )
        rows.append(
            dict(
                value=value,
                fall_sample=f,
                rise_sample=r,
                low_us=(r - f) * 1000000 / hz,
                setup_us=(f - before) * 1000000 / hz,
                hold_us=(after - r) * 1000000 / hz,
            )
        )
    return dict(
        analysis_kind=kind,
        capture=str(path),
        capture_sha256=digest(path),
        sample_rate=hz,
        assertions=len(rows),
        values=expected,
        measurements=rows,
        limits="First setup and final released data level are not independently observable.",
    )


def analyse_idle(path, manifest, data, hz):
    """Prove one idle data sequence, not DUT capture suppression."""
    require(len(data) > 1, "empty capture", "acquisition")
    low = next((index for index, value in enumerate(data) if not value & 128), None)
    if low is not None:
        raise Failure("waveform", "/AS asserted in idle capture", dict(sample=low))
    expected = manifest["expected_values"]
    require(
        expected == list(range(16)),
        "Idle stimulus requires the 0..15 sequence",
        "configuration",
    )
    # Compress data transitions. Data pins float before/after the bounded run;
    # only the complete timed sequence establishes the driven interval.
    starts = [0] + [
        i for i in range(1, len(data)) if (data[i] & 15) != (data[i - 1] & 15)
    ]
    ends = starts[1:] + [len(data)]
    values = [data[i] & 15 for i in starts]
    candidates = [
        i
        for i in range(len(values) - len(expected) + 1)
        if values[i : i + len(expected)] == expected
    ]
    require(len(candidates) == 1, "Expected one complete idle data sequence 0..15")
    first = candidates[0]
    period = manifest["data_period_us"]
    rows = []
    for n, value in enumerate(expected):
        index = first + n
        duration = (ends[index] - starts[index]) * 1000000 / hz
        # First/last intervals include unknown acquisition lead/trail and pin
        # release. Interior transitions independently establish exact timing.
        valid = duration >= period - 2 if n in (0, 15) else abs(duration - period) <= 2
        if not valid:
            raise Failure(
                "waveform",
                "Idle data hold outside timing tolerance",
                dict(
                    value=value,
                    sample=starts[index],
                    expected_us=period,
                    observed_us=duration,
                    partial_measurements=rows,
                ),
            )
        rows.append(
            dict(
                value=value,
                start_sample=starts[index],
                end_sample=ends[index],
                hold_us=duration,
            )
        )
    return dict(
        capture=str(path),
        capture_sha256=digest(path),
        sample_rate=hz,
        analysis_kind="idle",
        assertions=16,
        values=expected,
        measurements=rows,
        observed_falls=[],
        strobe="high throughout capture",
        limits="First/last data hold includes acquisition lead-in/release; data outside the detected sequence may float.",
    )


def analyse_held_active(path, manifest, data, hz):
    """Verify one strobe interval and every data phase within it."""
    expected = manifest["held_active_values"]
    state = dict(expected=expected)

    def check(condition, message):
        if not condition:
            raise Failure("waveform", message, state)

    check(len(data) > 1, "empty capture")
    falls = [i for i in range(1, len(data)) if data[i - 1] & 128 and not data[i] & 128]
    rises = [i for i in range(1, len(data)) if not data[i - 1] & 128 and data[i] & 128]
    state.update(observed_falls=falls, observed_rises=rises)
    check(len(falls) == len(rises) == 1, "expected one complete held-active strobe")
    fall, rise = falls[0], rises[0]
    check(fall < rise and data[0] & 128 and data[-1] & 128, "invalid held-active edge ordering")
    observed_low_us = (rise - fall) * 1000000 / hz
    state.update(expected_us=manifest["pulse_us"], observed_us=observed_low_us)
    check(abs(observed_low_us - manifest["pulse_us"]) <= 2, "held-active strobe width outside 2 us tolerance")
    start = fall
    value = data[start] & 15
    phases = []
    for sample in range(fall + 1, rise):
        current = data[sample] & 15
        if current != value:
            phases.append(dict(value=value, start_sample=start, end_sample=sample,
                               hold_us=(sample - start) * 1000000 / hz))
            start, value = sample, current
    phases.append(dict(value=value, start_sample=start, end_sample=rise,
                       hold_us=(rise - start) * 1000000 / hz))
    values = [phase["value"] for phase in phases]
    state.update(observed_values=values, phases=phases)
    check(values == expected, "held-active data phases differ from manifest")
    expected_phase_us = manifest["active_phase_us"]
    check(len(expected_phase_us) == len(phases), "held-active timing contract length differs from phases")
    for phase, expected_us in zip(phases, expected_phase_us):
        state.update(value=phase["value"], expected_us=expected_us,
                     observed_us=phase["hold_us"])
        check(abs(phase["hold_us"] - expected_us) <= 2,
              "held-active data phase outside 2 us tolerance")
    before = fall
    while before and (data[before - 1] & 15) == expected[0]:
        before -= 1
    setup_us = (fall - before) * 1000000 / hz
    state.update(expected_us=manifest["setup_us"], observed_us=setup_us)
    check(setup_us >= manifest["setup_us"] - 2, "initial data setup shorter than manifest")
    return dict(
        analysis_kind="held_active",
        capture=str(path),
        capture_sha256=digest(path),
        sample_rate=hz,
        assertions=1,
        values=values,
        observed_falls=falls,
        observed_rises=rises,
        measurements=[dict(value=expected[0], fall_sample=fall, rise_sample=rise,
                           low_us=observed_low_us, setup_us=setup_us,
                           hold_us=phases[0]["hold_us"])],
        transactions=[dict(index=0, assert_sample=fall, release_sample=rise,
                           capture_value=expected[0], phases=phases)],
        limits="The /AS fall marks the capture transaction. W0 has no external marker for the exact internal PIO sample clock; initial data is held through the first low phase.",
    )


def analyse_sampling_window(path, manifest, data, hz):
    """Measure declared data transitions before or after each /AS assertion."""
    cases = manifest.get("sampling_cases")
    require(isinstance(cases, list) and cases, "sampling_window needs sampling_cases", "configuration")
    falls = [i for i in range(1, len(data)) if data[i - 1] & 128 and not data[i] & 128]
    rises = [i for i in range(1, len(data)) if not data[i - 1] & 128 and data[i] & 128]
    require(len(falls) == len(rises) == len(cases), "unexpected sampling-window strobe edges")
    require(data[0] & 128 and data[-1] & 128, "sampling-window strobe must begin and end high")
    changes = [i for i in range(1, len(data)) if (data[i] & 15) != (data[i - 1] & 15)]
    rows, transactions = [], []
    for index, (case, fall, rise) in enumerate(zip(cases, falls, rises)):
        relation = case.get("relation")
        require(relation in ("before", "after"), "sampling case relation must be before or after", "configuration")
        expected = case.get("captured")
        old, newer = case.get("old"), case.get("new")
        require(all(isinstance(value, int) and 0 <= value < 16 for value in (expected, old, newer)),
                "sampling case values must be four-bit integers", "configuration")
        low_us = (rise - fall) * 1000000 / hz
        require(abs(low_us - case["pulse_us"]) <= 2,
                "sampling-window strobe width outside 2 us tolerance")
        captured = data[fall] & 15
        require(captured == expected, "sampling-window captured unexpected value")
        if relation == "before":
            lower = rises[index - 1] if index else 0
            candidates = [sample for sample in changes if lower < sample < fall]
            require(candidates, "missing data transition before /AS")
            transition = candidates[-1]
            require(data[transition - 1] & 15 == old and data[transition] & 15 == newer,
                    "unexpected data transition before /AS")
            offset_us = (fall - transition) * 1000000 / hz
        else:
            candidates = [sample for sample in changes if fall < sample < rise]
            require(candidates, "missing data transition while /AS is low")
            transition = candidates[0]
            require(data[fall] & 15 == old and data[transition - 1] & 15 == old and
                    data[transition] & 15 == newer,
                    "unexpected data transition after /AS")
            offset_us = (transition - fall) * 1000000 / hz
        require(abs(offset_us - case["offset_us"]) <= 2,
                "sampling-window data offset outside 2 us tolerance")
        phase_starts = [fall] + [sample for sample in changes if fall < sample < rise]
        phase_ends = phase_starts[1:] + [rise]
        phases = [dict(value=data[start] & 15, start_sample=start, end_sample=end,
                       hold_us=(end - start) * 1000000 / hz)
                  for start, end in zip(phase_starts, phase_ends)]
        rows.append(dict(case=case.get("id", str(index)), relation=relation,
                         old=old, new=newer, captured=captured, fall_sample=fall,
                         rise_sample=rise, transition_sample=transition,
                         offset_us=offset_us, low_us=low_us))
        transactions.append(dict(index=index, assert_sample=fall, release_sample=rise,
                                 capture_value=captured, phases=phases))
    return dict(capture=str(path), capture_sha256=digest(path), sample_rate=hz,
                analysis_kind="sampling_window", assertions=len(falls),
                values=[row["captured"] for row in rows], observed_falls=falls,
                observed_rises=rises, measurements=rows, transactions=transactions,
                limits="External offsets are 1 MHz samples (about ±1 us). W0 has no marker for the internal PIO IN PINS clock.")


def analyse_repetition(path, manifest, data, hz):
    """Validate repeated assertions while varying low width and released gap."""
    groups = manifest.get("repetition_groups")
    require(isinstance(groups, list) and groups, "repetition needs repetition_groups", "configuration")
    falls = [i for i in range(1, len(data)) if data[i - 1] & 128 and not data[i] & 128]
    rises = [i for i in range(1, len(data)) if not data[i - 1] & 128 and data[i] & 128]
    count = sum(group.get("count", 0) for group in groups)
    require(len(falls) == len(rises) == count, "unexpected repetition strobe edges")
    require(data[0] & 128 and data[-1] & 128, "repetition strobe must begin and end high")
    rows, transactions, group_rows, index = [], [], [], 0
    for group in groups:
        value, repeats = group.get("value"), group.get("count")
        pulse_us, gap_us = group.get("pulse_us"), group.get("gap_us")
        require(isinstance(value, int) and 0 <= value < 16 and isinstance(repeats, int) and repeats > 0,
                "invalid repetition group value/count", "configuration")
        require(all(isinstance(v, (int, float)) and v > 0 for v in (pulse_us, gap_us)),
                "invalid repetition group timing", "configuration")
        group_rows.append(dict(id=group.get("id", str(len(group_rows))), value=value,
                               count=repeats, pulse_us=pulse_us, gap_us=gap_us))
        for repeat in range(repeats):
            fall, rise = falls[index], rises[index]
            low_us = (rise - fall) * 1000000 / hz
            require(abs(low_us - pulse_us) <= 2, "repetition low width outside 2 us tolerance")
            require(data[fall] & 15 == value and all((sample & 15) == value for sample in data[fall:rise]),
                    "repetition data differs while /AS is asserted")
            row = dict(group=group_rows[-1]["id"], repeat=repeat + 1, value=value,
                       fall_sample=fall, rise_sample=rise, low_us=low_us)
            if repeat + 1 < repeats:
                gap = (falls[index + 1] - rise) * 1000000 / hz
                require(abs(gap - gap_us) <= 2, "repetition released gap outside 2 us tolerance")
                row["gap_us"] = gap
            rows.append(row)
            transactions.append(dict(index=index, assert_sample=fall, release_sample=rise,
                                     capture_value=value, phases=[dict(value=value, start_sample=fall, end_sample=rise, hold_us=low_us)]))
            index += 1
    return dict(capture=str(path), capture_sha256=digest(path), sample_rate=hz,
                analysis_kind="repetition", assertions=count,
                values=[row["value"] for row in rows], observed_falls=falls,
                observed_rises=rises, measurements=rows, groups=group_rows,
                transactions=transactions,
                limits="Only intra-group gaps are controlled. Cross-group gaps include the safe data-value transition and are diagnostic, not a timing limit.")


def analyse_width_control(path, manifest, data, hz):
    """Validate the explicitly observed W1 control subset at every /AS pulse."""
    signals = manifest.get("analyzer_signals")
    cases = manifest.get("control_transactions")
    require(isinstance(signals, dict) and isinstance(cases, list) and cases,
            "width_control needs analyzer_signals and control_transactions", "configuration")

    def bit(name):
        channel = signals.get(name)
        matched = re.fullmatch(r"D([0-7])", str(channel))
        require(matched is not None, "invalid analyzer channel for " + name, "configuration")
        return int(matched[1])

    as_bit = bit("as")
    control_bits = {name: bit(name) for name in ("select", "rw", "uds", "lds")}
    data_bits = signals.get("data_bits")
    require(isinstance(data_bits, dict) and data_bits,
            "width_control needs observed data_bits", "configuration")
    observed_data_bits = {}
    for name, channel in data_bits.items():
        match = re.fullmatch(r"D([0-7])", str(channel))
        require(match is not None and re.fullmatch(r"D(0|[1-9]|1[0-5])", str(name)),
                "invalid observed W1 data bit", "configuration")
        observed_data_bits[int(name[1:])] = int(match[1])

    falls = [i for i in range(1, len(data))
             if data[i - 1] & (1 << as_bit) and not data[i] & (1 << as_bit)]
    rises = [i for i in range(1, len(data))
             if not data[i - 1] & (1 << as_bit) and data[i] & (1 << as_bit)]
    require(len(falls) == len(rises) == len(cases),
            "unexpected width/control /AS edges")
    require(data[0] & (1 << as_bit) and data[-1] & (1 << as_bit),
            "width/control /AS must begin and end high")

    rows, transactions = [], []
    for index, (case, fall, rise) in enumerate(zip(cases, falls, rises)):
        required = ("id", "value", "accepted", "select", "rw", "uds", "lds")
        require(all(key in case for key in required),
                "incomplete width/control transaction", "configuration")
        value = case["value"]
        require(isinstance(value, int) and 0 <= value <= 0xffff and
                isinstance(case["accepted"], bool) and
                all(case[name] in (0, 1) for name in ("select", "rw", "uds", "lds")),
                "invalid width/control transaction", "configuration")
        low_us = (rise - fall) * 1000000 / hz
        require(abs(low_us - manifest["pulse_us"]) <= 2,
                "width/control /AS width outside 2 us tolerance")
        if index:
            setup_us = (fall - rises[index - 1]) * 1000000 / hz
            require(abs(setup_us - manifest["setup_us"]) <= 2,
                    "width/control released setup outside 2 us tolerance")
        else:
            setup_us = None
        for name, channel_bit in control_bits.items():
            actual = 1 if data[fall] & (1 << channel_bit) else 0
            require(actual == case[name], "width/control " + name + " differs at /AS fall")
            require(all((sample >> channel_bit) & 1 == case[name]
                        for sample in data[fall:rise]),
                    "width/control " + name + " changes while /AS is low")
        observed = {}
        for data_bit, channel_bit in observed_data_bits.items():
            actual = 1 if data[fall] & (1 << channel_bit) else 0
            expected = (value >> data_bit) & 1
            require(actual == expected and
                    all((sample >> channel_bit) & 1 == expected for sample in data[fall:rise]),
                    "width/control observed data bit differs while /AS is low")
            observed["D" + str(data_bit)] = actual
        row = dict(index=index, id=case["id"], value=value,
                   accepted=case["accepted"], fall_sample=fall, rise_sample=rise,
                   low_us=low_us, setup_us=setup_us, controls={
                       name: case[name] for name in control_bits}, observed_data=observed)
        rows.append(row)
        transactions.append(dict(index=index, id=case["id"], accepted=case["accepted"],
                                 assert_sample=fall, release_sample=rise,
                                 capture_value=value, phases=[dict(value=value,
                                 start_sample=fall, end_sample=rise, hold_us=low_us)]))
    return dict(capture=str(path), capture_sha256=digest(path), sample_rate=hz,
                analysis_kind="width_control", assertions=len(rows),
                accepted_assertions=sum(row["accepted"] for row in rows),
                values=[row["value"] for row in rows if row["accepted"]],
                observed_falls=falls, observed_rises=rises, measurements=rows,
                transactions=transactions, analyzer_signals=signals,
                limits="The analyzer observes /AS, SELECT, R/W, /UDS, /LDS and D0/D8/D15 only. Full 16-bit values are independently checked by the DUT report; this trace is not a simultaneous full-bus capture.")


def acceptance(manifest, dut_evidence=None):
    evidence_scope = (
        "stimulus-and-dut"
        if isinstance(dut_evidence, dict) and dut_evidence.get("status") == "observed"
        else "stimulus-only"
    )
    if manifest.get("dut"):
        if evidence_scope == "stimulus-and-dut":
            return dict(
                status="passed",
                category=None,
                stimulus_status="passed",
                experiment_status="passed",
                evidence_scope=evidence_scope,
                dut_evidence=dut_evidence,
            )
        return dict(
            status="stimulus_passed",
            category=None,
            stimulus_status="passed",
            experiment_status="incomplete",
            evidence_scope=evidence_scope,
            dut_evidence=dict(
                status="not_observed",
                reason="This report has waveform evidence only; no DUT counter report was collected.",
            ),
        )
    if manifest.get("analysis_kind") == "idle":
        return dict(status="stimulus_passed", category=None,
                    stimulus_status="passed", experiment_status="incomplete",
                    evidence_scope="stimulus-only",
                    dut_evidence=dict(status="not_observed",
                                      reason="This report has waveform evidence only; no DUT counter report was collected."))
    return dict(status="passed", category=None, evidence_scope=evidence_scope)


def dut_artifact(manifest):
    dut = manifest.get("dut")
    if not dut:
        return None
    return ROOT / "build" / dut["preset"] / (dut["target"] + ".elf")


def dut_reset(console, protocol):
    console.send("reset")
    require(console.line(3) == "reset protocol=" + protocol,
            "DUT did not acknowledge a fresh counter reset", "transport")


def dut_report(console, contract):
    protocol = contract["protocol"]
    console.send("report")
    line = console.line(3)
    match = re.fullmatch(
        r"result protocol=" + re.escape(protocol) +
        r" capture_count=([0-9]+) capture_irq_count=([0-9]+) values=([0-9,]*)", line)
    require(match is not None, "Malformed DUT counter report: " + line, "transport")
    observed = dict(capture_count=int(match[1]), capture_irq_count=int(match[2]),
                    values=[] if not match[3] else [int(value) for value in match[3].split(",")])
    expected = contract["expected"]
    require(all(observed.get(key) == value for key, value in expected.items()),
            "DUT counters differ from manifest expectation", "dut",
            )
    return dict(status="observed", protocol=protocol, expected=expected, observed=observed)


def command(args, category="build", timeout=180):
    argv = list(map(str, args))
    print("+ " + shlex.join(argv), flush=True)
    record = dict(argv=argv, returncode=None, output="", termination=None)
    COMMAND_LOG.append(record)
    child = None
    # A file retains partial output even when Ctrl-C interrupts communicate.
    with tempfile.TemporaryFile(mode="w+t") as output:
        try:
            child = subprocess.Popen(
                argv, cwd=ROOT, text=True, stdout=output, stderr=subprocess.STDOUT
            )
            child.wait(timeout=timeout)
        except BaseException as error:
            record["termination"] = type(error).__name__ + ": " + str(error)
            if child is not None and child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
            if isinstance(error, (OSError, subprocess.TimeoutExpired)):
                raise Failure(category, str(error)) from error
            raise
        finally:
            if child is not None:
                record["returncode"] = child.returncode
            output.seek(0)
            record["output"] = output.read()
            print(record["output"], end="", flush=True)
    require(
        record["returncode"] == 0,
        f"command failed ({record['returncode']}): {record['output']}",
        category,
    )
    return record["output"]


def source_identity(m):
    # Hash source/config inputs as well as recording Git state. Build products,
    # dependency caches and evidence captures are not first-party build inputs.
    inputs = {}
    for folder in (
        "src",
        "lab",
        "cmake",
        m["source_dir"],
    ):
        for path in sorted((ROOT / folder).rglob("*")):
            if path.is_file():
                inputs[str(path.relative_to(ROOT))] = digest(path)
    for path in sorted((ROOT / "scripts").iterdir()):
        if path.is_file() and path.suffix in (".py", ".sh"):
            inputs[str(path.relative_to(ROOT))] = digest(path)
    for name in ("experiment.py", "test_stimulus.c", "stimulus_expectations.h"):
        inputs["tests/feasibility/" + name] = digest(Path(__file__).with_name(name))
    for name in ("CMakeLists.txt", "CMakePresets.json", "dependencies.json"):
        inputs[name] = digest(ROOT / name)
    return dict(
        revision=command(["git", "rev-parse", "HEAD"]).strip(),
        status=command(["git", "status", "--short"]),
        inputs=inputs,
        manifest=m,
        sdk_path=os.environ.get("PICO_SDK_PATH", str(ROOT / ".deps/pico-sdk")),
        toolchain=os.environ.get("PICO_TOOLCHAIN_PATH"),
        compiler=command(["arm-none-eabi-gcc", "--version"]).strip(),
    )


def provenance(artifact):
    try:
        record = json.loads(artifact.with_suffix(".build.json").read_text())
    except (OSError, ValueError) as error:
        raise Failure(
            "transport", "Missing/invalid build identity sidecar; run build first."
        ) from error
    require(
        record.get("firmware_sha256") == digest(artifact),
        "Artifact differs from build identity; run build first.",
        "transport",
    )
    return record


def static_prerequisites(analyzer=False):
    configure_toolchain()
    tools = ["cmake", "ninja", "ctest", "git", "arm-none-eabi-gcc", "pkg-config"]
    if analyzer:
        tools.append("sigrok-cli")
    missing = [tool for tool in tools if not shutil.which(tool)]
    require(
        not missing,
        "Missing tools: "
        + ", ".join(missing)
        + "; source scripts/env.sh or install prerequisites.",
        "environment",
    )
    command(["pkg-config", "--exists", "libusb-1.0"], "environment")


def build(m):
    sdk_path = str(bootstrap.selected_sdk())
    if os.environ.get("PICO_SDK_PATH"):
        os.environ["PICO_SDK_PATH"] = sdk_path
    static_prerequisites()
    bridge_setup.validate_compiler_cache(ROOT / "build" / m["preset"])
    before = source_identity(m)
    command([str(ROOT / "scripts/create-deps.sh"), "--mode", "all"])
    presets = ["host", "host-release", m["preset"]]
    if m.get("dut"):
        presets.append(m["dut"]["preset"])
    for preset in presets:
        sdk_args = (
            []
            if preset.startswith("host")
            else [
                "-DPICO_SDK_PATH=" + sdk_path,
                "-DPICO_TOOLCHAIN_PATH=" + os.environ["PICO_TOOLCHAIN_PATH"],
            ]
        )
        command(["cmake", "--preset", preset, *sdk_args])
        command(["cmake", "--build", "--preset", preset])
        if preset.startswith("host"):
            command(["ctest", "--preset", preset])
    command(
        [
            "cmake",
            "-S",
            ".deps/picotool",
            "-B",
            PICOTOOL.parent,
            "-DPICOTOOL_NO_LIBUSB=OFF",
            "-DPICO_SDK_PATH=" + sdk_path,
        ]
    )
    command(["cmake", "--build", PICOTOOL.parent, "-j2"])
    require(
        source_identity(m) == before,
        "Source/config changed during build; rerun build.",
        "build",
    )
    for artifact in filter(None, (ROOT / "build" / m["preset"] / (m["target"] + ".elf"), dut_artifact(m))):
        identity = dict(before)
        identity["firmware_sha256"] = digest(artifact)
        identity["cmake_cache_sha256"] = digest(artifact.parent / "CMakeCache.txt")
        save(artifact.with_suffix(".build.json"), identity)


def usb_devices():
    devices = []
    for p in Path("/sys/bus/usb/devices").iterdir():
        try:
            devices.append(
                dict(
                    path=p.name,
                    vid=(p / "idVendor").read_text().strip(),
                    pid=(p / "idProduct").read_text().strip(),
                    bus=int((p / "busnum").read_text()),
                    address=int((p / "devnum").read_text()),
                    inode=p.resolve().stat().st_ino,
                    serial=(p / "serial").read_text().strip() if (p / "serial").exists() else None,
                )
            )
        except (OSError, ValueError):
            continue
    return devices


def access(d):
    node = Path(f"/dev/bus/usb/{d['bus']:03}/{d['address']:03}")
    require(
        os.access(node, os.R_OK | os.W_OK),
        f"No read/write access to {node}; install the documented one-time permissions then reconnect.",
        "transport",
    )


def serial_port(d):
    ports = []
    for p in Path("/sys/class/tty").glob("ttyACM*"):
        if (Path("/sys/bus/usb/devices") / d["path"]).resolve() in (
            p / "device"
        ).resolve().parents:
            ports.append(Path("/dev") / p.name)
    require(
        len(ports) == 1,
        f"Expected one CDC port on physical USB path {d['path']}, got {ports}",
        "transport",
    )
    require(
        os.access(ports[0], os.R_OK | os.W_OK),
        f"No serial access to {ports[0]}; see one-time permissions setup.",
        "transport",
    )
    return ports[0]


def preferred_serial_port(d):
    """Use a stable by-id link when the connected CDC device exposes one."""
    port = serial_port(d)
    for link in Path("/dev/serial/by-id").glob("*"):
        try:
            if link.resolve() == port.resolve():
                return link
        except OSError:
            continue
    return port


def dut_connection_hints(m, args):
    """Print copyable manifest-DUT arguments from connected RP devices; never guess roles."""
    label = m["id"] + " DUT"
    profile = read_profile(args.bench)
    generator_path, configured_dut_path = profile_paths(profile)
    if generator_path:
        print("Configured generator USB: " + generator_path)
    if configured_dut_path:
        print("Configured DUT USB: " + configured_dut_path)
    devices = [d for d in usb_devices() if d["vid"] == "2e8a"]
    dut_usb = [d for d in devices if d["pid"] in ("0009", "000f")]
    if len(dut_usb) == 1:
        d = dut_usb[0]
        suffix = " (serial " + d["serial"] + ")" if d.get("serial") else ""
        print(label + " USB: --dut-usb-path " + d["path"] + suffix)
        print(label + " port: unavailable until the feasibility DUT firmware is RAM-loaded.")
    elif len(dut_usb) > 1:
        print(label + " USB candidates: " + ", ".join(d["path"] for d in dut_usb))
    generator_path = None
    try:
        generator_path = json.loads(args.session.read_text()).get("usb", {}).get("path")
    except (OSError, ValueError):
        pass
    runtime = [d for d in devices if d["pid"] == "000a" and d["path"] != generator_path]
    if len(runtime) == 1:
        d = runtime[0]
        try:
            print(label + " arguments: --dut-usb-path {} --dut-port {}".format(
                d["path"], preferred_serial_port(d)))
        except Failure as error:
            print(label + " runtime found at {} but no accessible CDC port: {}".format(
                d["path"], error))
    elif len(runtime) > 1:
        print(label + " runtime candidates: " + ", ".join(d["path"] for d in runtime))
    if generator_path is None:
        generators = [d for d in devices if d["pid"] in ("0003", "000a")]
        if len(generators) == 1:
            generator_path = generators[0]["path"]
    if generator_path and len(dut_usb) == 1:
        print(
            "Save these paths: ./run.sh configure-paths --usb-path "
            + generator_path
            + " --dut-usb-path "
            + dut_usb[0]["path"]
        )


def boot_id():
    return Path("/proc/sys/kernel/random/boot_id").read_text().strip()


def configure_instruction(path):
    return f"No enrolled generator. Run {shlex.quote(str(ROOT / 'tests/feasibility/generator-check/run.sh'))} configure --bench {shlex.quote(str(path))} with your RP2040 in BOOTSEL."


def valid_usb_path(path):
    return isinstance(path, str) and re.fullmatch(r"[0-9]+-[0-9]+(?:\.[0-9]+)*", path)


def profile_paths(profile):
    """Return optional topology selections stored for this local bench."""
    if profile is None:
        return None, None
    return profile.get("generator_usb_path"), profile.get("dut_usb_path")


def apply_profile_paths(args, profile):
    """Use stored topology only when this invocation did not override it."""
    generator, dut = profile_paths(profile)
    if args.usb_path is None:
        args.usb_path = generator
    if args.dut_usb_path is None:
        args.dut_usb_path = dut


def make_profile(identity, generator_usb_path=None, dut_usb_path=None):
    """Create the smallest profile that records the known local bench setup."""
    value = dict(version=1, generator_flash_id=identity, analyzer="fx2lafw")
    if generator_usb_path is not None:
        value.update(version=2, generator_usb_path=generator_usb_path)
        if dut_usb_path is not None:
            value["dut_usb_path"] = dut_usb_path
    return value


def read_profile(path, required=False):
    try:
        return parse_profile(path, required)
    except Failure as error:
        if Path(path).exists():
            raise Failure(
                "configuration",
                str(error)
                + f". Recovery: move {shlex.quote(str(path))} aside, then run "
                + configure_instruction(path),
            ) from error
        raise


def parse_profile(path, required=False):
    try:
        value = json.loads(Path(path).read_text())
    except FileNotFoundError:
        require(not required, configure_instruction(path), "configuration")
        return None
    except (OSError, ValueError) as error:
        raise Failure(
            "configuration", f"Invalid bench profile {path}: {error}"
        ) from error
    base = {"version", "generator_flash_id", "analyzer"}
    extended = base | {"generator_usb_path", "dut_usb_path"}
    require(isinstance(value, dict) and type(value.get("version")) is int,
            f"Invalid bench profile {path}: missing integer version", "configuration")
    if value["version"] == 1:
        require(set(value) == base,
                f"Invalid bench profile {path}: version 1 permits only generator_flash_id and analyzer",
                "configuration")
    elif value["version"] == 2:
        require(set(value) in (base | {"generator_usb_path"}, extended)
                and valid_usb_path(value.get("generator_usb_path"))
                and ("dut_usb_path" not in value or valid_usb_path(value["dut_usb_path"])),
                f"Invalid bench profile {path}: version 2 needs generator_usb_path and an optional dut_usb_path in USB topology form",
                "configuration")
        require(value.get("generator_usb_path") != value.get("dut_usb_path"),
                "Invalid bench profile: generator and DUT USB paths must differ", "configuration")
    else:
        raise Failure("configuration", f"Invalid bench profile {path}: supported versions are 1 and 2")
    identity = value["generator_flash_id"]
    require(
        isinstance(identity, str)
        and re.fullmatch(r"[0-9a-fA-F]{16}", identity)
        and identity.upper() not in ("0" * 16, "E" * 16, "F" * 16),
        "Invalid generator_flash_id: expected non-placeholder 16hex flash identity",
        "configuration",
    )
    require(
        value["analyzer"] == "fx2lafw",
        "Invalid analyzer: profile supports fx2lafw only; use --analyzer for transient conn selection",
        "configuration",
    )
    return dict(value, generator_flash_id=identity.upper())


def assert_profile(args, expected):
    require(
        read_profile(args.bench, required=True) == expected,
        "Bench profile changed during operation; restart with the intended profile.",
        "configuration",
    )


def identify_bootsel(args, identity=None):
    require(
        PICOTOOL.is_file(),
        f"Build pinned USB picotool first: {shlex.quote(str(ROOT / 'tests/feasibility/generator-check/run.sh'))} build",
        "environment",
    )
    deadline = time.monotonic() + args.timeout
    selected = None
    while time.monotonic() < deadline:
        candidates = [
            d
            for d in usb_devices()
            if (d["vid"], d["pid"]) == ("2e8a", "0003")
            and (not args.usb_path or d["path"] == args.usb_path)
        ]
        require(
            len(candidates) <= 1,
            "Ambiguous RP2040 BOOTSEL devices; specify --usb-path from doctor.",
            "transport",
        )
        if candidates:
            selected = candidates[0]
            break
        time.sleep(0.2)
    require(selected is not None, "Timed out waiting for RP2040 BOOTSEL", "transport")
    access(selected)
    info = command(
        [
            PICOTOOL,
            "info",
            "-a",
            "--bus",
            selected["bus"],
            "--address",
            selected["address"],
        ],
        "transport",
        10,
    )
    discovered = validate_info(info, identity)
    require(
        selected in usb_devices(), "USB identity changed during validation", "transport"
    )
    return selected, info, discovered


def configure(args):
    # Remember exact existing bytes so a concurrent edit during the prompt cannot
    # become an accidentally authorized replacement.
    read_profile(args.bench)
    require(
        sys.stdin.isatty(),
        "Interactive terminal required to confirm bench enrollment",
        "configuration",
    )
    previous = args.bench.read_bytes() if args.bench.exists() else None
    require(
        PICOTOOL.is_file(),
        f"Build pinned USB picotool first: {shlex.quote(str(ROOT / 'tests/feasibility/generator-check/run.sh'))} build",
        "environment",
    )
    command(
        [sys.executable, "scripts/bootstrap.py", "--mode", "stimulus", "--check"],
        "environment",
    )
    print(
        f"Hold BOOT while reconnecting your RP2040. Waiting at most {args.timeout:g}s; enrollment reads identity only.",
        flush=True,
    )
    selected, info, identity = identify_bootsel(args)
    print(
        f"Discovered RP2040 flash identity {identity}; physical USB path {selected['path']}",
        flush=True,
    )
    prompt = (
        f"Replace existing bench profile {args.bench}"
        if previous is not None
        else f"Enroll generator in {args.bench}"
    )
    require(
        input(prompt + "? Type yes to confirm: ").strip().lower() == "yes",
        "Enrollment cancelled; profile unchanged",
        "cancelled",
    )
    require(
        selected in usb_devices(),
        "USB device changed during confirmation; configure again",
        "transport",
    )
    confirmed, _, _ = identify_bootsel(args, identity)
    require(
        confirmed == selected,
        "USB device changed during confirmation; configure again",
        "transport",
    )
    dut_path = getattr(args, "dut_usb_path", None)
    require(dut_path is None or valid_usb_path(dut_path),
            "--dut-usb-path must be in physical USB topology form", "configuration")
    require(dut_path != selected["path"],
            "Generator and DUT USB paths must differ", "configuration")
    value = make_profile(identity, selected["path"], dut_path)
    publish_profile(args.bench, value, previous)
    print(f"Enrolled {identity} in {args.bench}")
    args.enrolled_profile = value
    return selected


def configure_paths(m, args):
    """Persist explicit physical ports after the board identity is enrolled."""
    profile = read_profile(args.bench, required=True)
    require(valid_usb_path(args.usb_path),
            "configure-paths needs --usb-path in physical USB topology form (for example 1-2.3)",
            "configuration")
    if m.get("dut"):
        require(valid_usb_path(args.dut_usb_path),
                "This experiment needs --dut-usb-path in physical USB topology form", "configuration")
    elif args.dut_usb_path is not None:
        require(valid_usb_path(args.dut_usb_path),
                "--dut-usb-path must be in physical USB topology form", "configuration")
    require(args.usb_path != args.dut_usb_path,
            "Generator and DUT USB paths must differ", "configuration")
    previous = args.bench.read_bytes()
    value = make_profile(profile["generator_flash_id"], args.usb_path, args.dut_usb_path)
    publish_profile(args.bench, value, previous)
    print("Saved bench USB paths in " + str(args.bench))
    print("Generator: " + args.usb_path)
    if args.dut_usb_path:
        print("DUT: " + args.dut_usb_path)
    return value


def publish_profile(path, value, previous):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        # Keep this inode separate from the replaced profile inode. Every
        # cooperating writer holds it across compare and atomic publication.
        with (path.parent / (path.name + ".lock")).open("a") as lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            with tempfile.NamedTemporaryFile(
                mode="w", dir=path.parent, prefix=".bench-", delete=False
            ) as stream:
                temporary = Path(stream.name)
                json.dump(value, stream, indent=2)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            current = path.read_bytes() if path.exists() else None
            require(
                current == previous,
                "Bench profile changed during confirmation; configure again",
                "configuration",
            )
            if previous is None:
                os.link(temporary, path)
            else:
                os.replace(temporary, path)
            directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def validate_info(info, identity=None):
    require(
        re.search(r"(?im)^\s*(?:type|device type):\s*RP2040\b", info) is not None,
        "picotool did not verify RP2040 type",
        "transport",
    )
    match = re.search(r"(?im)^\s*flash id:\s*(?:0x)?([0-9a-f]{16})\s*$", info)
    require(
        match is not None
        and match[1].upper() not in ("0" * 16, "F" * 16, "E" * 16)
        and (identity is None or match[1].upper() == identity.upper()),
        "wrong or unavailable generator flash identity",
        "transport",
    )

    return match[1].upper()


def validate_ram_elf(artifact, ram_end=0x20042000):
    data = artifact.read_bytes()
    require(
        len(data) >= 52 and data[:7] == b"\x7fELF\x01\x01\x01",
        "Expected little-endian ELF32 RAM artifact",
        "transport",
    )
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    entry, offset, size, count = header[3], header[4], header[8], header[9]
    require(
        header[1] == 40
        and 0x20000000 <= (entry & ~1) < ram_end
        and size == 32
        and count > 0,
        "Expected ARM RAM-only entry/program headers",
        "transport",
    )
    require(
        offset + size * count <= len(data), "Truncated ELF program headers", "transport"
    )
    loads = 0
    for n in range(count):
        kind, fileoff, virtual, physical, filesz, memsz, flags, align = (
            struct.unpack_from("<IIIIIIII", data, offset + n * size)
        )
        if kind == 1 and memsz:
            loads += 1
            require(
                filesz <= memsz
                and fileoff + filesz <= len(data)
                and 0x20000000 <= physical < physical + memsz <= ram_end
                and 0x20000000 <= virtual < virtual + memsz <= ram_end,
                "ELF contains non-RAM or invalid load segment; refusing load",
                "transport",
            )
    require(loads > 0, "ELF has no loadable RAM segments", "transport")


def load(m, args, artifact):
    profile = read_profile(args.bench, required=True)
    args.session = args.session.resolve()
    require(artifact.is_file(), "Build the firmware before load", "transport")
    build_identity = provenance(artifact)
    # Private immutable-for-this-load copy prevents a concurrent build replacing
    # the bytes between validation, picotool reading, and session recording.
    snapshot_dir = args.session.parent / "artifacts"
    snapshot_dir.mkdir(parents=True, exist_ok=True)
    snapshot = snapshot_dir / (uuid.uuid4().hex + ".elf")
    with snapshot.open("xb") as stream:
        stream.write(artifact.read_bytes())
    snapshot.chmod(0o400)
    loaded_hash = digest(snapshot)
    require(
        loaded_hash == build_identity["firmware_sha256"],
        "Artifact changed while taking load snapshot; rebuild.",
        "transport",
    )
    load_mode = m.get("load_mode", "ram")
    require(load_mode in ("ram", "flash"), "Unknown generator load_mode", "configuration")
    if load_mode == "ram":
        validate_ram_elf(snapshot)
    command(
        [sys.executable, "scripts/bootstrap.py", "--mode", "stimulus", "--check"],
        "transport",
    )
    if load_mode == "ram":
        print(
            "Hold BOOT while reconnecting the RP2040 generator (or BOOT + reset), then release BOOT.\n"
            f"Waiting at most {args.timeout:g}s for flash identity {profile['generator_flash_id']}. RAM load starts idle firmware only.",
            flush=True,
        )
        selected, info, _ = identify_bootsel(args, profile["generator_flash_id"])
        force = []
    else:
        require(args.usb_path, "Flash-installed generator needs --usb-path from doctor", "configuration")
        print("Keep the flash-installed RP2040 connected. picotool will force its USB "
              f"firmware into the loader at USB {args.usb_path}.", flush=True)
        deadline = time.monotonic() + args.timeout
        selected = None
        while time.monotonic() < deadline:
            candidates = [d for d in usb_devices() if d["path"] == args.usb_path]
            require(len(candidates) <= 1, "Ambiguous RP2040 USB path", "transport")
            if candidates:
                selected = candidates[0]
                access(selected)
                break
            time.sleep(0.2)
        require(selected is not None, "Timed out waiting for flash-installed RP2040", "transport")
        if selected["pid"] == "0003":
            selected, info, _ = identify_bootsel(args, profile["generator_flash_id"])
            force = []
        else:
            info = "forced flash load by physical USB path"
            force = ["-f"]
    assert_profile(args, profile)
    selector = ["--bus", selected["bus"], "--address", selected["address"]]
    command([PICOTOOL, "load", "-v", "-x", snapshot, *force, *selector], "transport", 30)
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        runtime = [
            d
            for d in usb_devices()
            if d["path"] == selected["path"]
            and (d["vid"], d["pid"]) == ("2e8a", "000a")
        ]
        if len(runtime) == 1:
            d = runtime[0]
            try:
                port = serial_port(d)
            except Failure:
                time.sleep(0.2)
                continue
            record = dict(
                experiment=m["id"],
                flash_id=profile["generator_flash_id"],
                bench_profile=profile,
                firmware_sha256=loaded_hash,
                artifact_snapshot=str(snapshot),
                build_identity=build_identity,
                usb=d,
                boot_id=boot_id(),
                serial_port=str(port),
                picotool_info=info,
                commands=list(COMMAND_LOG),
            )
            args.session.parent.mkdir(parents=True, exist_ok=True)
            save(args.session, record)
            label = "RAM" if load_mode == "ram" else "flash"
            print(
                f'Validated {label} session saved to {args.session}; CDC {port}; physical port {d["path"]}'
            )
            return
        time.sleep(0.2)
    raise Failure(
        "transport",
        f"{load_mode} CDC re-enumeration/access timed out; inspect permissions. No session recorded.",
    )


def load_dut(m, args, artifact):
    """Load a manifest-declared DUT image; port identity stays explicit."""
    contract = m.get("dut")
    if not contract:
        return
    require(args.dut_usb_path,
            "This experiment needs --dut-usb-path for the Core2350B",
            "configuration")
    require(artifact.is_file(), "Build DUT firmware before load", "transport")
    validate_ram_elf(artifact, 0x20082000)
    print("Keep the Core2350B connected. picotool will force its compatible USB "
          f"firmware into the loader at USB {args.dut_usb_path} (up to {args.timeout:g}s).", flush=True)
    deadline = time.monotonic() + args.timeout
    selected = None
    while time.monotonic() < deadline:
        candidates = [d for d in usb_devices() if d["path"] == args.dut_usb_path]
        require(len(candidates) <= 1, "Ambiguous DUT USB path", "transport")
        if candidates:
            candidate = candidates[0]
            access(candidate)
            selected = candidate
            break
        time.sleep(0.2)
    require(selected is not None, "Timed out waiting for DUT USB path", "transport")
    snapshot_dir = args.session.parent / "artifacts"
    snapshot_dir.mkdir(parents=True, exist_ok=True)
    snapshot = snapshot_dir / ("dut-" + uuid.uuid4().hex + ".elf")
    with snapshot.open("xb") as stream:
        stream.write(artifact.read_bytes())
    snapshot.chmod(0o400)
    # A single forced load performs the application-to-BOOTSEL handoff and
    # transfer atomically. A preceding forced info command would reboot the
    # DUT back to its application and race this load.
    command([PICOTOOL, "load", "-v", "-x", snapshot, "-f", "--bus", selected["bus"],
             "--address", selected["address"]], "transport", 30)
    deadline = time.monotonic() + args.timeout
    cdc_error = None
    while time.monotonic() < deadline:
        runtime = [d for d in usb_devices() if d["path"] == selected["path"]]
        if len(runtime) == 1:
            try:
                port = preferred_serial_port(runtime[0])
            except Failure as error:
                cdc_error = str(error)
                time.sleep(0.2)
                continue
            requested = getattr(args, "dut_port", None)
            require(not requested or Path(requested).resolve() == port.resolve(),
                    "DUT re-enumerated on a different serial port", "transport")
            record = dict(protocol=contract["protocol"], firmware_sha256=digest(snapshot),
                          artifact_snapshot=str(snapshot), usb_path=selected["path"],
                          serial_port=str(port), boot_id=boot_id())
            save(args.session.with_name("dut-session.json"), record)
            print("Validated DUT RAM session saved; use --dut-port " + str(port))
            return
        time.sleep(0.2)
    detail = ("; last observation: " + cdc_error) if cdc_error else ""
    raise Failure("transport", "DUT CDC did not become available after RAM load" + detail)


def validate_dut_session(m, args, artifact):
    contract = m.get("dut")
    if not contract:
        return None
    try:
        record = json.loads(args.session.with_name("dut-session.json").read_text())
    except (OSError, ValueError) as error:
        raise Failure("transport", "Missing/invalid DUT session; use load again.") from error
    require(record.get("protocol") == contract["protocol"]
            and record.get("firmware_sha256") == digest(artifact)
            and record.get("boot_id") == boot_id()
            and (not getattr(args, "dut_port", None)
                 or Path(args.dut_port).resolve() == Path(record["serial_port"]).resolve()),
            "Stale or mismatched DUT session; use load again.", "transport")
    port = Path(record["serial_port"])
    require(port.exists() and os.access(port, os.R_OK | os.W_OK),
            "DUT serial port is unavailable: " + str(port), "transport")
    return port


def validate_session(m, session, artifact, devices, profile):
    identity = provenance(artifact)
    require(
        session.get("build_identity") == identity,
        "Session build identity differs; rebuild/load current artifact.",
        "transport",
    )
    require(
        session.get("experiment") == m["id"]
        and session.get("flash_id") == profile["generator_flash_id"]
        and session.get("firmware_sha256") == digest(artifact)
        and session.get("boot_id") == boot_id(),
        "Stale or mismatched session; use load to validate the selected generator and current firmware.",
        "transport",
    )
    d = session.get("usb")
    require(
        d in devices and (d["vid"], d["pid"]) == ("2e8a", "000a"),
        "USB session changed; use load again. EEEE serial is never identity.",
        "transport",
    )
    return d


class Console:
    def __init__(self, path, log):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.log = log
        self.pending = b""
        try:
            fcntl.ioctl(self.fd, termios.TIOCEXCL)
            settings = termios.tcgetattr(self.fd)
            settings[0] = settings[1] = settings[3] = 0
            settings[2] = termios.CS8 | termios.CREAD | termios.CLOCAL | termios.HUPCL
            settings[4] = settings[5] = termios.B115200
            settings[6][termios.VMIN] = settings[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, settings)
            fcntl.ioctl(self.fd, termios.TIOCMBIC, struct.pack("I", termios.TIOCM_DTR))
            time.sleep(0.05)  # Give firmware a visible DTR-down epoch.
            termios.tcflush(self.fd, termios.TCIFLUSH)
            fcntl.ioctl(self.fd, termios.TIOCMBIS, struct.pack("I", termios.TIOCM_DTR))
        except BaseException:
            os.close(self.fd)
            raise

    def send(self, text):
        self.log.write("> " + text + "\n")
        self.log.flush()
        require(
            os.write(self.fd, (text + "\n").encode()) == len(text) + 1,
            "short serial write",
            "transport",
        )

    def line(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if b"\n" in self.pending:
                line, self.pending = self.pending.split(b"\n", 1)
                text = line.decode("ascii", errors="replace").strip()
                self.log.write("< " + text + "\n")
                self.log.flush()
                return text
            if select.select([self.fd], [], [], max(0, deadline - time.monotonic()))[0]:
                chunk = os.read(self.fd, 4096)
                require(chunk, "generator disconnected", "transport")
                self.pending += chunk
                require(
                    len(self.pending) <= 8192,
                    "oversized/malformed serial response",
                    "transport",
                )
        raise Failure("transport", "serial response timed out")

    def synchronize(self):
        # Greeting may contain an old result. A help response is a FIFO barrier;
        # discard everything through it before issuing a fresh run.
        self.line(3)
        self.send("stop")
        self.send("help")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if self.line(max(0.01, deadline - time.monotonic())).startswith(
                "commands: run"
            ):
                return
        raise Failure("transport", "generator command synchronization failed")

    def close(self, stop=True):
        try:
            if stop:
                self.send("stop")
        finally:
            try:
                fcntl.ioctl(
                    self.fd, termios.TIOCMBIC, struct.pack("I", termios.TIOCM_DTR)
                )
            finally:
                os.close(self.fd)


def fresh_completion(console, samples=16, timeout=3):
    console.send("run")
    running = False
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = console.line(max(0.01, deadline - time.monotonic()))
        if line == f"running samples={samples} nominal_hz=100000":
            running = True
        elif line == f"complete generated={samples} nominal_hz=100000":
            require(
                running,
                "completion arrived without fresh running acknowledgement",
                "transport",
            )
            return
        elif line.startswith(("error", "aborted", "stopped")):
            raise Failure("transport", "generator: " + line)
    raise Failure("transport", "fresh completion timed out")


def check_acquisition_log(text):
    require(
        not re.search(
            r"(?i)(failed to (?:claim|open|start)|\b(?:busy|denied)\b|"
            r"LIBUSB_(?:ERROR|TRANSFER_(?:ERROR|TIMED_OUT|STALL|NO_DEVICE|OVERFLOW))|"
            r"receive_transfer\(\): error)",
            text,
        ),
        "analyser failure; inspect acquisition.log",
        "acquisition",
    )


def await_acquisition(child, log, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = log.read_text(errors="replace")
        require(
            child.poll() is None,
            "analyser exited before run; inspect acquisition.log",
            "acquisition",
        )
        check_acquisition_log(text)
        # Successful non-empty transfer proves acquisition, not merely an
        # attempted driver start. Untriggered five-second window retains lead-in.
        if re.search(
            r"fx2lafw: receive_transfer\(\): status (?:LIBUSB_SUCCESS / LIBUSB_TRANSFER_COMPLETED|COMPLETED|0) received [1-9][0-9]* bytes",
            text,
        ):
            return
        time.sleep(0.02)
    raise Failure(
        "acquisition",
        "No successful analyser data transfer within 5s; refusing to drive outputs",
    )


def physical_run(m, args, artifact, dut_image=None):
    profile = read_profile(args.bench, required=True)
    acquisition_seconds, acquisition_samples = acquisition_parameters(
        m, getattr(args, "acquisition_seconds", None)
    )
    require(
        args.output is not None,
        "run/all requires --output NEW_DIRECTORY",
        "configuration",
    )
    require(
        not args.output.exists(),
        "Results directory already exists; choose a fresh --output",
        "configuration",
    )
    args.output.mkdir(parents=True)
    report = dict(experiment=m["id"], status="failed", category="transport")
    child = console = dut_console = None
    try:
        require(not m.get("dut") or dut_image is not None,
                "DUT contract requires a DUT firmware artifact", "configuration")
        session = json.loads(args.session.read_text())
        d = validate_session(m, session, artifact, usb_devices(), profile)
        dut_port = validate_dut_session(m, args, dut_image) if m.get("dut") else None
        require(
            not args.usb_path or args.usb_path == d["path"],
            "--usb-path differs from validated session",
            "transport",
        )
        save(args.output / "session.json", session)
        save(args.output / "manifest.json", m)
        save(args.output / "bench.json", profile)
        report["firmware_sha256"] = digest(artifact)
        report["argv"] = sys.argv
        report["build_identity"] = session["build_identity"]
        if dut_image is not None:
            report["dut_firmware_sha256"] = digest(dut_image)
        report["analyzer_version"] = command(
            ["sigrok-cli", "--version"], "environment"
        ).strip()
        samples, wiring, description = generator_run_description(m)
        print(f"Generator {session['flash_id']} on USB {d['path']}: {wiring}", flush=True)
        require(
            sys.stdin.isatty(),
            "Interactive terminal required for explicit Enter before outputs",
            "configuration",
        )
        input(
            "Check wiring. Press Enter to arm acquisition and emit one "
            f"{description} (Ctrl-C cancels): "
        )
        assert_profile(args, profile)
        d = validate_session(m, session, artifact, usb_devices(), profile)
        port = serial_port(d)
        require(dut_port is None or str(dut_port) != str(port),
                "Generator and DUT serial ports must be distinct", "transport")
        with (
            (args.output / "console.log").open("w") as serial_log,
            (args.output / "dut-console.log").open("w") as dut_log,
            (args.output / "acquisition.log").open("w") as acquisition_log,
        ):
            console = Console(port, serial_log)
            if dut_port is not None:
                dut_console = Console(dut_port, dut_log)
            try:
                console.synchronize()
                if dut_console is not None:
                    dut_reset(dut_console, m["dut"]["protocol"])
                cmd = [
                    "sigrok-cli",
                    "--loglevel",
                    "5",
                    "--driver",
                    args.analyzer,
                    "--config",
                    "samplerate=" + str(m["samplerate_hz"]),
                    "--channels",
                    ",".join(m["analyzer_channels"]),
                    "--samples",
                    str(acquisition_samples),
                    "--output-file",
                    str(args.output / "capture.sr"),
                ]
                print("+ " + shlex.join(cmd), flush=True)
                report["acquisition"] = dict(
                    argv=cmd,
                    duration_seconds=acquisition_seconds,
                    samples=acquisition_samples,
                    returncode=None,
                    termination=None,
                )
                child = subprocess.Popen(
                    cmd, stdout=acquisition_log, stderr=subprocess.STDOUT
                )
                await_acquisition(child, args.output / "acquisition.log")
                require(
                    child.poll() is None, "analyser stopped before run", "acquisition"
                )
                fresh_completion(console, samples)
                try:
                    rc = child.wait(timeout=10)
                except subprocess.TimeoutExpired as e:
                    raise Failure(
                        "acquisition", "analyser acquisition timed out"
                    ) from e
                report["acquisition"]["returncode"] = rc
                require(rc == 0, "analyser acquisition failed", "acquisition")
                check_acquisition_log(
                    (args.output / "acquisition.log").read_text(errors="replace")
                )
                report["waveform"] = analyse(args.output / "capture.sr", m)
                evidence = dut_report(dut_console, m["dut"]) if dut_console else None
                report.update(acceptance(m, evidence))
                if report["waveform"]["analysis_kind"] in ("burst", "held_active", "idle", "sampling_window", "repetition"):
                    import waveform_visual
                    visual = args.output / "waveform.svg"
                    waveform_visual.write_svg(report, visual)
                    report["waveform_visual"] = str(visual)
            finally:
                if console is not None:
                    try:
                        console.close()
                    except (OSError, Failure) as e:
                        report["cleanup_error"] = str(e)
                        if report["status"] in ("passed", "stimulus_passed"):
                            raise Failure(
                                "transport", "stop cleanup failed: " + str(e)
                            ) from e
                    console = None
                if dut_console is not None:
                    try:
                        dut_console.close(stop=False)
                    except OSError as e:
                        report["dut_cleanup_error"] = str(e)
                    dut_console = None
    except BaseException as e:
        report.update(
            status="failed",
            category=getattr(
                e,
                "category",
                (
                    "cancelled"
                    if isinstance(e, (KeyboardInterrupt, EOFError))
                    else "transport"
                ),
            ),
            error=str(e),
            details=getattr(e, "details", {}),
        )
        if "acquisition" in report:
            report["acquisition"]["termination"] = type(e).__name__ + ": " + str(e)
        raise
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        if child is not None:
            report["acquisition"]["returncode"] = child.returncode
        report["commands"] = list(COMMAND_LOG)
        save(args.output / "report.json", report)
        print(f"{report['status']}: evidence retained in {args.output}")


def configure_toolchain():
    try:
        bridge_setup.configure_toolchain(required=False)
    except ValueError as error:
        raise Failure("environment", str(error)) from error


def doctor(m, args):
    profile = read_profile(args.bench)
    print(
        f"Bench: {args.bench}; "
        + (
            f"generator {profile['generator_flash_id']}"
            if profile
            else configure_instruction(args.bench)
        )
    )
    print(
        "RP2040 USB mode IDs: 2e8a:0003 = BOOTSEL; 2e8a:000a = RAM CDC. These are not board identity."
    )
    configure_toolchain()
    problems = []
    for tool in (
        "cmake",
        "ninja",
        "ctest",
        "git",
        "arm-none-eabi-gcc",
        "sigrok-cli",
        "pkg-config",
    ):
        found = shutil.which(tool)
        print(f'{tool}: {found or "MISSING: install tool or source scripts/env.sh"}')
        if not found:
            problems.append(tool)
    modes = ["host", "stimulus"]
    if m.get("dut"):
        modes.append("firmware")
    for mode in modes:
        try:
            command(
                [sys.executable, "scripts/bootstrap.py", "--mode", mode, "--check"],
                "environment",
            )
        except Failure as e:
            problems.append(str(e))
    try:
        command(["pkg-config", "--exists", "libusb-1.0"], "environment")
    except Failure as e:
        problems.append("Install libusb development files: " + str(e))
    print(
        f'USB picotool: {PICOTOOL} ({"built" if PICOTOOL.exists() else "build stage will create it"})'
    )
    devices = [
        d
        for d in usb_devices()
        if d["vid"] == "2e8a"
        and d["pid"] in ("0003", "000a")
        and (not args.usb_path or d["path"] == args.usb_path)
    ]
    if not devices:
        print(
            "Generator absent: offline checks available; connect selected generator for load/run."
        )
    for d in devices:
        print("USB: " + json.dumps(d))
        try:
            access(d)
            if d["pid"] == "000a":
                print("CDC: " + str(serial_port(d)))
        except Failure as e:
            problems.append(str(e))
    if m.get("dut"):
        dut_connection_hints(m, args)
    try:
        scan = command(
            ["sigrok-cli", "--driver", args.analyzer, "--scan"], "acquisition", 10
        )
        require(
            "fx2lafw" in scan and "with 8 channels" in scan,
            "Analyser unavailable/busy: close PulseView, check cable and USB permissions.",
            "acquisition",
        )
    except Failure as e:
        problems.append(str(e))
    require(not problems, "\n".join(problems), "environment")


def print_report_summary(path):
    """Present the already-written authoritative report without re-analysing it."""
    import report_summary

    print(report_summary.format_report(json.loads(Path(path).read_text())))


def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__,
        epilog="Default all is interactive: build, load the manifest-selected generator and DUT, Enter, acquire/run/analyse, then print the saved report summary. Run configure-paths once to store stable USB topology paths locally. RAM experiments require BOOTSEL; a manifest may intentionally install a flash fixture. No sudo. Offline: build or analyse --capture FILE.",
    )
    p.add_argument(
        "stage",
        nargs="?",
        default="all",
        choices=["doctor", "build", "configure", "configure-paths", "load", "run", "analyse", "all"],
    )
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument(
        "--bench",
        type=Path,
        default=ROOT / ".bench/generator-check.json",
        help="machine-local board/analyzer/USB topology profile",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="print plan only; no device access, commands, build or output writes",
    )
    p.add_argument(
        "--output",
        type=Path,
        help="fresh directory for physical run evidence (default: unique bridge build/feasibility directory)",
    )
    p.add_argument(
        "--capture", type=Path, help="existing sigrok .sr for offline analysis"
    )
    p.add_argument(
        "--session",
        type=Path,
        help="generator load session (default: build/feasibility/EXPERIMENT/session.json)",
    )
    p.add_argument(
        "--usb-path",
        help="physical generator USB port (e.g. 1-2.3); overrides configured bench path",
    )
    p.add_argument("--dut-port", help="Core2350B USB CDC port, preferably /dev/serial/by-id/..." )
    p.add_argument("--dut-usb-path", help="Core2350B physical USB path; overrides configured bench path")
    p.add_argument(
        "--analyzer",
        default=None,
        help="sigrok driver, optionally fx2lafw:conn=BUS.ADDRESS to select one analyzer",
    )
    p.add_argument(
        "--acquisition-seconds",
        type=float,
        help="analyser capture duration; overrides manifest/default 0.25 seconds (0.01..60)",
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=60,
        help="bounded BOOTSEL/re-enumeration wait in seconds (1..300)",
    )
    args = p.parse_args(argv)
    COMMAND_LOG.clear()
    m = {}
    stage_error = None
    try:
        m = json.loads(args.manifest.read_text())
        require(
            m.get("status") == "implemented",
            f"{m.get('id')}: unimplemented. Read {args.manifest.parent/'README.md'} for prerequisites; no operations performed.",
            "unimplemented",
        )
        if args.session is None:
            args.session = ROOT / "build/feasibility" / m["id"] / "session.json"
        profile = None if args.stage in ("build", "analyse") else read_profile(args.bench)
        apply_profile_paths(args, profile)
        analyzer_override = args.analyzer
        args.analyzer = args.analyzer or (profile["analyzer"] if profile else "fx2lafw")
        require(
            re.fullmatch(r"fx2lafw(?::conn=[0-9]+\.[0-9]+)?", args.analyzer),
            "--analyzer must be fx2lafw or fx2lafw:conn=BUS.ADDRESS",
            "configuration",
        )
        require(
            1 <= args.timeout <= 300,
            "--timeout must be between 1 and 300 seconds",
            "configuration",
        )
        artifact = ROOT / "build" / m["preset"] / (m["target"] + ".elf")
        dut_image = dut_artifact(m)
        acquisition_seconds, acquisition_samples = acquisition_parameters(
            m, args.acquisition_seconds
        )
        if args.dry_run:
            print(
                json.dumps(
                    dict(
                        stage=args.stage,
                        bench=str(args.bench.resolve()),
                        generator_flash_id=(
                            profile["generator_flash_id"] if profile else None
                        ),
                        analyzer=args.analyzer,
                        usb_path=args.usb_path,
                        dut_usb_path=args.dut_usb_path,
                        manifest=str(args.manifest),
                        artifact=str(artifact),
                        dut_artifact=str(dut_image) if dut_image else None,
                        session=str(args.session),
                        output=str(args.output),
                        acquisition_seconds=acquisition_seconds,
                        acquisition_samples=acquisition_samples,
                        actions={
                            "doctor": "check pinned sources, tools, USB permissions and analyzer scan",
                            "build": "bootstrap pins; configure/build/test host Debug+Release; build manifest stimulus/DUT images and pinned USB picotool",
                            "configure": "read RP2040 BOOTSEL identity; confirm enrollment; atomically save local bench profile",
                            "configure-paths": "atomically save explicit generator/DUT USB topology paths in the local bench profile",
                            "load": "perform the manifest-selected generator load, then load the manifest DUT image and persist sessions",
                            "run": "validate sessions; reset DUT counters; explicit Enter; acquire/run/analyse; collect DUT report",
                            "analyse": "validate existing capture metadata and finite waveform",
                            "all": "build -> configure if missing -> load -> run/analyse -> print report summary",
                        }[args.stage],
                    ),
                    indent=2,
                )
            )
            return 0
        if args.stage in ("load", "run", "configure-paths"):
            read_profile(args.bench, required=True)
        if m.get("dut") and args.stage == "load":
            require(args.dut_usb_path,
                    "This experiment needs --dut-usb-path or a configured bench DUT path", "configuration")
        if m.get("dut") and args.stage == "all":
            require(args.dut_usb_path,
                    "This experiment needs --dut-usb-path or a configured bench DUT path; run configure-paths once after enrollment", "configuration")
        if args.stage in ("all", "run") and args.output is None:
            args.output = (
                ROOT
                / "build/feasibility"
                / m["id"]
                / (
                    datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-")
                    + uuid.uuid4().hex[:8]
                )
            )
        if args.stage in ("all", "run"):
            require(
                not args.output.exists(),
                "Supply --output NEW_DIRECTORY before any physical workflow",
                "configuration",
            )
        if args.stage == "all":
            static_prerequisites(analyzer=True)
        if args.stage in ("all", "build"):
            build(m)
        if args.stage in ("doctor",):
            doctor(m, args)
        if args.stage == "configure" or (args.stage == "all" and profile is None):
            selected = configure(args)
            if args.stage == "all":
                args.usb_path = selected["path"]
                profile = args.enrolled_profile
                assert_profile(args, profile)
                args.analyzer = analyzer_override or profile["analyzer"]
        if args.stage == "configure-paths":
            configure_paths(m, args)
        if args.stage in ("all", "load"):
            if profile is not None:
                assert_profile(args, profile)
            if m.get("dut"):
                require(args.dut_usb_path,
                        "This experiment needs --dut-usb-path or a configured bench DUT path", "configuration")
            load(m, args, artifact)
            load_dut(m, args, dut_image)
        if args.stage in ("all", "run"):
            assert_profile(args, profile)
            physical_run(m, args, artifact, dut_image)
            if args.stage == "all":
                print_report_summary(args.output / "report.json")
        if args.stage == "analyse":
            require(
                args.capture is not None,
                "analyse requires --capture FILE.sr",
                "configuration",
            )
            analysis_report = dict(
                status="failed", capture=str(args.capture), manifest=m
            )
            if args.output is not None:
                require(
                    not args.output.exists(),
                    "Choose fresh --output for analysis",
                    "configuration",
                )
                args.output.mkdir(parents=True)
            try:
                analysis_report.update(
                    **acceptance(m), waveform=analyse(args.capture, m)
                )
            except Failure as error:
                analysis_report.update(
                    status="failed",
                    category=error.category,
                    error=str(error),
                    details=error.details,
                )
                raise
            finally:
                if args.output is not None:
                    save(args.output / "report.json", analysis_report)
            print(json.dumps(analysis_report, indent=2))
        return 0
    except (Failure, OSError, ValueError, KeyError, KeyboardInterrupt, EOFError) as e:
        stage_error = str(e)
        print(
            json.dumps(
                dict(
                    status="failed",
                    category=getattr(
                        e,
                        "category",
                        (
                            "cancelled"
                            if isinstance(e, (KeyboardInterrupt, EOFError))
                            else "configuration"
                        ),
                    ),
                    error=str(e),
                    details=getattr(e, "details", {}),
                )
            ),
            file=sys.stderr,
        )
        return 130 if isinstance(e, KeyboardInterrupt) else 1
    finally:
        if COMMAND_LOG and not args.dry_run and m.get("status") == "implemented":
            logs = ROOT / "build/feasibility/stage-logs"
            logs.mkdir(parents=True, exist_ok=True)
            destination = logs / (
                datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-")
                + uuid.uuid4().hex[:8]
                + ".json"
            )
            save(
                destination,
                dict(
                    stage=args.stage,
                    manifest=m,
                    argv=sys.argv,
                    error=stage_error,
                    commands=COMMAND_LOG,
                ),
            )
            print(f"Stage command log: {destination}", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
