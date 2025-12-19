#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import subprocess
from dataclasses import dataclass
from typing import List, Optional


@dataclass
class Step:
    name: str
    argv: List[str]
    expect: List[str]
    forbid: Optional[List[str]] = None
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


def _default_endpoints(ip: str) -> tuple[str, str]:
    # keep ports consistent with scripts/start_test_services.sh
    return (f"http://{ip}:8080", f"tcp://{ip}:7777")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Run end-to-end integration checks using scripts/fujinet CLI."
    )
    ap.add_argument(
        "--port",
        required=True,
        help="POSIX PTY (e.g. /dev/pts/5) or serial port (/dev/ttyACM1)",
    )

    # Mode selection
    ap.add_argument(
        "--esp32",
        action="store_true",
        help="ESP32 mode: requires --ip and assumes remote HTTP/TCP endpoints on that host",
    )
    ap.add_argument(
        "--ip",
        default=None,
        help="Host IP for HTTP/TCP test services (constructs http://IP:8080 and tcp://IP:7777)",
    )

    # Optional explicit overrides (rarely needed)
    ap.add_argument("--http-url", default=None, help="Override HTTP base URL (e.g. http://x:8080)")
    ap.add_argument("--tcp-url", default=None, help="Override TCP URL (e.g. tcp://x:7777)")

    # Serial / logging
    ap.add_argument("--baud", default=None)
    ap.add_argument("--debug", action="store_true")

    # Filesystem smoke test
    ap.add_argument("--fs", default="host", help="Filesystem name for list test (default: host)")

    args = ap.parse_args()

    # Resolve endpoints
    if args.http_url and not args.http_url.startswith("http"):
        raise SystemExit("--http-url must start with http:// or https://")
    if args.tcp_url and not args.tcp_url.startswith("tcp://"):
        raise SystemExit("--tcp-url must start with tcp://")

    if args.esp32:
        if not args.ip:
            raise SystemExit("--esp32 requires --ip <HOST_IP>")
        http_base, tcp_url = _default_endpoints(args.ip)
    else:
        # posix/dev default: localhost
        ip = args.ip or "127.0.0.1"
        http_base, tcp_url = _default_endpoints(ip)

    if args.http_url:
        http_base = args.http_url.rstrip("/")
    if args.tcp_url:
        tcp_url = args.tcp_url

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
            argv=base + ["net", "get", f"{http_base}/get"],
            expect=[r"url", r"/get"],
            timeout_s=10.0,
        )
    )

    steps.append(
        Step(
            name="HTTP: HEAD /headers",
            argv=base + ["net", "head", f"{http_base}/headers"],
            expect=[r"http_status", r"200|OK|Ok|status"],
            timeout_s=10.0,
        )
    )

    steps.append(
        Step(
            name="HTTP: POST /post (body)",
            argv=base + ["net", "send", "--method", "2", "--read-response", "--data", "hello", f"{http_base}/post"],
            expect=[r"\"data\": \"hello\""],
            timeout_s=10.0,
        )
    )

    # ---- NET / TCP ----
    steps.append(
        Step(
            name="TCP: sendrecv echo",
            argv=base + [
                "net",
                "tcp",
                "sendrecv",
                "--data",
                "ping",
                "--halfclose",
                "--idle-timeout",
                "0.5",
                tcp_url,
            ],
            expect=[r"ping"],
            timeout_s=10.0,
        )
    )

    # ---- CLOCK (lightweight sanity) ----
    steps.append(
        Step(
            name="Clock: get",
            argv=base + ["clock", "get"],
            expect=[r"\d{4}|\d{2}:\d{2}|\d+"],
            timeout_s=5.0,
        )
    )

    # ---- FILE (smoke) ----
    steps.append(
        Step(
            name="FS: list (root)",
            argv=base + ["list", args.fs, "/"],
            expect=[r":/|host|flash|sd|config"],
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
