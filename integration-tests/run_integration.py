#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
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
class CaptureConfig:
    mode: str = "text"  # "text" | "bytes" | "file"
    encoding: str = "utf-8"
    errors: str = "replace"
    out: Optional[str] = None  # explicit path, or None to use tempfile


@dataclass
class ExpectFileConfig:
    path: str  # "{CAPTURED_OUT}" or explicit path
    contains_text: List[str] = field(default_factory=list)
    contains_bytes_hex: List[str] = field(default_factory=list)
    sha256: Optional[str] = None
    size_min: Optional[int] = None
    size_exact: Optional[int] = None


@dataclass
class Step:
    group: str
    name: str
    source_file: str
    argv: List[str]
    expect: List[str]
    forbid: List[str]
    timeout_s: float
    only_mode: Optional[str] = None  # "posix" | "esp32" | None
    capture: CaptureConfig = field(default_factory=CaptureConfig)
    expect_file: Optional[ExpectFileConfig] = None
    setup_cmd: List[List[str]] = field(default_factory=list)
    expect_cmd: List[List[str]] = field(default_factory=list)
    expect_exit: int = 0


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


def _parse_capture_config(capture_dict: Optional[Dict[str, Any]]) -> CaptureConfig:
    """Parse capture configuration from YAML."""
    if capture_dict is None:
        return CaptureConfig()  # defaults

    if not isinstance(capture_dict, dict):
        raise ValueError("'capture' must be a mapping (dict)")

    mode = capture_dict.get("mode", "text")
    if mode not in ("text", "bytes", "file"):
        raise ValueError(f"capture.mode must be 'text', 'bytes', or 'file', got {mode!r}")

    return CaptureConfig(
        mode=mode,
        encoding=capture_dict.get("encoding", "utf-8"),
        errors=capture_dict.get("errors", "replace"),
        out=capture_dict.get("out"),
    )


def _parse_expect_file_config(expect_file_dict: Optional[Dict[str, Any]]) -> Optional[ExpectFileConfig]:
    """Parse expect_file configuration from YAML."""
    if expect_file_dict is None:
        return None

    if not isinstance(expect_file_dict, dict):
        raise ValueError("'expect_file' must be a mapping (dict)")

    path = expect_file_dict.get("path")
    if not isinstance(path, str):
        raise ValueError("expect_file.path must be a string")

    return ExpectFileConfig(
        path=path,
        contains_text=expect_file_dict.get("contains_text", []),
        contains_bytes_hex=expect_file_dict.get("contains_bytes_hex", []),
        sha256=expect_file_dict.get("sha256"),
        size_min=expect_file_dict.get("size_min"),
        size_exact=expect_file_dict.get("size_exact"),
    )


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
        only_mode = s.get("only_mode", None)
        timeout_s = float(s.get("timeout_s", 8.0))
        expect_exit = int(s.get("expect_exit", 0))

        if not isinstance(name, str) or not name.strip():
            raise ValueError(f"{path}: step {i} missing/invalid 'name'")
        if not isinstance(argv_tpl, list) or not argv_tpl:
            raise ValueError(f"{path}: step {i} missing/invalid 'argv' list")
        if not isinstance(expect, list):
            raise ValueError(f"{path}: step {i} 'expect' must be a list")
        if not isinstance(forbid, list):
            raise ValueError(f"{path}: step {i} 'forbid' must be a list")
        if only_mode is not None and only_mode not in ("posix", "esp32"):
            raise ValueError(f"{path}: step {i} only_mode must be 'posix' or 'esp32'")

        argv = _expand_argv(argv_tpl, vars=vars, cli_parts=cli_parts)

        # Parse capture config
        capture = _parse_capture_config(s.get("capture"))

        # Parse expect_file config
        expect_file = _parse_expect_file_config(s.get("expect_file"))

        # Parse expect_cmd
        expect_cmd_raw = s.get("expect_cmd", [])
        if not isinstance(expect_cmd_raw, list):
            raise ValueError(f"{path}: step {i} 'expect_cmd' must be a list")
        expect_cmd: List[List[str]] = []
        for j, cmd_item in enumerate(expect_cmd_raw):
            if not isinstance(cmd_item, dict):
                raise ValueError(f"{path}: step {i} expect_cmd[{j}] must be a mapping (dict)")
            cmd_argv = cmd_item.get("argv")
            if not isinstance(cmd_argv, list):
                raise ValueError(f"{path}: step {i} expect_cmd[{j}].argv must be a list")
            expect_cmd.append([str(x) for x in cmd_argv])

        # Parse setup_cmd
        setup_cmd_raw = s.get("setup_cmd", [])
        if not isinstance(setup_cmd_raw, list):
            raise ValueError(f"{path}: step {i} 'setup_cmd' must be a list")
        setup_cmd: List[List[str]] = []
        for j, cmd_item in enumerate(setup_cmd_raw):
            if not isinstance(cmd_item, dict):
                raise ValueError(f"{path}: step {i} setup_cmd[{j}] must be a mapping (dict)")
            cmd_argv = cmd_item.get("argv")
            if not isinstance(cmd_argv, list):
                raise ValueError(f"{path}: step {i} setup_cmd[{j}].argv must be a list")
            setup_cmd.append([str(x) for x in cmd_argv])


        out.append(
            Step(
                group=group,
                name=name,
                source_file=path.name,
                argv=argv,
                expect=[str(p) for p in expect],
                forbid=[str(p) for p in forbid],
                timeout_s=timeout_s,
                only_mode=only_mode,
                capture=capture,
                expect_file=expect_file,
                setup_cmd=setup_cmd,
                expect_cmd=expect_cmd,
                expect_exit=expect_exit,
            )
        )

    return out


def _has_out_flag(argv: List[str]) -> bool:
    """Check if argv already contains --out flag."""
    return "--out" in argv


def _inject_out_flag(argv: List[str], out_path: str) -> List[str]:
    """Inject --out flag into argv if not present and command supports it."""
    if _has_out_flag(argv):
        return argv

    # Commands that support --out: read, read-all, net get, net send, net tcp sendrecv
    # Check if it's a fujinet command that supports --out
    if len(argv) < 2 or argv[0] != "./scripts/fujinet":
        return argv

    # Check subcommand and determine if it needs --force
    subcmd_idx = None
    needs_force = False
    for i, arg in enumerate(argv):
        if arg in ("read", "read-all"):
            subcmd_idx = i
            needs_force = False  # file commands don't need --force
            break
        elif i > 0 and argv[i-1] in ("net",) and arg in ("get", "send"):
            subcmd_idx = i
            needs_force = True  # net get/send need --force to overwrite
            break
        elif i > 1 and argv[i-2] == "net" and argv[i-1] == "tcp" and arg == "sendrecv":
            subcmd_idx = i
            needs_force = True  # net tcp sendrecv needs --force to overwrite
            break

    if subcmd_idx is None:
        return argv

    # Insert --out (and --force if needed) after the subcommand
    injection = ["--out", out_path]
    if needs_force and "--force" not in argv:
        injection.append("--force")
    new_argv = argv[:subcmd_idx+1] + injection + argv[subcmd_idx+1:]
    return new_argv


def _validate_expect_file(
    file_path: Path, config: ExpectFileConfig, captured_out_path: Optional[str]
) -> Tuple[bool, str]:
    """Validate file expectations. Returns (ok, error_msg)."""
    # Expand {CAPTURED_OUT} placeholder
    if config.path == "{CAPTURED_OUT}":
        if captured_out_path is None:
            return False, "expect_file.path is {CAPTURED_OUT} but no file was captured"
        actual_path = Path(captured_out_path)
    else:
        actual_path = Path(config.path)

    if not actual_path.exists():
        return False, f"expect_file.path does not exist: {actual_path}"

    file_bytes = actual_path.read_bytes()
    file_size = len(file_bytes)

    # Size checks
    if config.size_exact is not None:
        if file_size != config.size_exact:
            return False, f"expect_file.size_exact: expected {config.size_exact}, got {file_size}"

    if config.size_min is not None:
        if file_size < config.size_min:
            return False, f"expect_file.size_min: expected >= {config.size_min}, got {file_size}"

    # SHA256 check
    if config.sha256:
        computed = hashlib.sha256(file_bytes).hexdigest()
        if computed.lower() != config.sha256.lower():
            return False, f"expect_file.sha256: expected {config.sha256}, got {computed}"

    # Text content checks
    if config.contains_text:
        file_text = file_bytes.decode("utf-8", errors="replace")
        for pattern in config.contains_text:
            if not re.search(pattern, file_text, re.MULTILINE):
                return False, f"expect_file.contains_text: pattern not found: {pattern!r}"

    # Binary content checks
    if config.contains_bytes_hex:
        for hex_str in config.contains_bytes_hex:
            try:
                search_bytes = bytes.fromhex(hex_str)
                if search_bytes not in file_bytes:
                    return False, f"expect_file.contains_bytes_hex: bytes not found: {hex_str}"
            except ValueError as e:
                return False, f"expect_file.contains_bytes_hex: invalid hex string {hex_str!r}: {e}"

    return True, ""


def run_step(
    step: Step, *, show_output_on_success: bool, keep_temp: bool = False, repo_root: Path
) -> bool:
    step_tmp = tempfile.TemporaryDirectory(prefix="fujinet-step-")
    step_tmp_path = step_tmp.name
    print(f"  -> {step.name}")
    print(f"     STEP_TMP={step_tmp_path}")

    # Determine if we need file capture
    needs_file_capture = step.capture.mode == "file" or step.expect_file is not None
    captured_out_path: Optional[str] = None
    temp_file: Optional[Path] = None

    # Handle file capture mode
    if needs_file_capture:
        if step.capture.out:
            captured_out_path = step.capture.out
        else:
            # Create temp file
            temp_fd, temp_path_str = tempfile.mkstemp(prefix="fujinet-test-", suffix=".bin")
            os.close(temp_fd)  # We'll open it via Path
            temp_file = Path(temp_path_str)
            captured_out_path = str(temp_file)

        # Inject --out if needed and possible
        if not _has_out_flag(step.argv):
            step_argv = _inject_out_flag(step.argv, captured_out_path)
            if step_argv == step.argv:
                # Command doesn't support --out, redirect stdout instead
                step_argv = step.argv
                # We'll handle stdout redirection in subprocess.run
        else:
            step_argv = step.argv
            # If --out is already present, extract it
            out_idx = step_argv.index("--out")
            if out_idx + 1 < len(step_argv):
                captured_out_path = step_argv[out_idx + 1]
    else:
        step_argv = step.argv

    # Run setup_cmd (if any)
    if step.setup_cmd:
        env = os.environ.copy()
        env["STEP_TMP"] = step_tmp_path
        # Provided by main() based on CLI args
        env["MODE"] = env.get("MODE", "")
        env["HOST_ROOT"] = env.get("HOST_ROOT", "")

        for cmd_argv in step.setup_cmd:
            expanded = []
            for a in cmd_argv:
                a = a.replace("{STEP_TMP}", step_tmp_path)
                a = a.replace("{HOST_ROOT}", env.get("HOST_ROOT", ""))
                a = a.replace("{MODE}", env.get("MODE", ""))
                expanded.append(a)

            try:
                scp = subprocess.run(
                    expanded,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    timeout=30.0,
                    text=True,
                    cwd=repo_root,
                    env=env,
                )
            except subprocess.TimeoutExpired:
                print(" FAIL: setup_cmd TIMEOUT")
                if not keep_temp:
                    step_tmp.cleanup()
                return False

            if scp.returncode != 0:
                print(f" FAIL: setup_cmd failed (exit {scp.returncode}): {' '.join(expanded)}")
                if scp.stdout:
                    print(scp.stdout.rstrip())
                if not keep_temp:
                    step_tmp.cleanup()
                return False

    # Run the command
    stdout_dest = subprocess.PIPE
    stderr_dest = subprocess.STDOUT
    stdout_file_handle = None

    # If file mode and --out injection failed, redirect stdout to file
    if needs_file_capture and not _has_out_flag(step_argv) and captured_out_path:
        stdout_file_handle = open(captured_out_path, "wb")
        stdout_dest = stdout_file_handle
        stderr_dest = subprocess.PIPE  # Keep stderr separate for error messages

    try:
        # Expand common placeholders for the main argv too (useful for bash -lc blocks)
        step_argv = [a.replace("{STEP_TMP}", step_tmp_path) for a in step_argv]
        step_argv = [a.replace("{HOST_ROOT}", os.environ.get("HOST_ROOT", "")) for a in step_argv]
        step_argv = [a.replace("{MODE}", os.environ.get("MODE", "")) for a in step_argv]
        print(f"     $ {' '.join(step_argv)}")
        cp = subprocess.run(
            step_argv,
            stdout=stdout_dest,
            stderr=stderr_dest,
            timeout=step.timeout_s,
            text=False,  # Always capture as bytes first
        )
    except subprocess.TimeoutExpired:
        print("     TIMEOUT")
        if temp_file and not keep_temp:
            try:
                temp_file.unlink()
            except Exception:
                pass
        return False
    finally:
        if stdout_file_handle:
            stdout_file_handle.close()

    # Decode output based on capture mode
    stdout_bytes = cp.stdout if isinstance(cp.stdout, bytes) else b""
    stderr_bytes = cp.stderr if isinstance(cp.stderr, bytes) else b""

    if step.capture.mode == "text":
        stdout_text = stdout_bytes.decode(step.capture.encoding, errors=step.capture.errors)
        stderr_text = stderr_bytes.decode(step.capture.encoding, errors=step.capture.errors)
        combined_text = stdout_text + stderr_text
    elif step.capture.mode == "bytes":
        # For bytes mode, create a hex dump representation for text matching
        combined_text = stdout_bytes.hex() + stderr_bytes.hex()
    else:  # file mode
        # Output went to file, decode stderr only for error messages
        combined_text = stderr_bytes.decode(step.capture.encoding, errors=step.capture.errors)

    ok = True
    failure_msg = ""

    # Check exit code
    if cp.returncode != step.expect_exit:
        ok = False
        failure_msg = f"exit code {cp.returncode} (expected {step.expect_exit})"

    # Text expectations (always check if provided, even in file mode we check stderr)
    if ok and step.expect:
        for pat in step.expect:
            if not re.search(pat, combined_text, re.MULTILINE):
                ok = False
                failure_msg = f"missing expected pattern: {pat!r}"
                break

    # Forbid patterns
    if ok and step.forbid:
        for pat in step.forbid:
            if re.search(pat, combined_text, re.MULTILINE):
                ok = False
                failure_msg = f"matched forbidden pattern: {pat!r}"
                break

    # File expectations
    if ok and step.expect_file:
        file_ok, file_error = _validate_expect_file(
            Path(step.expect_file.path), step.expect_file, captured_out_path
        )
        if not file_ok:
            ok = False
            failure_msg = file_error

    # Follow-on commands
    if ok and step.expect_cmd:
        for cmd_argv in step.expect_cmd:
            # Expand {CAPTURED_OUT} placeholder
            expanded_argv = []
            for arg in cmd_argv:
                arg = arg.replace("{STEP_TMP}", step_tmp_path)
                if captured_out_path:
                    arg = arg.replace("{CAPTURED_OUT}", captured_out_path)
                arg = arg.replace("{TEST_STEP_NAME}", step.name)
                expanded_argv.append(arg)

            env = os.environ.copy()
            if captured_out_path:
                env["TEST_CAPTURED_OUT"] = captured_out_path
            env["TEST_STEP_NAME"] = step.name

            try:
                cmd_cp = subprocess.run(
                    expanded_argv,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    timeout=30.0,
                    text=True,
                    cwd=repo_root,
                    env=env,
                )
                if cmd_cp.returncode != 0:
                    ok = False
                    failure_msg = f"expect_cmd failed (exit {cmd_cp.returncode}): {' '.join(expanded_argv)}"
                    if cmd_cp.stdout:
                        failure_msg += f"\n{cmd_cp.stdout}"
                    break
            except Exception as e:
                ok = False
                failure_msg = f"expect_cmd exception: {e}"
                break

    # Report results
    if not ok:
        print(f"     FAIL: {failure_msg}")
        if step.capture.mode != "file":
            print(combined_text.rstrip())
        if captured_out_path:
            cap_path = Path(captured_out_path)
            if cap_path.exists():
                print(f"     Captured file: {captured_out_path} ({cap_path.stat().st_size} bytes)")
        if temp_file and keep_temp:
            print(f"     Temp file preserved: {temp_file}")
    else:
        if show_output_on_success and step.capture.mode != "file":
            print(combined_text.rstrip())
        print("     OK")
        # Clean up temp file on success
        if temp_file and not keep_temp:
            try:
                temp_file.unlink()
            except Exception:
                pass
        if not keep_temp:
            step_tmp.cleanup()

    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description="Run end-to-end integration checks using scripts/fujinet CLI + YAML steps.")
    ap.add_argument("--port", required=True, help="POSIX PTY (/dev/pts/N) or serial port (/dev/ttyACM*)")

    ap.add_argument("--esp32", action="store_true", help="ESP32 mode: requires --ip for remote endpoints")
    ap.add_argument("--ip", default=None, help="Host IP for HTTP/TCP services (http://IP:8080, tcp://IP:7777)")
    ap.add_argument(
        "--host-root",
        default=None,
        help="POSIX-only: absolute path to the running fujinet-nio app's host FS root (the directory backing fs name 'host')",
    )

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
    ap.add_argument("--keep-temp", action="store_true", help="Keep temporary files even on success (for debugging)")

    args = ap.parse_args()
    mode = "esp32" if args.esp32 else "posix"
    repo_root = Path(__file__).parent.parent  # integration-tests/.. = repo root

    # POSIX convenience: auto-detect host root if not provided and there is exactly one build/*/fujinet-data.
    host_root = args.host_root
    if mode == "posix" and not host_root:
        build_dir = repo_root / "build"
        candidates: list[Path] = []
        if build_dir.exists():
            for p in build_dir.iterdir():
                if not p.is_dir():
                    continue
                if p.name == ".venv":
                    continue
                cand = p / "fujinet-data"
                if cand.exists() and cand.is_dir():
                    candidates.append(cand)
        if len(candidates) == 1:
            host_root = str(candidates[0].resolve())
            print(f"[auto] detected --host-root {host_root}")

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
        "MODE": mode,
        "HOST_ROOT": host_root or "",
    }
    # Export to environment so run_step() can pass through to setup_cmd and argv expansions.
    os.environ["MODE"] = mode
    os.environ["HOST_ROOT"] = vars["HOST_ROOT"]

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

    # Mode filtering
    all_steps = [s for s in all_steps if (s.only_mode is None or s.only_mode == mode)]

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

        ok = run_step(
            step,
            show_output_on_success=args.show_output,
            keep_temp=args.keep_temp,
            repo_root=repo_root,
        )
        ok_all = ok_all and ok

    if not ok_all:
        print("\nINTEGRATION TESTS: FAILED")
        return 1

    print("\nINTEGRATION TESTS: PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
