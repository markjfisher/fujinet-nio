#!/usr/bin/env python3
"""Cheap manifest and dry-run checks for the Story 2.3 lab runner."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("link_experiment", HERE / "link_experiment.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def main():
    manifests = sorted(HERE.glob("L*/experiment.json"))
    assert len(manifests) == 10
    for expected, path in enumerate(manifests):
        manifest = runner.load_manifest(path)
        assert manifest["scenario"] == expected
        assert (path.parent / "run.sh").is_file()
        assert (path.parent / "README.md").is_file()
        assert manifest["wiring"]["rp2350"]["SCLK"] == 2
        assert manifest["wiring"]["esp32s3"]["SCLK"] == 12
    assert runner.load_manifest(manifests[0])["run_profile"]["analyzer"]["sample_rate_hz"] == 12000000
    l0_cases = runner.round_trip_cases(runner.load_manifest(manifests[0]))
    l1_cases = runner.round_trip_cases(runner.load_manifest(manifests[1]))
    assert len(l0_cases) == 1
    assert len(l1_cases) == 12
    assert l1_cases[0]["length"] == 0 and l1_cases[-1]["length"] == 240
    l2_cases = runner.round_trip_cases(runner.load_manifest(manifests[2]))
    assert [case["operation"] for case in l2_cases] == [
        "run", "run", "run", "run", "oversize", "partial", "run", "run"
    ]
    assert l2_cases[4]["expect_status"] == "oversize_rejected"
    assert l2_cases[5]["slot_bytes"] == 32
    l3_cases = runner.round_trip_cases(runner.load_manifest(manifests[3]))
    assert {case["operation"] for case in l3_cases} == {"pressure", "run"}
    assert {case["pause_ms"] for case in l3_cases if case["operation"] == "pressure"} == {0, 1, 10, 100}
    l4_cases = runner.round_trip_cases(runner.load_manifest(manifests[4]))
    assert all(case["operation"] == "receive" for case in l4_cases)
    assert all(case["expect_status"] == "received" for case in l4_cases)
    l5_cases = runner.round_trip_cases(runner.load_manifest(manifests[5]))
    assert all(case["operation"] == "schedule" for case in l5_cases)
    assert [case["outgoing_sequence"] for case in l5_cases] == [1, 2, 3, 4, 5, 6]
    runner.show_plan(runner.load_manifest(manifests[0]))
    with tempfile.TemporaryDirectory() as directory:
        capture = Path(directory) / "capture.sr"
        with zipfile.ZipFile(capture, "w") as archive:
            archive.writestr("logic-1-1", bytes((0b00001000, 0b00000000, 0b00000001)))
        observed = runner.analyze_capture(capture, ("SCLK", "MOSI", "MISO", "CS"))
        assert observed["available"]
        assert observed["signals"]["CS"]["changes"] == 1
        assert observed["signals"]["SCLK"]["initial"] == 0
        assert "CS=high/1 edges" in runner.format_capture_observation(observed)
    frame = bytearray(256)
    struct.pack_into("<I", frame, 0, 0x4C4E4B31)
    frame[4:7] = bytes((1, 0, 0))
    struct.pack_into("<IHH", frame, 8, 1, 16, 0xD0A7)
    samples = [0b001000]
    def slot(mosi, miso):
        samples.append(0)
        for transmit, receive in zip(mosi, miso):
            for shift in range(7, -1, -1):
                value = ((transmit >> shift) & 1) << 1 | ((receive >> shift) & 1) << 2
                samples.extend((value, value | 1, value))
        samples.append(0b001000)
    slot(frame, bytes(256))
    slot(bytes(256), frame)
    with tempfile.TemporaryDirectory() as directory:
        capture = Path(directory) / "capture.sr"
        with zipfile.ZipFile(capture, "w") as archive:
            archive.writestr("metadata", "[device 1]\nunitsize=1\n")
            archive.writestr("logic-1-1", bytes(samples))
        output = Path(directory) / "waveform.svg"
        rows = runner.render_waveform({"experiment": "L0", "title": "Fixed packet bring-up",
                                       "purpose": "test", "analyzer": {"sample_rate_hz": 12_000_000}}, capture, output)
        text = output.read_text()
        assert [row["role"] for row in rows] == ["request", "echo"]
        assert "DATA_AVAILABLE" in text and "MOSI: L0 seq 1 16 B status 0 CRC D0A7" in text
        assert "E* = response to an incomplete triggered request." in text
        assert 'id="link-request"' in text and 'fill="url(#link-echo)"' in text
        # A /CS trigger can begin while the first slot is already active. The
        # incomplete leading slot must be ignored, not offset every later pair.
        triggered = samples[2:]  # begins after the request's CS fall
        capture = Path(directory) / "triggered.sr"
        with zipfile.ZipFile(capture, "w") as archive:
            archive.writestr("metadata", "[device 1]\nunitsize=1\n")
            archive.writestr("logic-1-1", bytes(triggered))
        rows = runner.render_waveform(
            {"experiment": "L1", "title": "triggered", "purpose": "test",
             "analyzer": {"sample_rate_hz": 12_000_000}}, capture,
            Path(directory) / "triggered.svg")
        assert [row["role"] for row in rows] == ["unpaired echo"]
    timing_rows = [
        {"role": "request", "start_sample": 10, "end_sample": 20},
        {"role": "echo", "start_sample": 50, "end_sample": 60},
    ]
    timing_words = [0b00010000] * 80
    timing_words[21:45] = [0] * 24
    import link_waveform
    link_waveform.annotate_timing(
        timing_rows, {"round_trip_cases": [{"id": "pause", "pause_ms": 10}]},
        timing_words, 1_000)
    assert timing_rows[0]["timing_detail"] == "Case pause: requested receiver pause 10 ms"
    assert "READY low 24.000 ms; requested >= 10 ms" == timing_rows[1]["timing_detail"]
    print("link experiment runner tests passed")


if __name__ == "__main__":
    main()
