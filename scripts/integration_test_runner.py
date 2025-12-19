#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from typing import List, Optional


@dataclass
class Step:
    name: str
    argv: List[str]
    expect: List[str]
    forbid: List[str] = None
    timeout_s: float = 8.0


def run_step(step: Step) -> bool:
    forbid = step.forbid or []
    print(f"\n==> {step.name}")
    print(" ".join(step.argv))

    try:
        cp = subprocess.run(
            step.argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=step.timeout_s,
            text=True,
        )
    except subprocess.TimeoutExpired:
        print("TIMEOUT")
        return False

    out = cp.stdout or ""
    print(out.rstrip())

    if cp.returncode != 0:
        print(f"FAIL: exit code {cp.returncode}")
        return False

    for pat in step.expect:
        if not re.search(pat, out, re.MULTILINE):
            print(f"FAIL: missing expected pattern: {pat!r}")
            return False

    for pat in forbid:
        if re.search(pat, out, re.MULTILINE):
            print(f"FAIL: matched forbidden pattern: {pat!r}")
            return False

    print("OK")
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="POSIX PTY (e.g. /dev/pts/5) or serial port (/dev/ttyACM1)")
    ap.add_argument("--http-url", default="http://localhost:8080")
    ap.add_argument("--tcp-url", default="tcp://127.0.0.1:7777")
    ap.add_argument("--baud", default=None)
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--fs", default="host")
    args = ap.parse_args()

    base = ["./scripts/fujinet", "-p", args.port]
    if args.baud:
        base += ["-b", args.baud]
    if args.debug:
        base += ["--debug"]

    steps: List[Step] = []

    # ---- NET / HTTP ----
    steps.append(
        Step(
            name="HTTP: GET /get",
            argv=base + ["net", "get", f"{args.http_url}/get"],
            expect=[r"url", r"/get"],  # tolerate format changes
            timeout_s=10.0,
        )
    )

    steps.append(
        Step(
            name="HTTP: HEAD /headers",
            argv=base + ["net", "head", f"{args.http_url}/headers"],
            expect=[r"http_status", r"200|OK|Ok|status"],
            timeout_s=10.0,
        )
    )

    steps.append(
        Step(
            name="HTTP: POST /post (body)",
            argv=base + ["net", "send", "--method", "2", "--read-response", "--data", "hello", f"{args.http_url}/post"],
            expect=[r"\"data\": \"hello\""],
            timeout_s=10.0,
        )
    )

    # ---- NET / TCP ----
    # uses tcp sendrecv (non-interactive)
    steps.append(
        Step(
            name="TCP: sendrecv echo",
            argv=base + ["net", "tcp", "sendrecv", "--data", "ping", "--halfclose", "--idle-timeout", "0.5", args.tcp_url],
            expect=[r"ping"],
            timeout_s=10.0,
        )
    )

    # ---- CLOCK (lightweight sanity) ----
    # These patterns may need adjusting once you confirm CLI output format.
    # We keep them permissive: just ensure command runs and prints something time-like.
    steps.append(
        Step(
            name="Clock: get",
            argv=base + ["clock", "get"],
            expect=[r"\d{4}|\d{2}:\d{2}|\d+"],  # any time-ish output
            timeout_s=5.0,
        )
    )

    # ---- FILE (smoke) ----
    # This assumes file device supports listing root or similar.
    # If your CLI requires args, tweak the step to match your actual file CLI.
    steps.append(
        Step(
            name="File: list (root)",
            argv=base + ["list", args.fs, "/"],
            expect=[r"host|flash|sd|config|:/"],  # permissive
            timeout_s=8.0,
        )
    )

    ok = True
    for s in steps:
        ok = run_step(s) and ok

    if not ok:
        print("\nINTEGRATION TESTS: FAILED")
        return 1

    print("\nINTEGRATION TESTS: PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
