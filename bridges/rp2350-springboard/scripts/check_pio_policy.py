#!/usr/bin/env python3
"""Reject first-party PIO text files and assembler-generation build rules."""

import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
ROOT_EXCLUDED = {".deps", ".deps-backups", "build", ".git"}
GENERATED_EXCLUDED = {".pio", ".pio-core"}


def violations(root):
    found = []
    for directory, dirs, files in os.walk(root):
        # Generated PlatformIO products can contain .pio strings in their CMake
        # machinery. They are neither first-party PIO programs nor source. Keep
        # looking through a source directory named "build": policy fixtures use it.
        excluded = ROOT_EXCLUDED if Path(directory) == root else GENERATED_EXCLUDED
        dirs[:] = [name for name in dirs if name not in excluded]
        for name in files:
            path = Path(directory) / name
            relative = path.relative_to(root)
            if path.suffix.lower() == ".pio":
                found.append(f"{relative}: forbidden PIO text source")
            # Test fixtures and this guard discuss forbidden rules intentionally.
            build_rule = (
                path.name
                in {"CMakeLists.txt", "Makefile", "GNUmakefile", "CMakePresets.json"}
                or path.suffix.lower() in {".cmake", ".mk", ".sh", ".yml", ".yaml"}
                or (
                    path.suffix == ".py"
                    and relative.parts[0] != "tests"
                    and relative != Path("scripts/check_pio_policy.py")
                )
            )
            if build_rule and re.search(
                r"pioasm|pico_generate_pio_header|\.pio(?![-A-Za-z0-9_])",
                path.read_text(),
                re.IGNORECASE,
            ):
                found.append(f"{relative}: forbidden PIO text build workflow")
    return found


if __name__ == "__main__":
    errors = violations(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        sys.exit(1)
    print("PIO policy passed")
