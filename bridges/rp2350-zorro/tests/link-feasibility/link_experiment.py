#!/usr/bin/env python3
"""Build and prepare manifest-defined Story 2.3 link experiments.

This runner owns no production protocol.  It builds the two feasibility endpoint
images and records the commands/settings used for a later physical run.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--dry-run", action="store_true")
    sub = parser.add_subparsers(dest="stage", required=True)
    sub.add_parser("plan")
    sub.add_parser("build")
    sub.add_parser("doctor")
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
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("link experiment: " + str(error), file=sys.stderr)
        raise SystemExit(1)

if __name__ == "__main__":
    main()
