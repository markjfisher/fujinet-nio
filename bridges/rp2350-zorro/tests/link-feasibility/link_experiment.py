#!/usr/bin/env python3
"""Build and prepare manifest-defined Story 2.3 link experiments.

This runner owns no production protocol.  It builds the two feasibility endpoint
images and records the commands/settings used for a later physical run.
"""
import argparse
import hashlib
import datetime
import select
import re
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
        "firmware": firmware_evidence(),
        "source": source_evidence(),
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


def wait_for_esp32_endpoint(manifest, dry_run):
    """Allow the freshly flashed ESP endpoint to create its link task.

    The RP2350 is loaded before the ESP32-S3 in ``all``.  That ordering matters
    for ESP-originated frames: a freshly booted producer starts at sequence one
    only after the RP2350's SPI pins have settled.  The delay is a manifest
    setting because endpoint initialization time is an experiment constraint.
    """
    wait_ms = int(manifest.get("run_profile", {}).get("esp_startup_wait_ms", 2000))
    if wait_ms < 0:
        raise ValueError("esp_startup_wait_ms must not be negative")
    print("Waiting {} ms for the ESP32-S3 link endpoint to start.".format(wait_ms))
    if not dry_run:
        time.sleep(wait_ms / 1000)


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


def sha256_file(path):
    """Return the content identity of a built image without loading it."""
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def file_evidence(path):
    """Describe an artifact even when a standalone run lacks a fresh build."""
    path = Path(path)
    result = {"path": str(path), "available": path.is_file()}
    if path.is_file():
        result.update({"bytes": path.stat().st_size, "sha256": sha256_file(path)})
    return result


def firmware_evidence():
    """Identify selected images; ``all`` builds and loads these before its run."""
    return {
        "rp2350_elf": file_evidence(ROOT / "build/link-rp2350/link_rp2350.elf"),
        "esp32_elf": file_evidence(ROOT / "lab/esp32-link/.pio/build/link-esp32s3/firmware.elf"),
        "esp32_bin": file_evidence(ROOT / "lab/esp32-link/.pio/build/link-esp32s3/firmware.bin"),
    }


def source_evidence():
    """Record repository revision and local dirty state without changing it."""
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              check=False)
    status = subprocess.run(["git", "status", "--short"], cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            check=False)
    return {
        "git_revision": revision.stdout.strip() if revision.returncode == 0 else None,
        "dirty_files": status.stdout.splitlines() if status.returncode == 0 else [],
    }


def open_raw_console(path):
    """Open an endpoint console as non-blocking raw bytes for evidence capture."""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    original = termios.tcgetattr(fd)
    raw = termios.tcgetattr(fd)
    raw[0] = raw[1] = raw[3] = 0
    raw[2] |= termios.CLOCAL | termios.CREAD
    raw[6][termios.VMIN] = 0
    raw[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, raw)
    return fd, original


def read_console_lines(fd, pending, lines):
    """Drain complete console lines while preserving an incomplete tail.

    ESP native USB disconnects during L7's intentional restart.  Its previous
    file descriptor can become readable with an I/O error before the runner
    closes it, which is evidence of that reset rather than a runner failure.
    """
    try:
        pending += os.read(fd, 4096)
    except OSError:
        return pending
    while b"\n" in pending:
        item, pending = pending.split(b"\n", 1)
        line = item.decode(errors="replace").strip()
        if line:
            lines.append(line)
    return pending


def restore_console(fd, original):
    termios.tcsetattr(fd, termios.TCSANOW, original)
    os.close(fd)


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
        if operation not in {"run", "oversize", "partial", "fault_partial", "pressure", "receive", "schedule", "batch", "soak", "rp_reset", "peer_reset"}:
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
        if operation in {"partial", "fault_partial"} and (not isinstance(slot_bytes, int) or not 0 < slot_bytes < 256):
            raise ValueError("partial case {} needs slot_bytes in 1..255".format(index))
        if operation == "rp_reset" and length != 0:
            raise ValueError("rp_reset case {} uses length 0".format(index))
        if operation == "peer_reset" and int(case.get("reboot_wait_ms", 2200)) < 0:
            raise ValueError("peer_reset case {} needs non-negative reboot_wait_ms".format(index))
        if operation == "batch":
            if not isinstance(case.get("count"), int) or not 0 < case["count"] <= 1000:
                raise ValueError("batch case {} needs count in 1..1000".format(index))
            if not isinstance(case.get("spi_hz"), int) or case["spi_hz"] <= 0:
                raise ValueError("batch case {} needs positive spi_hz".format(index))
        if operation == "soak":
            if not isinstance(case.get("cycles"), int) or not 0 < case["cycles"] <= 1000:
                raise ValueError("soak case {} needs cycles in 1..1000".format(index))
            if not isinstance(case.get("fault_period"), int) or case["fault_period"] <= 0:
                raise ValueError("soak case {} needs positive fault_period".format(index))
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
            "count": case.get("count"), "spi_hz": case.get("spi_hz"),
            "cycles": case.get("cycles"), "fault_period": case.get("fault_period"),
            "reboot_wait_ms": int(case.get("reboot_wait_ms", 2200)),
        })
    return result

def performance_summary(case_results):
    """Summarize batch result lines; experiment firmware remains the authority."""
    pattern = re.compile(r"status=batch_pass count=(?P<count>\d+) length=(?P<length>\d+) "
                         r"payload_bytes=(?P<bytes>\d+) elapsed_us=(?P<elapsed>\d+) spi_hz=(?P<hz>\d+)")
    groups = {}
    for case in case_results:
        match = pattern.search(case.get("result", ""))
        if not match:
            continue
        value = {key: int(raw) for key, raw in match.groupdict().items()}
        key = (value["hz"], value["length"])
        groups.setdefault(key, []).append(value["bytes"] * 1_000_000 / value["elapsed"])
    return [{"spi_hz": hz, "payload_bytes": length, "trials": len(rates),
             "rate_bytes_per_s_min": min(rates), "rate_bytes_per_s_mean": sum(rates) / len(rates),
             "rate_bytes_per_s_max": max(rates)}
            for (hz, length), rates in sorted(groups.items())]


def run_round_trip(manifest, args):
    cases = round_trip_cases(manifest)
    bench = read_bench()
    output = Path(args.output) if args.output else (ROOT / "build/link-feasibility" / manifest["id"] / (datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8]))
    output.mkdir(parents=True, exist_ok=False)
    rp_port, esp_port = bench["rp_port"], bench["esp_port"]
    wait_for_path(rp_port)
    wait_for_path(esp_port)
    rp_console_log = output / "console.log"
    esp_console_log = output / "esp32-console.log"
    capture = output / "capture.sr"
    analyzer = manifest["run_profile"].get("analyzer", {})
    sample_rate = int(analyzer.get("sample_rate_hz", 1000000))
    capture_ms = int(analyzer.get("capture_ms", 1000))
    channels = analyzer.get("channels", ["D0", "D1", "D2", "D3", "D4", "D5"])
    samples = sample_rate * capture_ms // 1000
    trigger = analyzer.get("trigger")
    sigrok = ["sigrok-cli", "--driver", "fx2lafw", "--config", f"samplerate={sample_rate}", "--channels", ",".join(channels)]
    if trigger:
        pretrigger_percent = int(analyzer.get("pretrigger_percent", 0))
        if not 0 <= pretrigger_percent <= 100:
            raise ValueError("analyzer pretrigger_percent must be 0..100")
        if pretrigger_percent:
            sigrok.extend(["--config", "captureratio={}".format(pretrigger_percent)])
        sigrok.extend(["--triggers", str(trigger), "--wait-trigger"])
    sigrok.extend(["--samples", str(samples), "--output-file", str(capture)])
    print("+ " + " ".join(sigrok))
    acquisition = subprocess.Popen(sigrok, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    arm_ms = int(analyzer.get("arm_ms", 1500))
    if arm_ms < 0:
        raise ValueError("analyzer arm_ms must not be negative")
    print("Waiting {} ms for the analyzer trigger capture to arm.".format(arm_ms))
    time.sleep(arm_ms / 1000)
    if acquisition.poll() is not None:
        analyzer_log, _ = acquisition.communicate()
        raise ValueError("analyzer exited before the experiment started: " + analyzer_log.strip())

    rp_fd = esp_fd = None
    rp_original = esp_original = None
    rp_lines, esp_lines = [], []
    case_results = []
    try:
        # Open both consoles before issuing the first command. RP output decides
        # the verdict; ESP output independently records the peer's slot work.
        rp_fd, rp_original = open_raw_console(rp_port)
        esp_fd, esp_original = open_raw_console(esp_port)
        rp_pending = esp_pending = b""
        for case in cases:
            if case["delay_before_ms"]:
                time.sleep(case["delay_before_ms"] / 1000)
            if case["operation"] == "rp_reset":
                # L6 resets the RAM-loaded master after a partial slot. Close
                # its CDC endpoint before picotool forces the reset and reload.
                restore_console(rp_fd, rp_original)
                rp_fd = None
                load_rp2350(manifest, False)
                wait_for_path(rp_port, seconds=10)
                time.sleep(0.5)
                rp_fd, rp_original = open_raw_console(rp_port)
                case_results.append(dict(case, result="result protocol=link-feasibility-v1 status=rp_reset_reloaded"))
                continue
            if case["operation"] == "receive":
                command_line = "receive {} {}".format(manifest["scenario"], case["sequence"])
            elif case["operation"] == "schedule":
                command_line = "schedule {} {} {} {} {}".format(
                    manifest["scenario"], case["outgoing_sequence"], case["sequence"],
                    case["length"], PATTERN_IDS[case["pattern"]])
            elif case["operation"] == "batch":
                command_line = "batch {} {} {} {} {} {}".format(
                    manifest["scenario"], case["sequence"], case["count"], case["length"],
                    PATTERN_IDS[case["pattern"]], case["spi_hz"])
            elif case["operation"] == "soak":
                command_line = "soak {} {} {} {}".format(
                    manifest["scenario"], case["sequence"], case["cycles"],
                    case["fault_period"])
            elif case["operation"] == "peer_reset":
                command_line = "peer_reset {} {}".format(manifest["scenario"], case["sequence"])
            else:
                command_line = "{} {} {} {} {}".format(
                    case["operation"], manifest["scenario"], case["length"],
                    PATTERN_IDS[case["pattern"]], case["sequence"])
            if case["operation"] in {"partial", "fault_partial"}:
                command_line += " {}".format(case["slot_bytes"])
            elif case["operation"] == "pressure":
                command_line += " {} {}".format(case["queue_depth"], case["pause_ms"])
            result_start = len(rp_lines)
            os.write(rp_fd, (command_line + "\n").encode())
            deadline = time.monotonic() + 5
            result = ""
            while time.monotonic() < deadline and not result:
                ready, _, _ = select.select([rp_fd, esp_fd], [], [], 0.1)
                if esp_fd in ready:
                    esp_pending = read_console_lines(esp_fd, esp_pending, esp_lines)
                if rp_fd in ready:
                    rp_pending = read_console_lines(rp_fd, rp_pending, rp_lines)
                    result = next((line for line in reversed(rp_lines[result_start:])
                                   if line.startswith("result protocol=link-feasibility-v1")), "")
            case_results.append(dict(case, result=result or "missing"))
            if case["operation"] == "peer_reset":
                # ESP reset disconnects its native USB console. Reopen it after
                # its documented boot interval so the recovery case is logged.
                restore_console(esp_fd, esp_original)
                esp_fd = None
                time.sleep(case["reboot_wait_ms"] / 1000)
                wait_for_path(esp_port, seconds=10)
                esp_fd, esp_original = open_raw_console(esp_port)

        # Give the ESP task a short opportunity to emit the peer-side outcome
        # for the last completed transfer before serial evidence is closed.
        deadline = time.monotonic() + 0.25
        while time.monotonic() < deadline:
            ready, _, _ = select.select([esp_fd], [], [], 0.05)
            if esp_fd in ready:
                esp_pending = read_console_lines(esp_fd, esp_pending, esp_lines)
    finally:
        if rp_fd is not None:
            restore_console(rp_fd, rp_original)
        if esp_fd is not None:
            restore_console(esp_fd, esp_original)

    try:
        analyser_log, _ = acquisition.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        acquisition.terminate()
        analyser_log, _ = acquisition.communicate(timeout=2)
    rp_console_log.write_text("\n".join(rp_lines) + "\n")
    esp_console_log.write_text("\n".join(esp_lines) + "\n")
    result = case_results[-1]["result"] if case_results else "missing"
    observation = analyze_capture(capture, ("SCLK", "MOSI", "MISO", "CS", "READY", "DATA_AVAILABLE"))
    passed_cases = all("status={}".format(case["expect_status"]) in case["result"]
                       for case in case_results)
    status = "passed" if passed_cases and acquisition.returncode == 0 and capture.is_file() else "failed"
    report = {
        "experiment": manifest["id"], "title": manifest["title"], "purpose": manifest["purpose"],
        "status": status, "rp2350_result": result, "round_trip_cases": case_results,
        "rp_console": str(rp_console_log), "esp32_console": str(esp_console_log),
        "capture": str(capture), "firmware": firmware_evidence(), "source": source_evidence(),
        "analyzer": {"sample_rate_hz": sample_rate, "capture_ms": capture_ms, "channels": channels,
                     "svg_max_transactions": analyzer.get("svg_max_transactions")},
        "analyzer_exit": acquisition.returncode, "analyzer_log": analyser_log,
        "performance": performance_summary(case_results),
        "analyzer_observation": observation, "bench": bench,
        "note": "Raw analyzer evidence is recorded; waveform decoding is not an independent verdict.",
    }
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
    for row in report["performance"]:
        print("Performance: {spi_hz} Hz, {payload_bytes} B, {trials} trials, "
              "{rate_bytes_per_s_mean:.0f} B/s mean ({rate_bytes_per_s_min:.0f}..{rate_bytes_per_s_max:.0f})".format(**row))
    print("RP2350 console: " + str(rp_console_log))
    print("ESP32 console: " + str(esp_console_log))
    if report.get("waveform_svg"):
        print("Waveform SVG: " + report["waveform_svg"])
    if status != "passed":
        raise ValueError("round-trip run did not produce passed RP2350 results and analyzer capture")


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
            # Bring the SPI master up before the ESP endpoint.  This prevents
            # RP2350 reboot pin transitions consuming ESP autonomous frames.
            load_rp2350(manifest, args.dry_run)
            load_esp32(manifest, args.dry_run)
            wait_for_esp32_endpoint(manifest, args.dry_run)
            if not args.dry_run: run_round_trip(manifest, args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("link experiment: " + str(error), file=sys.stderr)
        raise SystemExit(1)

if __name__ == "__main__":
    main()
