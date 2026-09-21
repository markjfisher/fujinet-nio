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
    print("link experiment runner tests passed")


if __name__ == "__main__":
    main()
