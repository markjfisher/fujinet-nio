#!/usr/bin/env python3
"""Fetch exact source pins, or validate existing checkouts without modifying them."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def validate(path, pin):
    guidance = f"Preserve local work, remove {path}, then rerun bootstrap (or supply a clean pinned SDK)."
    try:
        if Path(git(path, "rev-parse", "--show-toplevel")).resolve() != path.resolve():
            raise ValueError("not a standalone checkout")
        actual = git(path, "rev-parse", "HEAD")
        if actual != pin["revision"]:
            raise ValueError(f"revision {actual}; expected {pin['revision']}")
        if git(path, "status", "--porcelain", "--untracked-files=all", "--ignore-submodules=none"):
            raise ValueError("dirty source checkout")
        for submodule in pin.get("submodules", []):
            state = subprocess.check_output(["git", "-C", str(path), "submodule", "status", "--recursive", "--", submodule], text=True)
            if not state or any(line[0] != " " for line in state.splitlines()):
                raise ValueError(f"missing or wrong submodule {submodule}")
    except (subprocess.CalledProcessError, ValueError) as exc:
        raise ValueError(f"Invalid dependency {path}: {exc}. {guidance}") from exc


def setup(path, pin, check_only=False):
    if not path.exists():
        if check_only:
            raise ValueError(f"Missing dependency {path}; run python3 scripts/bootstrap.py --mode host or --mode firmware.")
        path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--quiet", "--depth", "1", "--branch", pin["version"], pin["url"], str(path)], check=True)
        # Verify the tag before initializing any dependent sources.
        if git(path, "rev-parse", "HEAD") != pin["revision"]:
            raise ValueError(f"Tag revision mismatch at {path}; remove checkout after inspection.")
        if pin.get("submodules"):
            subprocess.run(["git", "-C", str(path), "submodule", "update", "--init", "--recursive", "--depth", "1", "--", *pin["submodules"]], check=True)
    validate(path, pin)
    print(f"Validated {path.name}: {pin['revision']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=["host", "firmware"], required=True)
    parser.add_argument("--check", action="store_true", help="validate only; never download")
    parser.add_argument("--sdk-path", type=Path)
    args = parser.parse_args()
    pins = json.loads((ROOT / "dependencies.json").read_text())
    names = ["apio", "epio"] if args.mode == "host" else ["apio", "pico-sdk", "picotool"]
    for name in names:
        sdk_override = args.sdk_path or os.environ.get("PICO_SDK_PATH")
        override = name == "pico-sdk" and sdk_override
        path = Path(sdk_override).resolve() if override else ROOT / ".deps" / name
        setup(path, pins[name], args.check or bool(override))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, subprocess.CalledProcessError) as error:
        print(f"bootstrap: {error}", file=sys.stderr)
        sys.exit(1)
