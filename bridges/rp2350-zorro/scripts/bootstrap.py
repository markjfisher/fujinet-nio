#!/usr/bin/env python3
"""Fetch exact source pins, or validate existing checkouts without modifying them."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid
import shlex

ROOT = Path(__file__).resolve().parents[1]


def git(path, *args):
    return subprocess.check_output(
        ["git", "--no-optional-locks", "-C", str(path), *args], text=True
    ).strip()


def validate(path, pin):
    guidance = (
        f"Preserve local work. Repair managed caches with: {shlex.quote(str(ROOT / 'scripts/create-deps.sh'))} --repair. "
        "External SDK paths are validation-only; supply a clean pinned SDK."
    )
    try:
        if Path(git(path, "rev-parse", "--show-toplevel")).resolve() != path.resolve():
            raise ValueError("not a standalone checkout")
        actual = git(path, "rev-parse", "HEAD")
        if actual != pin["revision"]:
            raise ValueError(f"revision {actual}; expected {pin['revision']}")
        if git(
            path,
            "status",
            "--porcelain",
            "--untracked-files=all",
            "--ignore-submodules=none",
        ):
            raise ValueError("dirty source checkout")
        for submodule in pin.get("submodules", []):
            state = subprocess.check_output(
                [
                    "git",
                    "-C",
                    str(path),
                    "submodule",
                    "status",
                    "--recursive",
                    "--",
                    submodule,
                ],
                text=True,
            )
            if not state or any(line[0] != " " for line in state.splitlines()):
                raise ValueError(f"missing or wrong submodule {submodule}")
    except (subprocess.CalledProcessError, ValueError) as exc:
        raise ValueError(f"Invalid dependency {path}: {exc}. {guidance}") from exc


def managed_target(path):
    """Only direct, non-symlink cache children may be replaced."""
    path = Path(path).absolute()
    deps = ROOT / ".deps"
    if path.parent != deps or any(p.is_symlink() for p in (deps, path, path / ".git")):
        raise ValueError(f"Refusing repair of symlink/external target {path}")
    if (path / ".git").exists() and not (path / ".git").is_dir():
        raise ValueError(f"Refusing repair of external Git directory at {path}")
    backups = ROOT / ".deps-backups"
    if backups.is_symlink():
        raise ValueError(f"Refusing symlink backup directory {backups}")
    return backups


def clone_sources(path, pin, reference=None):
    # A local clone retains objects but discards all working-tree formatting.
    # --no-hardlinks makes the replacement independent of the retained backup.
    local = False
    if reference is not None:
        try:
            git(reference, "cat-file", "-e", pin["revision"] + "^{commit}")
            local = True
        except subprocess.CalledProcessError:
            pass
    if local:
        subprocess.run(
            [
                "git",
                "clone",
                "--quiet",
                "--no-hardlinks",
                "--dissociate",
                "--no-checkout",
                str(reference),
                str(path),
            ],
            check=True,
        )
        git(path, "remote", "set-url", "origin", pin["url"])
        git(path, "checkout", "--quiet", "--detach", pin["revision"])
    else:
        subprocess.run(
            [
                "git",
                "clone",
                "--quiet",
                "--depth",
                "1",
                "--branch",
                pin["version"],
                pin["url"],
                str(path),
            ],
            check=True,
        )
    if git(path, "rev-parse", "HEAD") != pin["revision"]:
        raise ValueError(f"Tag revision mismatch at {path}")
    for submodule in pin.get("submodules", []):
        git(path, "submodule", "init", "--", submodule)
        prefix = ["git", "-C", str(path)]
        # Git stores module objects/config by registered name, not checkout path.
        entries = git(
            path,
            "config",
            "--file",
            ".gitmodules",
            "--get-regexp",
            r"^submodule\..*\.path$",
        )
        names = [
            key[len("submodule.") : -len(".path")]
            for key, value in (line.split(None, 1) for line in entries.splitlines())
            if value == submodule
        ]
        if len(names) != 1:
            raise ValueError(f"Expected one registered submodule for {submodule}")
        name = names[0]
        subref = reference / ".git/modules" / name if reference else None
        if subref and subref.is_dir():
            # Scope file transport to this trusted local object store only.
            prefix += [
                "-c",
                "protocol.file.allow=always",
                "-c",
                f"submodule.{name}.url={subref}",
            ]
        subprocess.run(
            [
                *prefix,
                "submodule",
                "update",
                "--init",
                "--recursive",
                "--depth",
                "1",
                "--",
                submodule,
            ],
            check=True,
        )
        # Restore upstream URLs after a local clone, keeping no backup dependency.
        git(path, "submodule", "sync", "--recursive", "--", submodule)
    validate(path, pin)


def setup(path, pin, check_only=False, repair=False):
    path = Path(path).absolute()
    if check_only and repair:
        raise ValueError("--check and --repair are mutually exclusive")
    if repair:
        backups = managed_target(path)
    if path.exists() or path.is_symlink():
        try:
            validate(path, pin)
            print(f"Validated {path.name}: {pin['revision']}")
            return
        except ValueError:
            if not repair:
                raise
    elif check_only:
        raise ValueError(
            f"Missing dependency {path}; run {ROOT / 'scripts/create-deps.sh'} (bootstrap)."
        )
    reference = None
    if repair and path.exists():
        backups.mkdir(parents=True, exist_ok=True)
        reference = backups / (path.name + "-" + uuid.uuid4().hex)
        path.rename(reference)
        print(f"Preserved original dependency: {reference}", flush=True)
    path.parent.mkdir(parents=True, exist_ok=True)
    # Stage and validate before publishing; failures retain the original backup.
    with tempfile.TemporaryDirectory(prefix=".restore-", dir=path.parent) as tmp:
        staged = Path(tmp) / path.name
        clone_sources(staged, pin, reference)
        staged.rename(path)
    print(f"Validated {path.name}: {pin['revision']}")


def selected_sdk():
    return Path(os.environ.get("PICO_SDK_PATH") or ROOT / ".deps/pico-sdk").resolve()


def setup_sources(mode="all", check_only=False, repair=False, sdk_path=None):
    pins = json.loads((ROOT / "dependencies.json").read_text())
    names = (
        ["apio", "epio"]
        if mode == "host"
        else (
            ["apio", "epio", "pico-sdk", "picotool"]
            if mode == "all"
            else ["apio", "pico-sdk", "picotool"]
        )
    )
    override = sdk_path or os.environ.get("PICO_SDK_PATH")
    # Validate external SDK before any managed cache mutation.
    if override and "pico-sdk" in names:
        setup(Path(override).absolute(), pins["pico-sdk"], check_only=True)
    for name in names:
        if name == "pico-sdk" and override:
            continue
        setup(ROOT / ".deps" / name, pins[name], check_only, repair)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mode", choices=["host", "firmware", "stimulus", "link", "all"], default="all"
    )
    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "--check", action="store_true", help="validate only; never download"
    )
    group.add_argument(
        "--repair",
        action="store_true",
        help="back up invalid managed caches and restore pins",
    )
    parser.add_argument("--sdk-path", type=Path)
    args = parser.parse_args()
    setup_sources(args.mode, args.check, args.repair, args.sdk_path)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"bootstrap: {error}", file=sys.stderr)
        sys.exit(1)
