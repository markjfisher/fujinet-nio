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

ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / ".bench" / "link-feasibility.json"


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


def run_l0(manifest, args):
    if manifest["id"] != "L0":
        raise ValueError("automated physical run is implemented for L0 only")
    bench = read_bench()
    output = Path(args.output) if args.output else (ROOT / "build/link-feasibility" / manifest["id"] / (datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8]))
    output.mkdir(parents=True, exist_ok=False)
    rp_port = bench["rp_port"]
    wait_for_path(rp_port)
    console_log = output / "console.log"
    capture = output / "capture.sr"
    sigrok = ["sigrok-cli", "--driver", "fx2lafw", "--config", "samplerate=1000000", "--channels", "D0,D1,D2,D3,D4,D5", "--samples", "1000000", "--output-file", str(capture)]
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
        time.sleep(0.15)  # give the analyzer a bounded arm window
        os.write(fd, b"run 0 16 2\n")
        deadline = time.monotonic() + 5
        pending = b""
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if ready:
                pending += os.read(fd, 4096)
                while b"\n" in pending:
                    item, pending = pending.split(b"\n", 1)
                    line = item.decode(errors="replace").strip()
                    if line:
                        lines.append(line)
                        if line.startswith("result protocol=link-feasibility-v1"):
                            deadline = time.monotonic()
                            break
        termios.tcsetattr(fd, termios.TCSANOW, old)
    finally:
        os.close(fd)
    try:
        analyser_log, _ = acquisition.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        acquisition.terminate(); analyser_log, _ = acquisition.communicate(timeout=2)
    console_log.write_text("\n".join(lines) + "\n")
    result = next((line for line in lines if line.startswith("result protocol=link-feasibility-v1")), "")
    observation = analyze_capture(capture, ("SCLK", "MOSI", "MISO", "CS", "READY", "DATA_AVAILABLE"))
    status = "passed" if "status=passed" in result and acquisition.returncode == 0 and capture.is_file() else "failed"
    report = {"experiment": manifest["id"], "status": status, "rp2350_result": result or "missing", "rp_console": str(console_log), "capture": str(capture), "analyzer_exit": acquisition.returncode, "analyzer_log": analyser_log, "analyzer_observation": observation, "bench": bench, "note": "Raw analyzer evidence is recorded; L0 waveform decoding is not an independent verdict."}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("{}: evidence retained in {}".format(status, output))
    print(result or "no RP2350 result line")
    print(format_capture_observation(observation))
    if status != "passed": raise ValueError("L0 run did not produce a passed RP2350 result and analyzer capture")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--dry-run", action="store_true")
    sub = parser.add_subparsers(dest="stage", required=True)
    sub.add_parser("plan")
    sub.add_parser("build")
    sub.add_parser("doctor")
    sub.add_parser("load-rp2350")
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
        if args.stage == "run": run_l0(manifest, args)
        if args.stage == "all":
            if manifest["id"] != "L0":
                raise ValueError("automated physical run is implemented for L0 only")
            build(manifest, args.dry_run)
            load_rp2350(manifest, args.dry_run)
            if not args.dry_run: run_l0(manifest, args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("link experiment: " + str(error), file=sys.stderr)
        raise SystemExit(1)

if __name__ == "__main__":
    main()
