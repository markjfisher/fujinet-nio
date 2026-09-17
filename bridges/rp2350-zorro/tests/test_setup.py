"""Offline setup contract tests; fixtures never use network or hardware."""

import contextlib
import hashlib
import tarfile
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import bootstrap
import bridge_setup


def git(path, *args):
    return bootstrap.git(path, *args)


def repository(path):
    path.mkdir(parents=True)
    git(path, "init", "-q")
    for key, value in [
        ("user.name", "Fixture"),
        ("user.email", "fixture@example.invalid"),
        ("commit.gpgsign", "false"),
        ("tag.gpgsign", "false"),
    ]:
        git(path, "config", key, value)
    (path / "source.c").write_text("original\n")
    git(path, "add", ".")
    git(path, "commit", "-qm", "fixture")
    git(path, "tag", "fixture")
    return dict(
        url=str(path), version="fixture", revision=git(path, "rev-parse", "HEAD")
    )


class Setup(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.patch = patch.object(bootstrap, "ROOT", self.root)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.pin = repository(self.root / "origin")
        self.target = self.root / ".deps/apio"

    def test_repair_preserves_work_and_is_idempotent_without_origin(self):
        bootstrap.setup(self.target, self.pin)
        (self.target / "source.c").write_text("formatted\n")
        (self.target / "scratch.txt").write_text("precious\n")
        (self.root / "origin").rename(self.root / "origin-offline")
        with self.assertRaisesRegex(ValueError, "create-deps.sh.*--repair"):
            bootstrap.setup(self.target, self.pin)
        with self.assertRaises(ValueError):
            bootstrap.setup(self.target, self.pin, check_only=True)
        self.assertFalse((self.root / ".deps-backups").exists())
        bootstrap.setup(self.target, self.pin, repair=True)
        backups = list((self.root / ".deps-backups").iterdir())
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / "source.c").read_text(), "formatted\n")
        self.assertEqual((backups[0] / "scratch.txt").read_text(), "precious\n")
        bootstrap.validate(self.target, self.pin)
        inode = self.target.stat().st_ino
        bootstrap.setup(self.target, self.pin, repair=True)
        self.assertEqual(self.target.stat().st_ino, inode)
        self.assertEqual(list((self.root / ".deps-backups").iterdir()), backups)

    def test_failed_restore_and_wrong_pin_keep_original(self):
        bootstrap.setup(self.target, self.pin)
        (self.target / "source.c").write_text("keep me")
        wrong = dict(self.pin, revision="0" * 40)
        with self.assertRaisesRegex(ValueError, "Tag revision mismatch"):
            bootstrap.setup(self.target, wrong, repair=True)
        self.assertFalse(self.target.exists())
        backup = next((self.root / ".deps-backups").iterdir())
        self.assertEqual((backup / "source.c").read_text(), "keep me")
        bootstrap.setup(self.target, self.pin)
        (self.target / "untracked").write_text("also keep")
        with patch.object(bootstrap, "clone_sources", side_effect=OSError("disk full")):
            with self.assertRaisesRegex(OSError, "disk full"):
                bootstrap.setup(self.target, self.pin, repair=True)
        self.assertEqual(len(list((self.root / ".deps-backups").iterdir())), 2)
        self.assertFalse(self.target.exists())

    def test_external_and_symlink_targets_refused(self):
        with self.assertRaisesRegex(ValueError, "external"):
            bootstrap.setup(self.root / "origin", self.pin, repair=True)
        self.target.parent.mkdir()
        self.target.symlink_to(self.root / "origin", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            bootstrap.setup(self.target, self.pin, repair=True)
        self.target.unlink()
        self.target.parent.rmdir()
        self.target.parent.symlink_to(self.root / "origin", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            bootstrap.setup(self.target, self.pin, repair=True)
        self.assertEqual((self.root / "origin/source.c").read_text(), "original\n")

    def test_registered_submodule_repair_reuses_local_objects(self):
        sub = self.root / "sub-origin"
        repository(sub)
        origin = self.root / "origin"
        git(
            origin,
            "-c",
            "protocol.file.allow=always",
            "submodule",
            "add",
            "--name",
            "tinyusb",
            str(sub),
            "lib/tinyusb",
        )
        git(origin, "commit", "-qam", "submodule")
        git(origin, "tag", "-f", "fixture")
        pin = dict(
            self.pin,
            revision=git(origin, "rev-parse", "HEAD"),
            submodules=["lib/tinyusb"],
        )
        with patch.dict(os.environ, GIT_ALLOW_PROTOCOL="file"):
            bootstrap.setup(self.target, pin)
        (self.target / "lib/tinyusb/source.c").write_text("formatted submodule")
        (self.target / "lib/tinyusb/scratch").write_text("submodule work")
        origin.rename(self.root / "offline-origin")
        sub.rename(self.root / "offline-sub")
        bootstrap.setup(self.target, pin, repair=True)
        bootstrap.validate(self.target, pin)
        backup = next((self.root / ".deps-backups").iterdir())
        self.assertEqual((backup / "lib/tinyusb/scratch").read_text(), "submodule work")
        self.assertEqual(
            git(self.target / "lib/tinyusb", "remote", "get-url", "origin"), str(sub)
        )
        # Replacement has no dependency on backup paths or alternates.
        backup.rename(self.root / "moved-backup")
        bootstrap.validate(self.target, pin)
        git(self.target / "lib/tinyusb", "fsck", "--no-reflogs")

    def test_external_sdk_repair_is_validation_only(self):
        (self.root / "dependencies.json").write_text(
            json.dumps({n: self.pin for n in ["apio", "epio", "pico-sdk", "picotool"]})
        )
        origin = self.root / "origin"
        (origin / "source.c").write_text("external work")
        with patch.dict(os.environ, PICO_SDK_PATH=str(origin)):
            with self.assertRaisesRegex(ValueError, "dirty source"):
                bootstrap.setup_sources(repair=True)
        self.assertFalse((self.root / ".deps").exists())
        self.assertFalse((self.root / ".deps-backups").exists())
        self.assertEqual((origin / "source.c").read_text(), "external work")

    def test_cli_cwd_help_and_mutually_exclusive_options(self):
        fixture = self.root / "standalone"
        shutil.copytree(
            ROOT / "scripts",
            fixture / "scripts",
            ignore=shutil.ignore_patterns("__pycache__"),
        )
        before = {
            str(p.relative_to(fixture)): p.read_bytes()
            for p in fixture.rglob("*")
            if p.is_file()
        }
        for name in ["create-deps", "setup", "test", "build", "install-toolchain"]:
            result = subprocess.run(
                [str(fixture / "scripts" / (name + ".sh")), "--help"],
                cwd=self.root,
                text=True,
                capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
        after = {
            str(p.relative_to(fixture)): p.read_bytes()
            for p in fixture.rglob("*")
            if p.is_file()
        }
        self.assertEqual(before, after)
        self.assertFalse(list(fixture.rglob("__pycache__")))
        pins = {name: self.pin for name in ["apio", "epio"]}
        (fixture / "dependencies.json").write_text(json.dumps(pins))
        command = [str(fixture / "scripts/create-deps.sh"), "--mode", "host"]
        result = subprocess.run(command, cwd="/tmp", capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run(
            [*command, "--check", "--repair"],
            cwd="/tmp",
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not allowed", result.stderr)
        self.assertFalse((fixture / ".deps-backups").exists())

    def test_generator_help_and_dry_run_do_not_write_bytecode(self):
        fixture = self.root / "standalone"
        shutil.copytree(
            ROOT / "scripts",
            fixture / "scripts",
            ignore=shutil.ignore_patterns("__pycache__"),
        )
        experiment = fixture / "tests/feasibility"
        (experiment / "generator-check").mkdir(parents=True)
        for name in (
            "experiment.py",
            "generator-check/run.sh",
            "generator-check/experiment.json",
        ):
            shutil.copy2(ROOT / "tests/feasibility" / name, experiment / name)
        before = {
            str(p.relative_to(fixture)): p.read_bytes()
            for p in fixture.rglob("*")
            if p.is_file()
        }
        for args in (["--help"], ["build", "--dry-run"]):
            result = subprocess.run(
                [str(experiment / "generator-check/run.sh"), *args],
                cwd="/tmp",
                text=True,
                capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
        after = {
            str(p.relative_to(fixture)): p.read_bytes()
            for p in fixture.rglob("*")
            if p.is_file()
        }
        self.assertEqual(before, after)
        self.assertFalse(list(fixture.rglob("__pycache__")))

    def test_shared_clone_repair_dissociates_borrowed_objects(self):
        self.target.parent.mkdir()
        subprocess.run(
            ["git", "clone", "--shared", str(self.root / "origin"), str(self.target)],
            check=True,
            capture_output=True,
        )
        self.assertTrue((self.target / ".git/objects/info/alternates").exists())
        (self.target / "source.c").write_text("dirty")
        bootstrap.setup(self.target, self.pin, repair=True)
        (self.root / "origin").rename(self.root / "offline-origin")
        self.assertFalse((self.target / ".git/objects/info/alternates").exists())
        bootstrap.validate(self.target, self.pin)
        git(self.target, "fsck", "--no-reflogs")

    def test_setup_host_repair_precedes_unrelated_tool_requirement(self):
        fixture = self.root / "standalone"
        shutil.copytree(
            ROOT / "scripts",
            fixture / "scripts",
            ignore=shutil.ignore_patterns("__pycache__"),
        )
        (fixture / "dependencies.json").write_text(
            json.dumps({name: self.pin for name in ("apio", "epio")})
        )
        create = [str(fixture / "scripts/create-deps.sh"), "--mode", "host"]
        subprocess.run(create, cwd="/tmp", check=True, capture_output=True)
        (fixture / ".deps/apio/source.c").write_text("local work")
        command = [str(fixture / "scripts/setup.sh"), "--host-only", "--repair"]
        result = subprocess.run(
            command,
            cwd="/tmp",
            capture_output=True,
            text=True,
            env=dict(os.environ, CC="nonexistent-fixture-compiler"),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing prerequisite tools", result.stderr)
        backup = next((fixture / ".deps-backups").iterdir())
        self.assertEqual((backup / "source.c").read_text(), "local work")
        bootstrap.validate(fixture / ".deps/apio", self.pin)
        result = subprocess.run(
            command,
            cwd="/tmp",
            capture_output=True,
            text=True,
            env=dict(
                os.environ, CC="cc -O2", PICO_TOOLCHAIN_PATH="/invalid-unused-arm"
            ),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(list((fixture / ".deps-backups").iterdir())), 1)

    def test_requirements_accept_compiler_arguments_and_enforce_cmake_version(self):
        with (
            patch.dict(os.environ, CC="cc -O2", CXX="c++ -O2"),
            patch.object(
                bridge_setup.shutil,
                "which",
                side_effect=lambda tool: "/bin/" + tool if " " not in tool else None,
            ),
            patch.object(
                bridge_setup.subprocess,
                "check_output",
                return_value="cmake version 3.21.0",
            ),
        ):
            bridge_setup.requirements(host_only=False, usb=False)
            with patch.object(
                bridge_setup.subprocess,
                "check_output",
                return_value="cmake version 3.20.9",
            ):
                with self.assertRaisesRegex(ValueError, "CMake >=3.21"):
                    bridge_setup.requirements(host_only=True)

    def test_incomplete_explicit_toolchain_and_stale_compiler_cache_rejected(self):
        compiler = self.root / "bin/arm-none-eabi-gcc"
        compiler.parent.mkdir()
        compiler.write_text("#!/bin/sh\nexit 0\n")
        compiler.chmod(0o755)
        with patch.dict(os.environ, PICO_TOOLCHAIN_PATH=str(compiler.parent)):
            with self.assertRaisesRegex(ValueError, "Invalid explicit"):
                bridge_setup.configure_toolchain(install=True)
            build = self.root / "build"
            build.mkdir()
            cache = build / "CMakeCache.txt"
            cache.write_text("CMAKE_C_COMPILER:FILEPATH=/old/bin/arm-none-eabi-gcc\n")
            with self.assertRaisesRegex(ValueError, "Move .* aside"):
                bridge_setup.validate_compiler_cache(build)
            cache.write_text("CMAKE_C_COMPILER:FILEPATH=" + str(compiler) + "\n")
            bridge_setup.validate_compiler_cache(build)

    def test_install_requires_safe_extraction_without_downloading(self):
        with patch.object(bridge_setup.tarfile, "data_filter", create=True):
            del bridge_setup.tarfile.data_filter
            with patch.object(
                bridge_setup.urllib.request,
                "urlretrieve",
                side_effect=AssertionError("downloaded"),
            ):
                with self.assertRaisesRegex(ValueError, "tarfile.data_filter"):
                    bridge_setup.install_toolchain(self.root / "toolchains")
        self.assertFalse((self.root / "toolchains").exists())

    @unittest.skipUnless(
        hasattr(tarfile, "data_filter"), "safe tar extraction unavailable"
    )
    def test_toolchain_install_verifies_archive_and_rerun_reuses_compiler(self):
        archive = self.root / "fixture.tar.xz"
        compiler = b"#!/bin/sh\nexit 0\n"
        with tarfile.open(archive, "w:xz") as output:
            for name in ("gcc", "g++", "ar", "objcopy"):
                member = tarfile.TarInfo(
                    bridge_setup.TOOLCHAIN + "/bin/arm-none-eabi-" + name
                )
                member.size, member.mode = len(compiler), 0o755
                output.addfile(member, io.BytesIO(compiler))

        def download(url, destination):
            shutil.copyfile(archive, destination)

        with (
            patch.object(bridge_setup.platform, "system", return_value="Linux"),
            patch.object(bridge_setup.platform, "machine", return_value="x86_64"),
            patch.object(
                bridge_setup,
                "TOOLCHAIN_SHA256",
                hashlib.sha256(archive.read_bytes()).hexdigest(),
            ),
            patch.object(
                bridge_setup.urllib.request, "urlretrieve", side_effect=download
            ) as fetch,
        ):
            with (
                patch.dict(os.environ, PICO_TOOLCHAIN_PATH="", PATH=""),
                patch.object(bridge_setup, "workspace", return_value=None),
                patch.object(bridge_setup, "ROOT", self.root),
                patch.object(Path, "home", return_value=self.root / "home"),
            ):
                installed = bridge_setup.configure_toolchain(install=True)
                self.assertEqual(
                    installed,
                    self.root / "build/toolchains" / bridge_setup.TOOLCHAIN / "bin",
                )
                self.assertEqual(
                    bridge_setup.configure_toolchain(install=True), installed
                )
            self.assertEqual(fetch.call_count, 1)

    @unittest.skipUnless(
        hasattr(tarfile, "data_filter"), "safe tar extraction unavailable"
    )
    def test_toolchain_rejects_archive_link_escape(self):
        archive = self.root / "unsafe.tar.xz"
        with tarfile.open(archive, "w:xz") as output:
            member = tarfile.TarInfo(bridge_setup.TOOLCHAIN + "/escape")
            member.type, member.linkname = tarfile.SYMTYPE, "/tmp"
            output.addfile(member)

        def download(url, destination):
            shutil.copyfile(archive, destination)

        with (
            patch.object(bridge_setup.platform, "system", return_value="Linux"),
            patch.object(bridge_setup.platform, "machine", return_value="x86_64"),
            patch.object(
                bridge_setup,
                "TOOLCHAIN_SHA256",
                hashlib.sha256(archive.read_bytes()).hexdigest(),
            ),
            patch.object(
                bridge_setup.urllib.request, "urlretrieve", side_effect=download
            ),
        ):
            with self.assertRaises(tarfile.FilterError):
                bridge_setup.install_toolchain(self.root / "toolchains")
        self.assertEqual(list((self.root / "toolchains").iterdir()), [])

    @unittest.skipUnless(
        hasattr(tarfile, "data_filter"), "safe tar extraction unavailable"
    )
    def test_toolchain_checksum_failure_and_explicit_override(self):
        def download(url, destination):
            Path(destination).write_bytes(b"not the pinned archive")

        with (
            patch.object(bridge_setup.platform, "system", return_value="Linux"),
            patch.object(bridge_setup.platform, "machine", return_value="x86_64"),
            patch.object(
                bridge_setup.urllib.request, "urlretrieve", side_effect=download
            ),
            patch.object(
                bridge_setup.tarfile,
                "open",
                side_effect=AssertionError("extracted before verification"),
            ),
        ):
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                bridge_setup.install_toolchain(self.root / "toolchains")
        self.assertEqual(list((self.root / "toolchains").iterdir()), [])
        with (
            patch.dict(os.environ, PICO_TOOLCHAIN_PATH=str(self.root / "bad")),
            patch.object(
                bridge_setup,
                "install_toolchain",
                side_effect=AssertionError("replaced override"),
            ),
        ):
            with self.assertRaisesRegex(ValueError, "Invalid explicit"):
                bridge_setup.configure_toolchain(install=True)


if __name__ == "__main__":
    unittest.main()
