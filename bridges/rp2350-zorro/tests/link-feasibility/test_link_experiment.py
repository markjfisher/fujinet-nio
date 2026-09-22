#!/usr/bin/env python3
"""Cheap manifest and dry-run checks for the Story 2.3 lab runner."""
import importlib.util
import os
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
    # Autonomous endpoints must be restarted only after the RP2350 image is
    # live; this settle interval covers ESP task creation before the first slot.
    l4_manifest = runner.load_manifest(manifests[4])
    assert l4_manifest["run_profile"]["esp_startup_wait_ms"] == 2000
    assert l4_manifest["run_profile"]["analyzer"]["arm_ms"] == 1500
    runner.wait_for_esp32_endpoint(l4_manifest, dry_run=True)
    l5_cases = runner.round_trip_cases(runner.load_manifest(manifests[5]))
    assert all(case["operation"] == "schedule" for case in l5_cases)
    assert [case["outgoing_sequence"] for case in l5_cases] == [1, 2, 3, 4, 5, 6]
    l6_cases = runner.round_trip_cases(runner.load_manifest(manifests[6]))
    assert [case["operation"] for case in l6_cases] == ["fault_partial", "rp_reset", "run"]
    l7_cases = runner.round_trip_cases(runner.load_manifest(manifests[7]))
    assert [case["operation"] for case in l7_cases] == ["peer_reset", "run"]
    l8_cases = runner.round_trip_cases(runner.load_manifest(manifests[8]))
    assert len(l8_cases) == 51
    assert all(case["operation"] == "batch" and case["count"] == 50 for case in l8_cases)
    l8_cells = {(case["spi_hz"], case["length"]) for case in l8_cases}
    assert {(1_000_000, 16), (4_000_000, 64), (8_000_000, 240)} <= l8_cells
    assert {(hz, length) for hz in (5_000_000, 6_000_000, 7_000_000, 8_000_000)
            for length in (64, 240)} <= l8_cells
    l9_cases = runner.round_trip_cases(runner.load_manifest(manifests[9]))
    assert l9_cases[0]["operation"] == "soak"
    assert (l9_cases[0]["cycles"], l9_cases[0]["fault_period"]) == (100, 10)
    runner.show_plan(runner.load_manifest(manifests[0]))
    with tempfile.TemporaryDirectory() as directory:
        artifact = Path(directory) / "firmware.elf"
        artifact.write_bytes(b"link-lab-artifact")
        identity = runner.file_evidence(artifact)
        assert identity["available"] and identity["bytes"] == len(b"link-lab-artifact")
        assert identity["sha256"] == __import__("hashlib").sha256(b"link-lab-artifact").hexdigest()
        assert not runner.file_evidence(Path(directory) / "missing.bin")["available"]
    read_fd, write_fd = os.pipe()
    try:
        os.write(write_fd, b"first\nsecond\npartial")
        lines = []
        pending = runner.read_console_lines(read_fd, b"", lines)
        assert lines == ["first", "second"] and pending == b"partial"
    finally:
        os.close(read_fd)
        os.close(write_fd)
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
    import link_waveform
    representative, omitted = link_waveform.displayed_rows(
        [{"role": "request"}] * 20, 12)
    assert [index for index, _row in representative] == [1, 2, 3, 4, 5, 6, 15, 16, 17, 18, 19, 20]
    assert omitted == 8
    compressed_path = link_waveform._path(
        [0, 1, 0], 0, 2, 0, lambda _sample: 10, 1, 2)
    assert compressed_path.endswith("V 2.00 H 10.00")
    timing_rows = [
        {"role": "request", "start_sample": 10, "end_sample": 20},
        {"role": "echo", "start_sample": 30, "end_sample": 40},
        {"role": "request", "start_sample": 50, "end_sample": 60},
        {"role": "echo", "start_sample": 90, "end_sample": 100},
    ]
    timing_words = [0b00010000] * 110
    timing_words[21:25] = [0] * 4
    timing_words[61:85] = [0] * 24
    link_waveform.annotate_timing(timing_rows, {"round_trip_cases": [
        {"id": "baseline", "pause_ms": 0}, {"id": "pause", "pause_ms": 10}
    ]}, timing_words, 1_000)
    assert timing_rows[1]["timing_detail"] == "READY low 4.000 ms (baseline endpoint overhead)"
    assert ("READY low 24.000 ms = 4.000 ms baseline + 20.000 ms injected; "
            "requested 10 ms") == timing_rows[3]["timing_detail"]
    print("link experiment runner tests passed")


if __name__ == "__main__":
    main()
