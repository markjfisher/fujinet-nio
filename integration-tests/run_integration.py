#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

try:
    import yaml  # PyYAML
except ImportError as e:
    raise SystemExit(
        "Missing dependency: PyYAML\n"
        "Install with:\n"
        "  python -m pip install PyYAML\n"
        "or add PyYAML to your dev dependencies.\n"
    ) from e


@dataclass
class Step:
    group: str
    name: str
    source_file: str
    argv: List[str]
    expect: List[str]
    forbid: List[str]
    timeout_s: float


def _default_endpoints(ip: str) -> tuple[str, str]:
    return (f"http://{ip}:8080", f"tcp://{ip}:7777")


def _load_yaml_file(path: Path) -> Dict[str, Any]:
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{path}: top-level must be a mapping (dict)")
    return data


def _discover_step_files(steps_dir: Path) -> List[Path]:
    if not steps_dir.exists():
        raise SystemExit(f"Steps directory not found: {steps_dir}")
    files = sorted([p for p in steps_dir.iterdir() if p.is_file() and p.suffix in (".yaml", ".yml")])
    if not files:
        raise SystemExit(f"No .yaml files found under: {steps_dir}")
    return files


def _expand_token(token: str, vars: Dict[str, str], cli_parts: List[str]) -> List[str]:
    # Substitute placeholders
    for k, v in vars.items():
        token = token.replace("{" + k + "}", v)

    # Expand {CLI} placeholder into multiple argv tokens
    if token == vars["CLI"]:
        return list(cli_parts)

    return [token]


def _expand_argv(argv_tpl: List[Any], vars: Dict[str, str], cli_parts: List[str]) -> List[str]:
    out: List[str] = []
    for tok in argv_tpl:
        tok_s = str(tok)
        out.extend(_expand_token(tok_s, vars, cli_parts))
    return out


def _compile_steps_from_file(path: Path, vars: Dict[str, str], cli_parts: List[str]) -> List[Step]:
    doc = _load_yaml_file(path)

    group = doc.get("group")
    steps = doc.get("steps")

    if not isinstance(group, str) or not group.strip():
        raise ValueError(f"{path}: missing/invalid 'group' (string)")
    if not isinstance(steps, list):
        raise ValueError(f"{path}: missing/invalid 'steps' (list)")

    out: List[Step] = []
    for i, s in enumerate(steps):
        if not isinstance(s, dict):
            raise ValueError(f"{path}: step {i} must be a mapping (dict)")

        name = s.get("name")
        argv_tpl = s.get("argv")
        expect = s.get("expect", [])
        forbid = s.get("forbid", [])
        timeout_s = float(s.get("timeout_s", 8.0))

        if not isinstance(name, str) or not name.strip():
            raise ValueError(f"{path}: step {i} missing/invalid 'name'")
        if not isinstance(argv_tpl, list) or not argv_tpl:
            raise ValueError(f"{path}: step {i} missing/invalid 'argv' list")
        if not isinstance(expect, list):
            raise ValueError(f"{path}: step {i} 'expect' must be a list")
        if not isinstance(forbid, list):
            raise ValueError(f"{path}: step {i} 'forbid' must be a list")

        argv = _expand_argv(argv_tpl, vars=vars, cli_parts=cli_parts)

        out.append(
            Step(
                group=group,
                name=name,
                source_file=path.name,
                argv=argv,
                expect=[str(p) for p in expect],
                forbid=[str(p) for p in forbid],
                timeout_s=timeout_s,
            )
        )

    return out


def run_step(step: Step, *, show_output_on_success: bool) -> bool:
    print(f"  -> {step.name}")
    print(f"     $ {' '.join(step.argv)}")

    try:
        cp = subprocess.run(
            step.argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=step.timeout_s,
            text=True,
        )
    except subprocess.TimeoutExpired:
        print("     TIMEOUT")
        return False

    out = cp.stdout or ""
    ok = True

    if cp.returncode != 0:
        ok = False
        print(out.rstrip())
        print(f"     FAIL: exit code {cp.returncode}")

    if ok:
        for pat in step.expect:
            if not re.search(pat, out, re.MULTILINE):
                ok = False
                print(out.rstrip())
                print(f"     FAIL: missing expected pattern: {pat!r}")
                break

    if ok:
        for pat in step.forbid:
            if re.search(pat, out, re.MULTILINE):
                ok = False
                print(out.rstrip())
                print(f"     FAIL: matched forbidden pattern: {pat!r}")
                break

    if ok:
        if show_output_on_success:
            print(out.rstrip())
        print("     OK")

    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description="Run end-to-end integration checks using scripts/fujinet CLI + YAML steps.")
    ap.add_argument("--port", required=True, help="POSIX PTY (/dev/pts/N) or serial port (/dev/ttyACM*)")

    ap.add_argument("--esp32", action="store_true", help="ESP32 mode: requires --ip for remote endpoints")
    ap.add_argument("--ip", default=None, help="Host IP for HTTP/TCP services (http://IP:8080, tcp://IP:7777)")

    ap.add_argument("--http-url", default=None, help="Override HTTP base URL (rare)")
    ap.add_argument("--tcp-url", default=None, help="Override TCP URL (rare)")

    ap.add_argument("--baud", default=None)
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--fs", default="host", help="Filesystem name for list test (default: host)")

    ap.add_argument(
        "--steps-dir",
        default=str(Path(__file__).with_name("steps")),
        help="Directory containing YAML step files (default: integration-tests/steps)",
    )

    ap.add_argument("--list", action="store_true", help="List discovered steps and exit")
    ap.add_argument("--only-file", action="append", default=[], help="Run only steps from YAML file name (repeatable)")
    ap.add_argument("--only-group", action="append", default=[], help="Run only groups matching substring (repeatable)")
    ap.add_argument("--only-step", action="append", default=[], help="Run only step names matching substring (repeatable)")

    ap.add_argument("--show-output", action="store_true", help="Show full output even on success")

    args = ap.parse_args()

    # Resolve endpoints
    if args.esp32:
        if not args.ip:
            raise SystemExit("--esp32 requires --ip <HOST_IP>")
        http_base, tcp_url = _default_endpoints(args.ip)
    else:
        ip = args.ip or "127.0.0.1"
        http_base, tcp_url = _default_endpoints(ip)

    if args.http_url:
        http_base = args.http_url.rstrip("/")
    if args.tcp_url:
        tcp_url = args.tcp_url

    # CLI prefix
    cli_parts = ["./scripts/fujinet", "-p", args.port]
    if args.baud:
        cli_parts += ["-b", str(args.baud)]
    if args.debug:
        cli_parts += ["--debug"]

    vars = {
        "CLI": " ".join(cli_parts),  # placeholder token to detect {CLI} expansion
        "HTTP": http_base,
        "TCP": tcp_url,
        "FS": args.fs,
        "PORT": args.port,
    }

    steps_dir = Path(args.steps_dir)
    step_files = _discover_step_files(steps_dir)

    all_steps: List[Step] = []
    for f in step_files:
        all_steps.extend(_compile_steps_from_file(f, vars=vars, cli_parts=cli_parts))

    # Filtering
    def match_any_substr(val: str, needles: List[str]) -> bool:
        if not needles:
            return True
        v = val.lower()
        return any(n.lower() in v for n in needles)

    if args.only_file:
        wanted = {x.strip() for x in args.only_file if x.strip()}
        all_steps = [s for s in all_steps if s.source_file in wanted]

    all_steps = [s for s in all_steps if match_any_substr(s.group, args.only_group)]
    all_steps = [s for s in all_steps if match_any_substr(s.name, args.only_step)]

    if args.list:
        print(f"Steps dir: {steps_dir}")
        print(f"Target port: {args.port}")
        print(f"HTTP: {http_base}")
        print(f"TCP:  {tcp_url}")
        print(f"FS:   {args.fs}")
        print("")
        for s in all_steps:
            print(f"- [{s.source_file}] {s.group} :: {s.name}")
        return 0

    print(f"Steps dir: {steps_dir}")
    print(f"Target port: {args.port}")
    print(f"HTTP: {http_base}")
    print(f"TCP:  {tcp_url}")
    print(f"FS:   {args.fs}")

    if not all_steps:
        print("No steps selected (filters excluded everything).")
        return 2

    ok_all = True
    current_group = None

    for step in all_steps:
        if step.group != current_group:
            current_group = step.group
            print(f"\n=== {current_group} ===")

        ok = run_step(step, show_output_on_success=args.show_output)
        ok_all = ok_all and ok

    if not ok_all:
        print("\nINTEGRATION TESTS: FAILED")
        return 1

    print("\nINTEGRATION TESTS: PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
