#!/usr/bin/env python3
"""Build and prepare manifest-defined Story 2.3 link experiments.

This runner owns no production protocol.  It builds the two feasibility endpoint
images and records the commands/settings used for a later physical run.
"""
import argparse
import hashlib
import datetime
import fcntl
import select
import termios
import time
import uuid
import json
import os
import subprocess
import sys
import zipfile
from pathlib import Path

from link_waveform import render as render_waveform

ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / ".bench" / "link-feasibility.json"
PATTERN_IDS = {"zero": 0, "ff": 1, "increment": 2, "alternating": 3,
               "fixed-random": 4, "slip-bytes": 5}


def load_manifest(path):
    manifest = json.loads(Path(path).read_text())
    required = ("id", "title", "scenario", "purpose", "rp2350", "esp32", "wiring")
    missing = [key for key in required if key not in manifest]
    if missing or manifest["scenario"] not in range(10):
        raise ValueError("invalid link manifest: " + ", ".join(missing or ["scenario must be 0..9"]))
    return manifest


def command(args, dry_run=False):
    print("+ " + " ".join(map(str, args)))
    if not dry_run:
        subprocess.run(args, cwd=ROOT, check=True)


def show_plan(manifest):
    print(f"{manifest['id']} — {manifest['title']}")
    print(manifest["purpose"])
    print("RP2350: " + ", ".join(f"{name}=GP{pin}" for name, pin in manifest["wiring"]["rp2350"].items()))
    print("ESP32-S3: " + ", ".join(f"{name}=GPIO{pin}" for name, pin in manifest["wiring"]["esp32s3"].items()))
    print("Analyzer: " + manifest["wiring"]["analyzer"])
    print("Run profile: " + json.dumps(manifest["run_profile"], sort_keys=True))


def build(manifest, dry_run):
    scenario = str(manifest["scenario"])
    command([str(ROOT / "scripts/build.sh"), "link-rp2350", "--link-scenario", scenario], dry_run)
    command([str(ROOT / "lab/esp32-link/build.sh"), "L" + scenario], dry_run)
    artifacts = {
        "experiment": manifest["id"], "scenario": manifest["scenario"],
        "rp2350_elf": str(ROOT / "build/link-rp2350/link_rp2350.elf"),
        "esp32_build": str(ROOT / "lab/esp32-link/.pio/build/link-esp32s3"),
        "manifest_sha256": hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest(),
        "status": "built",
    }
    output = ROOT / "build/link-feasibility" / manifest["id"] / "build.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(artifacts, indent=2) + "\n")
    print("Built endpoint images; build record: " + str(output))


def configure(args):
    current = json.loads(BENCH.read_text()) if BENCH.exists() else {}
    for key in ("rp_usb_path", "rp_port", "esp_usb_path", "esp_port"):
        value = getattr(args, key)
        if value:
            current[key] = value
    missing = [key for key in ("rp_usb_path", "rp_port", "esp_usb_path", "esp_port") if key not in current]
    if missing:
        raise ValueError("missing bench settings: " + ", ".join(missing))
    BENCH.parent.mkdir(parents=True, exist_ok=True)
    BENCH.write_text(json.dumps(current, indent=2) + "\n")
    print("Saved ignored bench settings: " + str(BENCH))


def doctor():
    """Show candidate ports without claiming a generic USB serial is unique."""
    by_id = Path("/dev/serial/by-id")
    print("Link-lab serial discovery (read-only):")
    if by_id.is_dir():
        entries = sorted(by_id.iterdir())
        if entries:
            for entry in entries:
                print(f"  {entry} -> {os.path.realpath(entry)}")
        else:
            print("  no /dev/serial/by-id entries")
    else:
        print("  /dev/serial/by-id is unavailable")
    for port in sorted(Path("/dev").glob("ttyACM*")):
        result = subprocess.run(
            ["udevadm", "info", "--query=property", "--name", str(port)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False,
        )
        props = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
        print("  {}: {}:{} serial={} interface={}".format(
            port, props.get("ID_VENDOR_ID", "?"), props.get("ID_MODEL_ID", "?"),
            props.get("ID_SERIAL_SHORT", "?"), props.get("ID_USB_INTERFACE_NUM", "?")))
    print("Enroll only the stable /dev/serial/by-id port that prints the lab ready line.")


def load_esp32(manifest, dry_run):
    """Install the selected lab endpoint through the enrolled ESP USB port."""
    bench = read_bench()
    wait_for_path(bench["esp_port"])
    print("Installing the selected ESP32-S3 lab image through the enrolled port.")
    command([str(ROOT / "lab/esp32-link/build.sh"), "L" + str(manifest["scenario"]),
             "--upload", bench["esp_port"]], dry_run)


def load_rp2350(manifest, dry_run):
    """Force-load the no-flash Core2350B image through the pinned USB picotool."""
    artifact = ROOT / "build/link-rp2350/link_rp2350.elf"
    picotool = ROOT / "build/picotool-usb/picotool"
    if not artifact.is_file():
        raise ValueError("build the link RP2350 image first: ./run.sh build")
    if not picotool.is_file():
        raise ValueError("build the USB picotool first: tests/feasibility/generator-check/run.sh build")
    print("Keep only the intended Core2350B connected as an RP-series USB target.")
    print("picotool force-reboots its compatible running firmware, loads SRAM, then starts it; flash is unchanged.")
    command([str(picotool), "load", "-v", "-x", "-f", str(artifact)], dry_run)


def read_bench():
    if not BENCH.is_file():
        raise ValueError("configure the local bench first: ./run.sh configure --rp-usb-path ... --rp-port ... --esp-usb-path ... --esp-port ...")
    value = json.loads(BENCH.read_text())
    required = ("rp_usb_path", "rp_port", "esp_usb_path", "esp_port")
    missing = [key for key in required if not isinstance(value.get(key), str) or not value[key]]
    if missing:
        raise ValueError("incomplete bench profile: " + ", ".join(missing))
    return value


def wait_for_path(path, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if Path(path).exists(): return
        time.sleep(0.1)
    raise ValueError("serial port did not appear: " + path)


def analyze_capture(path, signal_names):
    """Describe recorded logic levels; this is diagnostic evidence, not a verdict."""
    if not path.is_file():
        return {"available": False}
    try:
        with zipfile.ZipFile(path) as archive:
            parts = sorted(
                (entry for entry in archive.namelist() if entry.startswith("logic-1-")),
                key=lambda entry: int(entry.rsplit("-", 1)[1]),
            )
            samples = b"".join(archive.read(entry) for entry in parts)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        return {"available": False, "error": str(error)}
    if not samples:
        return {"available": False, "error": "no logic samples"}
    signals = {}
    for bit, name in enumerate(signal_names):
        levels = [(sample >> bit) & 1 for sample in samples]
        signals[name] = {
            "levels": sorted(set(levels)), "initial": levels[0], "final": levels[-1],
            "changes": sum(before != after for before, after in zip(levels, levels[1:])),
        }
    return {"available": True, "samples": len(samples), "signals": signals}


def format_capture_observation(observation):
    if not observation.get("available"):
        return "Analyzer: no readable capture"
    parts = []
    for name, value in observation["signals"].items():
        level = "high" if value["initial"] else "low"
        suffix = "static" if value["changes"] == 0 else f"{value['changes']} edges"
        parts.append(f"{name}={level}/{suffix}")
    return "Analyzer: " + ", ".join(parts)


def round_trip_cases(manifest):
    """Validate the small manifest command contract shared by L0 onward."""
    execution = manifest.get("execution") or {}
    if execution.get("kind") != "round_trip" or execution.get("automated") is not True:
        raise ValueError("this experiment does not yet declare an automated round-trip run")
    cases = manifest.get("run_profile", {}).get("cases")
    if not isinstance(cases, list) or not cases:
        raise ValueError("automated round-trip run requires one or more manifest cases")
    result = []
    for index, case in enumerate(cases, 1):
        if not isinstance(case, dict):
            raise ValueError("round-trip case {} is not an object".format(index))
        operation = case.get("operation", "run")
        length, pattern = case.get("length"), case.get("pattern")
        if operation not in {"run", "oversize", "partial", "pressure", "receive", "schedule"}:
            raise ValueError("unknown round-trip operation in case {}".format(index))
        if not isinstance(length, int) or length < 0 or pattern not in PATTERN_IDS:
            raise ValueError("invalid round-trip case {}".format(index))
        if operation == "schedule":
            if not isinstance(case.get("outgoing_sequence"), int) or case["outgoing_sequence"] < 1:
                raise ValueError("schedule case {} needs outgoing_sequence".format(index))
        if operation == "receive" and length != 0:
            raise ValueError("receive case {} uses length 0; its endpoint defines the payload".format(index))
        if operation == "run" and length > 240:
            raise ValueError("invalid normal-transfer length in case {}".format(index))
        if operation == "oversize" and length <= 240:
            raise ValueError("oversize case {} must exceed 240 bytes".format(index))
        slot_bytes = case.get("slot_bytes")
        if operation == "pressure":
            if case.get("queue_depth") not in {1, 2, 3, 4} or case.get("pause_ms") not in {0, 1, 10, 100}:
                raise ValueError("pressure case {} needs queue_depth 1..4 and pause_ms 0/1/10/100".format(index))
        if operation == "partial" and (not isinstance(slot_bytes, int) or not 0 < slot_bytes < 256):
            raise ValueError("partial case {} needs slot_bytes in 1..255".format(index))
        result.append({
            "id": str(case.get("id", "case-{}".format(index))),
            "operation": operation,
            "length": length,
            "pattern": pattern,
            "sequence": int(case.get("sequence", index)),
            "slot_bytes": slot_bytes,
            "delay_before_ms": int(case.get("delay_before_ms", 0)),
            "expect_status": str(case.get("expect_status", "passed")),
            "queue_depth": case.get("queue_depth"),
            "pause_ms": case.get("pause_ms"),
            "outgoing_sequence": case.get("outgoing_sequence"),
        })
    return result

def run_round_trip(manifest, args):
    cases = round_trip_cases(manifest)
    bench = read_bench()
    output = Path(args.output) if args.output else (ROOT / "build/link-feasibility" / manifest["id"] / (datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8]))
    output.mkdir(parents=True, exist_ok=False)
    rp_port = bench["rp_port"]
    wait_for_path(rp_port)
    console_log = output / "console.log"
    capture = output / "capture.sr"
    analyzer = manifest["run_profile"].get("analyzer", {})
    sample_rate = int(analyzer.get("sample_rate_hz", 1000000))
    capture_ms = int(analyzer.get("capture_ms", 1000))
    channels = analyzer.get("channels", ["D0", "D1", "D2", "D3", "D4", "D5"])
    samples = sample_rate * capture_ms // 1000
    trigger = analyzer.get("trigger")
    sigrok = ["sigrok-cli", "--driver", "fx2lafw", "--config", f"samplerate={sample_rate}", "--channels", ",".join(channels)]
    if trigger:
        sigrok.extend(["--triggers", str(trigger), "--wait-trigger"])
    sigrok.extend(["--samples", str(samples), "--output-file", str(capture)])
    print("+ " + " ".join(sigrok))
    acquisition = subprocess.Popen(sigrok, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    fd = os.open(rp_port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    lines = []
    try:
        old = termios.tcgetattr(fd)
        raw = termios.tcgetattr(fd)
        raw[0] = raw[1] = raw[3] = 0
        raw[2] |= termios.CLOCAL | termios.CREAD
        raw[6][termios.VMIN] = 0; raw[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, raw)
        time.sleep(0.5)  # let the analyzer subscribe before the first transaction
        pending = b""
        case_results = []
        for case in cases:
            if case["delay_before_ms"]:
                time.sleep(case["delay_before_ms"] / 1000)
            if case["operation"] == "receive":
                command_line = "receive {} {}".format(manifest["scenario"], case["sequence"])
            elif case["operation"] == "schedule":
                command_line = "schedule {} {} {} {} {}".format(
                    manifest["scenario"], case["outgoing_sequence"], case["sequence"],
                    case["length"], PATTERN_IDS[case["pattern"]])
            else:
                command_line = "{} {} {} {} {}".format(
                    case["operation"], manifest["scenario"], case["length"],
                    PATTERN_IDS[case["pattern"]], case["sequence"])
            if case["operation"] == "partial":
                command_line += " {}".format(case["slot_bytes"])
            elif case["operation"] == "pressure":
                command_line += " {} {}".format(case["queue_depth"], case["pause_ms"])
            os.write(fd, (command_line + "\n").encode())
            deadline = time.monotonic() + 5
            result = ""
            while time.monotonic() < deadline and not result:
                ready, _, _ = select.select([fd], [], [], 0.1)
                if ready:
                    pending += os.read(fd, 4096)
                    while b"\n" in pending:
                        item, pending = pending.split(b"\n", 1)
                        line = item.decode(errors="replace").strip()
                        if line:
                            lines.append(line)
                            if line.startswith("result protocol=link-feasibility-v1"):
                                result = line
                                break
            case_results.append(dict(case, result=result or "missing"))
        termios.tcsetattr(fd, termios.TCSANOW, old)
    finally:
        os.close(fd)
    try:
        analyser_log, _ = acquisition.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        acquisition.terminate(); analyser_log, _ = acquisition.communicate(timeout=2)
    console_log.write_text("\n".join(lines) + "\n")
    result = case_results[-1]["result"] if case_results else "missing"
    observation = analyze_capture(capture, ("SCLK", "MOSI", "MISO", "CS", "READY", "DATA_AVAILABLE"))
    passed_cases = all("status={}".format(case["expect_status"]) in case["result"]
                       for case in case_results)
    status = "passed" if passed_cases and acquisition.returncode == 0 and capture.is_file() else "failed"
    report = {"experiment": manifest["id"], "title": manifest["title"], "purpose": manifest["purpose"], "status": status, "rp2350_result": result, "round_trip_cases": case_results, "rp_console": str(console_log), "capture": str(capture), "analyzer": {"sample_rate_hz": sample_rate, "capture_ms": capture_ms, "channels": channels}, "analyzer_exit": acquisition.returncode, "analyzer_log": analyser_log, "analyzer_observation": observation, "bench": bench, "note": "Raw analyzer evidence is recorded; L0 waveform decoding is not an independent verdict."}
    if capture.is_file():
        waveform_svg = output / "waveform.svg"
        try:
            report["waveform_svg"] = str(waveform_svg)
            report["waveform_transactions"] = render_waveform(report, capture, waveform_svg)
        except ValueError as error:
            report["waveform_svg_error"] = str(error)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("{}: evidence retained in {}".format(status, output))
    print("Round-trip cases: {}/{} met expected status".format(
        sum("status={}".format(case["expect_status"]) in case["result"] for case in case_results),
        len(case_results)))
    print(result or "no RP2350 result line")
    print(format_capture_observation(observation))
    if report.get("waveform_svg"):
        print("Waveform SVG: " + report["waveform_svg"])
    if status != "passed": raise ValueError("round-trip run did not produce passed RP2350 results and analyzer capture")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--dry-run", action="store_true")
    sub = parser.add_subparsers(dest="stage", required=True)
    sub.add_parser("plan")
    sub.add_parser("build")
    sub.add_parser("doctor")
    sub.add_parser("load-rp2350")
    sub.add_parser("load-esp32")
    run = sub.add_parser("run")
    run.add_argument("--output")
    all_stage = sub.add_parser("all")
    all_stage.add_argument("--output")
    config = sub.add_parser("configure")
    config.add_argument("--rp-usb-path")
    config.add_argument("--rp-port")
    config.add_argument("--esp-usb-path")
    config.add_argument("--esp-port")
    args = parser.parse_args()
    try:
        if args.stage == "configure":
            configure(args)
            return
        manifest = load_manifest(args.manifest)
        if args.stage == "plan": show_plan(manifest)
        if args.stage == "build": build(manifest, args.dry_run)
        if args.stage == "doctor": doctor()
        if args.stage == "load-rp2350": load_rp2350(manifest, args.dry_run)
        if args.stage == "load-esp32": load_esp32(manifest, args.dry_run)
        if args.stage == "run": run_round_trip(manifest, args)
        if args.stage == "all":
            build(manifest, args.dry_run)
            load_esp32(manifest, args.dry_run)
            load_rp2350(manifest, args.dry_run)
            if not args.dry_run: run_round_trip(manifest, args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("link experiment: " + str(error), file=sys.stderr)
        raise SystemExit(1)

if __name__ == "__main__":
    main()
