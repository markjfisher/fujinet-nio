"""Offline tests use tiny local git origins, never the reference project."""

import importlib.util
import json
import os
import shutil
import sys
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


bootstrap = module("bootstrap")
policy = module("check_pio_policy")


class Tooling(unittest.TestCase):
    def test_bootstrap_idempotence_stale_and_dirty(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            origin = root / "origin"
            origin.mkdir()

            def git(*args):
                return subprocess.check_output(
                    ["git", "-C", str(origin), *args], text=True
                ).strip()

            git("init", "-q")
            git("config", "commit.gpgsign", "false")
            git("config", "tag.gpgsign", "false")
            git("config", "user.email", "fixture@example.invalid")
            git("config", "user.name", "Fixture")
            (origin / "source.c").write_text("first\n")
            git("add", ".")
            git("commit", "-qm", "fixture")
            git("tag", "fixture-v1")
            pin = {
                "url": str(origin),
                "version": "fixture-v1",
                "revision": git("rev-parse", "HEAD"),
            }
            checkout = root / "checkout"
            bootstrap.setup(checkout, pin)
            first_head = bootstrap.git(checkout, "rev-parse", "HEAD")
            bootstrap.setup(checkout, pin)
            self.assertEqual(first_head, bootstrap.git(checkout, "rev-parse", "HEAD"))
            with self.assertRaisesRegex(ValueError, "revision.*expected"):
                bootstrap.setup(checkout, dict(pin, revision="0" * 40))
            (checkout / "source.c").write_text("dirty\n")
            with self.assertRaisesRegex(
                ValueError, "dirty source.*Preserve local work"
            ):
                bootstrap.setup(checkout, pin)
            self.assertEqual((checkout / "source.c").read_text(), "dirty\n")
            bootstrap.git(checkout, "restore", "source.c")
            (checkout / "untracked.c").write_text("local work")
            with self.assertRaisesRegex(ValueError, "dirty source"):
                bootstrap.setup(checkout, pin)
            (checkout / "untracked.c").unlink()
            with self.assertRaisesRegex(ValueError, "Invalid dependency"):
                bootstrap.setup(checkout, dict(pin, submodules=["missing-submodule"]))
            with self.assertRaisesRegex(ValueError, "Missing dependency.*bootstrap"):
                bootstrap.setup(root / "missing", pin, True)
            with self.assertRaisesRegex(ValueError, "Invalid dependency"):
                bootstrap.setup(root, pin, True)

    def test_invalid_configurations(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name, args, message in [
                (
                    "mode",
                    ["-DBRIDGE_MODE=invalid"],
                    "BRIDGE_MODE must be host or firmware",
                ),
                (
                    "board",
                    [
                        "-DBRIDGE_MODE=firmware",
                        "-DPICO_BOARD=pico2",
                        "-DPICO_PLATFORM=rp2350-arm-s",
                    ],
                    "Firmware requires",
                ),
                (
                    "platform",
                    [
                        "-DBRIDGE_MODE=firmware",
                        "-DPICO_BOARD=waveshare_core2350b",
                        "-DPICO_PLATFORM=rp2040",
                    ],
                    "Firmware requires",
                ),
                (
                    "stimulus-board",
                    [
                        "-DBRIDGE_MODE=stimulus",
                        "-DPICO_BOARD=pico2",
                        "-DPICO_PLATFORM=rp2040",
                    ],
                    "Stimulus requires",
                ),
                (
                    "stimulus-platform",
                    [
                        "-DBRIDGE_MODE=stimulus",
                        "-DPICO_BOARD=pico",
                        "-DPICO_PLATFORM=rp2350-arm-s",
                    ],
                    "Stimulus requires",
                ),
                (
                    "sdk",
                    [
                        "-DBRIDGE_MODE=firmware",
                        "-DPICO_BOARD=waveshare_core2350b",
                        "-DPICO_PLATFORM=rp2350-arm-s",
                        f"-DPICO_SDK_PATH={tmp}/missing-sdk",
                    ],
                    "Missing dependency",
                ),
            ]:
                result = subprocess.run(
                    ["cmake", "-S", str(ROOT), "-B", str(Path(tmp) / name), *args],
                    capture_output=True,
                    text=True,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stdout + result.stderr)

    def test_registered_submodules_and_cli_overrides(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            def git(path, *args):
                return subprocess.check_output(
                    ["git", "-C", str(path), *args], text=True
                ).strip()

            def repository(path):
                path.mkdir(parents=True)
                git(path, "init", "-q")
                for key, value in [
                    ("user.email", "fixture@example.invalid"),
                    ("user.name", "Fixture"),
                    ("commit.gpgsign", "false"),
                    ("tag.gpgsign", "false"),
                ]:
                    git(path, "config", key, value)
                (path / "source.c").write_text("initial")
                git(path, "add", ".")
                git(path, "commit", "-qm", "initial")

            sub = root / "sub-origin"
            repository(sub)
            sub_initial = git(sub, "rev-parse", "HEAD")
            sdk = root / "sdk-origin"
            repository(sdk)
            git(
                sdk,
                "-c",
                "protocol.file.allow=always",
                "submodule",
                "add",
                str(sub),
                "lib/fixture",
            )
            git(sdk, "commit", "-qam", "register submodule")
            pin = {
                "revision": git(sdk, "rev-parse", "HEAD"),
                "submodules": ["lib/fixture"],
            }
            clone = root / "sdk-checkout"
            subprocess.run(["git", "clone", "-q", str(sdk), str(clone)], check=True)
            with self.assertRaisesRegex(ValueError, "missing or wrong submodule"):
                bootstrap.validate(clone, pin)
            git(
                clone,
                "-c",
                "protocol.file.allow=always",
                "submodule",
                "update",
                "--init",
            )
            bootstrap.validate(clone, pin)
            (sub / "source.c").write_text("next")
            git(sub, "commit", "-qam", "next")
            local_sub = clone / "lib/fixture"
            git(local_sub, "fetch", "-q", "origin")
            git(local_sub, "checkout", "-q", git(sub, "rev-parse", "HEAD"))
            with self.assertRaisesRegex(ValueError, "Invalid dependency"):
                bootstrap.validate(clone, pin)
            git(local_sub, "checkout", "-q", sub_initial)
            (local_sub / "source.c").write_text("dirty submodule")
            with self.assertRaisesRegex(ValueError, "dirty source"):
                bootstrap.validate(clone, pin)
            git(local_sub, "restore", "source.c")
            bootstrap.validate(clone, pin)

            # Exercise real CLI path selection against only local fixture sources.
            fixture = root / "bridge"
            (fixture / "scripts").mkdir(parents=True)
            shutil.copyfile(
                ROOT / "scripts/bootstrap.py", fixture / "scripts/bootstrap.py"
            )
            pins = {}
            for name in ["apio", "epio", "picotool"]:
                checkout = fixture / ".deps" / name
                repository(checkout)
                pins[name] = {"revision": git(checkout, "rev-parse", "HEAD")}
            pins["pico-sdk"] = pin
            manifest = fixture / "dependencies.json"
            manifest.write_text(json.dumps(pins))

            def cli(mode, sdk_path):
                return subprocess.run(
                    [
                        sys.executable,
                        str(fixture / "scripts/bootstrap.py"),
                        "--mode",
                        mode,
                        "--check",
                    ],
                    capture_output=True,
                    text=True,
                    env=dict(os.environ, PICO_SDK_PATH=str(sdk_path)),
                )

            self.assertEqual(cli("host", root / "missing-sdk").returncode, 0)
            self.assertIn(
                "Missing dependency", cli("firmware", root / "missing-sdk").stderr
            )
            self.assertEqual(cli("firmware", clone).returncode, 0)
            self.assertEqual(cli("stimulus", clone).returncode, 0)
            self.assertIn(
                "Missing dependency", cli("stimulus", root / "missing-sdk").stderr
            )
            pins["pico-sdk"] = dict(pin, revision="0" * 40)
            manifest.write_text(json.dumps(pins))
            result = cli("firmware", clone)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("expected", result.stderr)

    def test_guard_rejects_sources_and_build_rules(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, content in [
                ("capture.pio", ".program capture"),
                ("CMakeLists.txt", "pico_generate_pio_header(target capture.pio)"),
                ("rules.cmake", "add_custom_command(COMMAND pioasm x y)"),
                ("Makefile", "generate:\n\tpioasm input output"),
                ("build.py", "run('pioasm')"),
                ("src/build/capture.pio", ".program nested"),
            ]:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
                self.assertTrue(policy.violations(root), name)
                result = subprocess.run(
                    ["python3", str(ROOT / "scripts/check_pio_policy.py"), str(root)],
                    capture_output=True,
                )
                self.assertNotEqual(result.returncode, 0)
                path.unlink()
            (root / ".deps-backups").mkdir()
            (root / ".deps-backups" / "vendor.pio").write_text("vendor")
            (root / ".deps").mkdir()
            (root / ".deps" / "vendor.pio").write_text("vendor")
            self.assertEqual(policy.violations(root), [])


if __name__ == "__main__":
    unittest.main()
