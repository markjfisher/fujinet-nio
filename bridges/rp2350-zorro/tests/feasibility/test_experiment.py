#!/usr/bin/env python3
"""Offline acceptance, session, transport and acquisition rejection checks."""

import contextlib
import argparse
import io
import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile
import experiment as e

M = dict(
    id="generator-check",
    status="implemented",
    preset="stimulus-rp2040",
    target="feasibility_stimulus",
    expected_flash_id="754765170F445253",
    samplerate_hz=1000000,
    analyzer_channels=["D0", "D1", "D2", "D3", "D7"],
    expected_values=list(range(16)),
    pulse_us=100,
    period_us=300,
    setup_hold_us=100,
)
META = "[global]\nsigrok version=0.5.2\n[device 1]\ncapturefile=logic-1\ntotal probes=8\nsamplerate=1 MHz\nunitsize=1\nprobe1=D0\nprobe2=D1\nprobe3=D2\nprobe4=D3\nprobe8=D7\n"


def waveform():
    return (
        b"\x80" * 200
        + b"".join(
            bytes([128 + n]) * 100 + bytes([n]) * 100 + bytes([128 + n]) * 100
            for n in range(16)
        )
        + b"\x8f" * 200
    )


class Experiments(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.capture = self.directory / "capture.sr"

    def write_capture(self, data=None, metadata=META, names=None):
        with zipfile.ZipFile(self.capture, "w") as z:
            z.writestr("metadata", metadata)
            for name, chunk in names or [
                ("logic-1-1", waveform() if data is None else data)
            ]:
                z.writestr(name, chunk)

    def test_finite_valid_waveform(self):
        self.write_capture()
        self.assertEqual(e.analyse(self.capture, M)["values"], list(range(16)))

    def test_recorded_final_captures(self):
        for name in ("w0-final-001.sr", "w0-final-002.sr"):
            path = e.ROOT / "docs/feasibility/results/2026-09-17-generator" / name
            self.assertEqual(e.analyse(path, M)["assertions"], 16)

    def test_corrupt_truncated_wrong_metadata(self):
        for meta in (
            META.replace("1 MHz", "unknown"),
            META.replace("1 MHz", "2 MHz"),
            META.replace("probe8=D7", "probe8=D6"),
            META.replace("unitsize=1", "unitsize=2"),
        ):
            with self.subTest(metadata=meta):
                self.write_capture(metadata=meta)
                with self.assertRaises(e.Failure):
                    e.analyse(self.capture, M)
        self.capture.write_bytes(b"corrupt")
        with self.assertRaises(e.Failure):
            e.analyse(self.capture, M)
        for data in (b"", waveform()[:1000], waveform()[:-350], b"\x80" * 5000):
            self.write_capture(data)
            with self.assertRaises(e.Failure):
                e.analyse(self.capture, M)

    def test_wrong_pulse_data_period_and_extra_edges(self):
        for mutation in ("data", "pulse", "period", "extra"):
            data = bytearray(waveform())
            if mutation == "data":
                data[350] = 1
            if mutation == "pulse":
                data[350:370] = b"\x80" * 20
            if mutation == "period":
                data[510:510] = b"\x81" * 10
            if mutation == "extra":
                data.extend(b"\x00" * 100 + b"\x80" * 100)
            self.write_capture(data)
            with self.subTest(mutation=mutation), self.assertRaises(e.Failure):
                e.analyse(self.capture, M)

    def test_chunk_sequence(self):
        data = waveform()
        self.write_capture(names=[("logic-1-1", data[:100]), ("logic-1-2", data[100:])])
        self.assertEqual(e.analyse(self.capture, M)["assertions"], 16)
        self.write_capture(names=[("logic-1-2", data)])
        with self.assertRaises(e.Failure):
            e.analyse(self.capture, M)

    def test_ram_only_elf(self):
        artifact = self.directory / "firmware.elf"
        data = bytearray(88)
        data[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into(
            "<HHIIIIIHHHHHH",
            data,
            16,
            2,
            40,
            1,
            0x20000001,
            52,
            0,
            0,
            52,
            32,
            1,
            0,
            0,
            0,
        )
        struct.pack_into(
            "<IIIIIIII", data, 52, 1, 84, 0x20000000, 0x20000000, 4, 4, 5, 4
        )
        artifact.write_bytes(data)
        e.validate_ram_elf(artifact)
        struct.pack_into("<I", data, 64, 0x10000000)
        artifact.write_bytes(data)
        with self.assertRaises(e.Failure):
            e.validate_ram_elf(artifact)
        artifact.write_bytes(b"bad ELF")
        with self.assertRaises(e.Failure):
            e.validate_ram_elf(artifact)

    def test_identity_not_placeholder(self):
        e.validate_info(
            "Device Information\n type: RP2040\n flash id: 754765170F445253\n",
            M["expected_flash_id"],
        )
        for text in (
            "type: RP2350\nflash id: 754765170F445253",
            "type: RP2040\nflash id: EEEEEEEEEEEEEEEE",
            "type: RP2040",
        ):
            with self.assertRaises(e.Failure):
                e.validate_info(text, M["expected_flash_id"])

    def test_stale_session(self):
        artifact = self.directory / "firmware.elf"
        artifact.write_bytes(b"current image")
        usb = dict(path="1-2", vid="2e8a", pid="000a", bus=1, address=42, inode=123)
        session = dict(
            experiment=M["id"],
            flash_id=M["expected_flash_id"],
            firmware_sha256=e.digest(artifact),
            usb=usb,
            boot_id=e.boot_id(),
        )
        session["build_identity"] = dict(
            firmware_sha256=e.digest(artifact), revision="built-revision"
        )
        e.save(artifact.with_suffix(".build.json"), session["build_identity"])
        self.assertEqual(e.validate_session(M, session, artifact, [usb]), usb)
        for change in (
            {"firmware_sha256": "old"},
            {"boot_id": "old"},
            {"flash_id": "EEEEEEEEEEEEEEEE"},
        ):
            with self.assertRaises(e.Failure):
                e.validate_session(M, dict(session, **change), artifact, [usb])
        with self.assertRaises(e.Failure):
            e.validate_session(M, session, artifact, [dict(usb, address=43)])
        with self.assertRaises(e.Failure):
            e.validate_session(M, session, artifact, [dict(usb, inode=124)])

    def test_fresh_ack_required(self):
        console = Mock()
        console.line.side_effect = ["complete generated=16 nominal_hz=100000"]
        with self.assertRaises(e.Failure):
            e.fresh_completion(console)
        console.line.side_effect = [
            "running samples=16 nominal_hz=100000",
            "complete generated=16 nominal_hz=100000",
        ]
        e.fresh_completion(console)
        console.line.side_effect = ["aborted disconnect generated=unknown"]
        with self.assertRaises(e.Failure):
            e.fresh_completion(console)

    def test_analyzer_failure_before_run(self):
        log = self.directory / "acquisition.log"
        child = Mock()
        child.poll.return_value = None
        log.write_text("fx2lafw: Failed to claim interface: busy")
        with self.assertRaises(e.Failure):
            e.await_acquisition(child, log)
        log.write_text(
            "fx2lafw: receive_transfer(): status LIBUSB_SUCCESS / LIBUSB_TRANSFER_COMPLETED received 512 bytes"
        )
        e.await_acquisition(child, log)
        child.poll.return_value = 1
        with self.assertRaises(e.Failure):
            e.await_acquisition(child, log)
        child.poll.return_value = None
        log.write_text("hwdriver: fx2lafw: Starting acquisition.")
        with self.assertRaises(e.Failure):
            e.await_acquisition(child, log, timeout=0.01)

    def test_dry_run_no_operations_and_planned_fails(self):
        manifest = self.directory / "experiment.json"
        manifest.write_text(json.dumps(M))
        with (
            patch.object(e, "command", side_effect=AssertionError("command")),
            patch.object(e, "usb_devices", side_effect=AssertionError("USB")),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            for stage in ("doctor", "build", "load", "run", "analyse", "all"):
                self.assertEqual(
                    e.main(["--manifest", str(manifest), stage, "--dry-run"]), 0
                )
        manifest.write_text(json.dumps(dict(M, status="planned")))
        with (
            patch.object(e, "command", side_effect=AssertionError("command")),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(e.main(["--manifest", str(manifest), "all"]), 1)
        self.assertEqual(
            sorted(p.name for p in self.directory.iterdir()), ["experiment.json"]
        )

    def test_existing_output_retained(self):
        manifest = self.directory / "experiment.json"
        manifest.write_text(json.dumps(M))
        marker = self.directory / "keep"
        marker.write_text("untouched")
        with (
            patch.object(e, "doctor", side_effect=AssertionError("side effect")),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(
                e.main(
                    [
                        "--manifest",
                        str(manifest),
                        "all",
                        "--output",
                        str(self.directory),
                    ]
                ),
                1,
            )
        self.assertEqual(marker.read_text(), "untouched")

    def fixture(self):
        artifact = self.directory / "firmware.elf"
        data = bytearray(88)
        data[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into(
            "<HHIIIIIHHHHHH",
            data,
            16,
            2,
            40,
            1,
            0x20000001,
            52,
            0,
            0,
            52,
            32,
            1,
            0,
            0,
            0,
        )
        struct.pack_into(
            "<IIIIIIII", data, 52, 1, 84, 0x20000000, 0x20000000, 4, 4, 5, 4
        )
        artifact.write_bytes(data)
        identity = dict(firmware_sha256=e.digest(artifact), revision="built-revision")
        e.save(artifact.with_suffix(".build.json"), identity)
        usb = dict(path="1-2", vid="2e8a", pid="000a", bus=1, address=42, inode=123)
        session = dict(
            experiment=M["id"],
            flash_id=M["expected_flash_id"],
            firmware_sha256=e.digest(artifact),
            usb=usb,
            boot_id=e.boot_id(),
            build_identity=identity,
        )
        args = argparse.Namespace(
            output=self.directory / "result",
            session=self.directory / "session.json",
            usb_path=None,
            timeout=0.001,
            analyzer="fx2lafw",
        )
        e.save(args.session, session)
        return artifact, usb, session, args

    def test_load_wrong_identity_never_loads(self):
        artifact, usb, session, args = self.fixture()
        bootsel = dict(usb, pid="0003")
        with (
            patch.object(e, "usb_devices", return_value=[bootsel]),
            patch.object(e, "access"),
            patch.object(
                e, "command", return_value="type: RP2040\nflash id: EEEEEEEEEEEEEEEE"
            ) as cmd,
        ):
            with self.assertRaises(e.Failure):
                e.load(M, args, artifact)
        self.assertFalse(any(call.args[0][1] == "load" for call in cmd.call_args_list))

    def test_load_tracks_port_and_exact_snapshot(self):
        artifact, usb, session, args = self.fixture()
        bootsel = dict(usb, pid="0003", address=41, inode=122)
        info = "type: RP2040\nflash id: 754765170F445253"
        original = artifact.read_bytes()
        args.session = Path(e.os.path.relpath(args.session))

        def cmd(argv, *unused):
            if argv[1] == "load":
                self.assertTrue(Path(argv[4]).is_absolute())
                self.assertEqual(Path(argv[4]).read_bytes(), original)
                artifact.write_bytes(b"concurrent rebuild")
            return info

        with (
            patch.object(e, "usb_devices", side_effect=[[bootsel], [bootsel], [usb]]),
            patch.object(e, "access"),
            patch.object(e, "serial_port", return_value=Path("/dev/fake")),
            patch.object(e, "command", side_effect=cmd),
        ):
            e.load(M, args, artifact)
        saved = json.loads(args.session.read_text())
        self.assertEqual(saved["firmware_sha256"], session["firmware_sha256"])
        self.assertEqual(saved["usb"]["path"], "1-2")
        self.assertEqual(Path(saved["artifact_snapshot"]).read_bytes(), original)

    def test_load_wrong_reenumeration_port_fails(self):
        artifact, usb, session, args = self.fixture()
        args.session.unlink()
        bootsel = dict(usb, pid="0003")
        calls = iter([[bootsel], [bootsel]])

        def devices():
            return next(calls, [dict(usb, path="1-3")])

        with (
            patch.object(e, "usb_devices", side_effect=devices),
            patch.object(e, "access"),
            patch.object(
                e, "command", return_value="type: RP2040\nflash id: 754765170F445253"
            ),
            patch.object(e.time, "sleep"),
        ):
            with self.assertRaises(e.Failure):
                e.load(M, args, artifact)
        self.assertFalse(args.session.exists())

    @contextlib.contextmanager
    def fake_run(self, usb, prompt=None, acquisition=None):
        child = Mock()
        child.poll.return_value = None
        child.wait.return_value = 0
        child.returncode = 0
        console = Mock()
        console.line.side_effect = [
            "running samples=16 nominal_hz=100000",
            "complete generated=16 nominal_hz=100000",
        ]
        with (
            patch.object(e, "usb_devices", return_value=[usb]),
            patch.object(e, "serial_port", return_value=Path("/dev/fake")),
            patch.object(e, "command", return_value="sigrok fake"),
            patch.object(e.sys.stdin, "isatty", return_value=True),
            patch("builtins.input", side_effect=prompt),
            patch.object(e, "Console", return_value=console) as constructor,
            patch.object(e.subprocess, "Popen", return_value=child),
            patch.object(e, "await_acquisition", side_effect=acquisition),
        ):
            yield console, child, constructor

    def test_run_acquisition_failure_prevents_run_retains_failure(self):
        artifact, usb, session, args = self.fixture()
        with self.fake_run(usb, acquisition=e.Failure("acquisition", "busy")) as (
            console,
            child,
            ctor,
        ):
            with self.assertRaises(e.Failure):
                e.physical_run(M, args, artifact)
            console.send.assert_not_called()
            console.close.assert_called_once()
            child.terminate.assert_called_once()
        report = json.loads((args.output / "report.json").read_text())
        self.assertEqual(report["category"], "acquisition")
        self.assertIn("argv", report["acquisition"])

    def test_run_cancel_cleans_up(self):
        artifact, usb, session, args = self.fixture()
        with self.fake_run(usb, acquisition=KeyboardInterrupt()) as (
            console,
            child,
            ctor,
        ):
            with self.assertRaises(KeyboardInterrupt):
                e.physical_run(M, args, artifact)
            console.close.assert_called_once()
            child.terminate.assert_called_once()
        self.assertEqual(
            json.loads((args.output / "report.json").read_text())["category"],
            "cancelled",
        )

    def test_run_revalidates_after_prompt(self):
        artifact, usb, session, args = self.fixture()

        def prompt(*unused):
            usb["address"] = 99

        with self.fake_run(usb, prompt=prompt) as (console, child, ctor):
            with self.assertRaises(e.Failure):
                e.physical_run(M, args, artifact)
            ctor.assert_not_called()

    def test_run_honors_explicit_port(self):
        artifact, usb, session, args = self.fixture()
        args.usb_path = "1-3"
        with self.fake_run(usb) as (console, child, ctor):
            with self.assertRaises(e.Failure):
                e.physical_run(M, args, artifact)
            ctor.assert_not_called()

    def test_run_success_retains_build_identity(self):
        artifact, usb, session, args = self.fixture()
        self.write_capture()

        def ready(*unused):
            (args.output / "capture.sr").write_bytes(self.capture.read_bytes())

        with self.fake_run(usb, acquisition=ready) as (console, child, ctor):
            child.poll.side_effect = [None, 0]
            e.physical_run(M, args, artifact)
            console.send.assert_called_once_with("run")
            console.close.assert_called_once()
        report = json.loads((args.output / "report.json").read_text())
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["build_identity"]["revision"], "built-revision")
        self.assertEqual(report["acquisition"]["returncode"], 0)

    def test_run_late_acquisition_error(self):
        artifact, usb, session, args = self.fixture()

        def ready(*unused):
            (args.output / "acquisition.log").write_text(
                "fx2lafw: receive_transfer(): error overflow"
            )

        with self.fake_run(usb, acquisition=ready):
            with self.assertRaises(e.Failure):
                e.physical_run(M, args, artifact)
        self.assertEqual(
            json.loads((args.output / "report.json").read_text())["category"],
            "acquisition",
        )

    def test_console_consecutive_sessions_toggle_dtr(self):
        settings = [0, 0, 0, 0, 0, 0, [0] * 32]
        with (
            patch.object(e.os, "open", return_value=10),
            patch.object(e.os, "close") as close,
            patch.object(e.os, "write", side_effect=lambda fd, data: len(data)),
            patch.object(e.fcntl, "ioctl") as ioctl,
            patch.object(e.termios, "tcgetattr", return_value=settings),
            patch.object(e.termios, "tcsetattr") as setattrs,
            patch.object(e.termios, "tcflush"),
            patch.object(e.time, "sleep"),
        ):
            for _ in range(2):
                console = e.Console("/dev/fake", io.StringIO())
                console.close()
        toggles = [
            call.args[1]
            for call in ioctl.call_args_list
            if call.args[1] in (e.termios.TIOCMBIC, e.termios.TIOCMBIS)
        ]
        self.assertEqual(
            toggles, [e.termios.TIOCMBIC, e.termios.TIOCMBIS, e.termios.TIOCMBIC] * 2
        )
        self.assertTrue(setattrs.call_args.args[2][2] & e.termios.HUPCL)
        self.assertEqual(close.call_count, 2)

    def test_standalone_analysis_retains_failure_detail(self):
        self.write_capture(bytes([128]) * 5000)
        manifest = self.directory / "experiment.json"
        e.save(manifest, M)
        output = self.directory / "analysis"
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(
                e.main(
                    [
                        "--manifest",
                        str(manifest),
                        "analyse",
                        "--capture",
                        str(self.capture),
                        "--output",
                        str(output),
                    ]
                ),
                1,
            )
        report = json.loads((output / "report.json").read_text())
        self.assertEqual(report["details"]["observed_falls"], [])
        self.assertEqual(report["details"]["expected"], list(range(16)))

    def test_command_records_start_failure_and_partial_timeout(self):
        e.COMMAND_LOG.clear()
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(e.Failure):
                e.command(["/no/such/program"], timeout=0.01)
            self.assertIn("FileNotFoundError", e.COMMAND_LOG[-1]["termination"])
            with self.assertRaises(e.Failure):
                e.command(
                    [
                        sys.executable,
                        "-c",
                        'import time; print("partial", flush=True); time.sleep(2)',
                    ],
                    timeout=0.1,
                )
        self.assertIn("partial", e.COMMAND_LOG[-1]["output"])
        self.assertIn("TimeoutExpired", e.COMMAND_LOG[-1]["termination"])
        e.COMMAND_LOG.clear()

    def test_missing_sidecar_blocks_load_before_usb(self):
        artifact, usb, session, args = self.fixture()
        artifact.with_suffix(".build.json").unlink()
        with patch.object(e, "usb_devices", side_effect=AssertionError("USB accessed")):
            with self.assertRaises(e.Failure):
                e.load(M, args, artifact)

    def test_build_shared_setup_and_selected_sdk_overrides_cached_sdk(self):
        calls = []
        selected = self.directory / "external-sdk"
        with (
            patch.object(e, "static_prerequisites"),
            patch.object(e, "source_identity", return_value={}),
            patch.object(e.bridge_setup, "validate_compiler_cache"),
            patch.object(
                e,
                "command",
                side_effect=lambda args: calls.append(list(map(str, args))),
            ),
            patch.object(e, "digest", return_value="hash"),
            patch.object(e, "save"),
            patch.dict(
                e.os.environ,
                PICO_SDK_PATH=str(selected),
                PICO_TOOLCHAIN_PATH="/compiler/bin",
            ),
        ):
            e.build(M)
        setups = [call for call in calls if "create-deps.sh" in call[0]]
        self.assertEqual(
            setups, [[str(e.ROOT / "scripts/create-deps.sh"), "--mode", "all"]]
        )
        configure = next(
            call
            for call in calls
            if call[:3] == ["cmake", "--preset", "stimulus-rp2040"]
        )
        self.assertIn("-DPICO_SDK_PATH=" + str(selected), configure)
        loader = next(call for call in calls if call[:2] == ["cmake", "-S"])
        self.assertIn("-DPICO_SDK_PATH=" + str(selected), loader)
        for preset in ["host", "host-release"]:
            self.assertIn(["ctest", "--preset", preset], calls)

    def test_build_relative_sdk_from_tmp_is_normalized_before_setup_and_identity(self):
        selected = self.directory / "external-sdk"
        observed = []
        old_cwd = e.os.getcwd()
        try:
            e.os.chdir("/tmp")
            relative = e.os.path.relpath(selected, "/tmp")
            with (
                patch.object(e, "static_prerequisites"),
                patch.object(e.bridge_setup, "validate_compiler_cache"),
                patch.object(
                    e,
                    "source_identity",
                    side_effect=lambda m: observed.append(e.os.environ["PICO_SDK_PATH"])
                    or {},
                ),
                patch.object(
                    e,
                    "command",
                    side_effect=lambda args: observed.append(
                        e.os.environ["PICO_SDK_PATH"]
                    ),
                ),
                patch.object(e, "digest", return_value="hash"),
                patch.object(e, "save"),
                patch.dict(
                    e.os.environ,
                    PICO_SDK_PATH=relative,
                    PICO_TOOLCHAIN_PATH="/compiler/bin",
                ),
            ):
                e.build(M)
        finally:
            e.os.chdir(old_cwd)
        self.assertTrue(observed)
        self.assertEqual(set(observed), {str(selected.resolve())})

    def test_command_failure_keeps_actionable_repair_diagnostic(self):
        message = "Repair managed caches with: /bridge/scripts/create-deps.sh --repair"
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(e.Failure, "create-deps.sh --repair"):
                e.command(
                    [
                        sys.executable,
                        "-c",
                        "import sys; print(" + repr(message) + "); sys.exit(1)",
                    ]
                )
        self.assertIn(message, e.COMMAND_LOG[-1]["output"])
        e.COMMAND_LOG.clear()

    def test_explicit_toolchain_bin_selected_without_path(self):
        toolchain = self.directory / "toolchain"
        (toolchain / "bin").mkdir(parents=True)
        compiler = toolchain / "bin/arm-none-eabi-gcc"
        for name in ("gcc", "g++", "ar", "objcopy"):
            tool = toolchain / "bin" / ("arm-none-eabi-" + name)
            tool.write_text("#!/bin/sh\n")
            tool.chmod(0o700)
        with patch.dict(
            e.os.environ, {"PICO_TOOLCHAIN_PATH": str(toolchain), "PATH": ""}
        ):
            e.configure_toolchain()
            self.assertEqual(e.shutil.which("arm-none-eabi-gcc"), str(compiler))
            self.assertEqual(
                e.os.environ["PICO_TOOLCHAIN_PATH"], str(toolchain / "bin")
            )

    def test_command_cancellation_recorded_and_child_stopped(self):
        child = Mock()
        child.wait.side_effect = [KeyboardInterrupt(), 0]
        child.poll.return_value = None
        child.returncode = -15
        with (
            patch.object(e.subprocess, "Popen", return_value=child),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            with self.assertRaises(KeyboardInterrupt):
                e.command(["fake-command"])
        child.terminate.assert_called_once()
        self.assertIn("KeyboardInterrupt", e.COMMAND_LOG[-1]["termination"])
        self.assertEqual(e.COMMAND_LOG[-1]["returncode"], -15)
        e.COMMAND_LOG.clear()

    def test_starter_from_other_cwd(self):
        starter = Path(__file__).parent / "generator-check/run.sh"
        result = subprocess.run(
            [str(starter.resolve()), "--help"],
            cwd=self.directory,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("no device access", result.stdout)


if __name__ == "__main__":
    unittest.main()
