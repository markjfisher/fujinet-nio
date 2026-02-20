#!/usr/bin/env python3
"""
FujiNet-NIO Integration Test Suite

Runs integration tests against both POSIX and ESP32 targets.
Wraps the existing run_integration.py with simplified arguments.

Usage:
    python suite.py --posix-port /dev/pts/2 --esp32-port /dev/ttyUSB0 --services-ip 192.168.1.101

Requirements:
    - Test services must be running (./scripts/start_test_services.sh all)
    - ESP32 must be flashed with fujinet-nio firmware
"""

import argparse
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional


def get_default_ip() -> str:
    """Get the default non-loopback IP address of this machine."""
    try:
        # Create a socket to determine the outbound IP
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            # Connect to an external address (doesn't actually send data)
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"


@dataclass
class TestResult:
    target: str
    passed: bool
    output: str
    duration_ms: float


class IntegrationSuite:
    def __init__(self, posix_port: str, esp32_port: str, services_ip: str, verbose: bool = False):
        self.posix_port = posix_port
        self.esp32_port = esp32_port
        self.services_ip = services_ip
        self.verbose = verbose
        self.suite_dir = Path(__file__).parent
        self.results: List[TestResult] = []

    def run_integration(self, target: str, port: str, ip: str, 
                        only_file: Optional[str] = None) -> TestResult:
        """Run integration tests for a specific target."""
        cmd = [
            sys.executable,
            str(self.suite_dir / "run_integration.py"),
            "--port", port,
            "--fs", "host",
            "--ip", ip,
        ]

        if target == "esp32":
            cmd.extend(["--esp32"])

        if only_file:
            cmd.extend(["--only-file", only_file])

        start = time.time()
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=300  # 5 minute timeout
            )
            duration_ms = (time.time() - start) * 1000
            output = result.stdout + result.stderr
            passed = result.returncode == 0
            return TestResult(target, passed, output, duration_ms)
        except subprocess.TimeoutExpired:
            duration_ms = (time.time() - start) * 1000
            return TestResult(target, False, "TIMEOUT", duration_ms)
        except Exception as e:
            duration_ms = (time.time() - start) * 1000
            return TestResult(target, False, str(e), duration_ms)

    def run_target_tests(self, target: str, port: str, ip: str) -> List[TestResult]:
        """Run all integration test groups for a target."""
        results = []

        # Test groups to run (matching step files)
        test_groups = [
            ("00_http.yaml", "HTTP"),
            ("01_http_request_headers.yaml", "HTTP Headers"),
            ("03_http_streaming_body.yaml", "HTTP Streaming"),
            ("10_tcp.yaml", "TCP"),
            ("11_tls.yaml", "TLS"),
            ("20_clock.yaml", "Clock"),
            ("30_filesystem.yaml", "Filesystem"),
            ("40_disk_raw.yaml", "Disk"),
        ]

        print(f"\n{'='*60}")
        print(f"Running integration tests for {target.upper()}")
        print(f"  Port: {port}")
        print(f"  IP: {ip}")
        print(f"{'='*60}")

        for file_name, test_name in test_groups:
            print(f"\n  [{test_name}] ", end="", flush=True)
            result = self.run_integration(target, port, ip, only_file=file_name)
            results.append(result)

            if result.passed:
                print(f"✓ PASSED ({result.duration_ms:.0f}ms)")
            else:
                print(f"✗ FAILED ({result.duration_ms:.0f}ms)")
                if self.verbose:
                    # Print last 20 lines of output
                    lines = result.output.strip().split('\n')
                    for line in lines[-20:]:
                        print(f"    {line}")

        return results

    def run_all(self) -> bool:
        """Run all tests for all targets."""
        all_passed = True

        # Test POSIX
        if self.posix_port:
            results = self.run_target_tests("posix", self.posix_port, "127.0.0.1")
            self.results.extend(results)
            if not all(r.passed for r in results):
                all_passed = False

        # Test ESP32
        if self.esp32_port and self.services_ip:
            results = self.run_target_tests("esp32", self.esp32_port, self.services_ip)
            self.results.extend(results)
            if not all(r.passed for r in results):
                all_passed = False

        return all_passed

    def print_summary(self):
        """Print test summary."""
        print(f"\n{'='*60}")
        print("INTEGRATION TEST SUMMARY")
        print(f"{'='*60}")

        # Group by target
        targets = sorted(set(r.target for r in self.results))

        total_passed = 0
        total_failed = 0

        for target in targets:
            target_results = [r for r in self.results if r.target == target]
            passed = sum(1 for r in target_results if r.passed)
            failed = sum(1 for r in target_results if not r.passed)
            total_passed += passed
            total_failed += failed

            print(f"\n{target.upper()}:")
            for r in target_results:
                status = "✓" if r.passed else "✗"
                print(f"  {status} ({r.duration_ms:.0f}ms)")
            print(f"  Total: {passed} passed, {failed} failed")

        print(f"\n{'='*60}")
        print(f"OVERALL: {total_passed} passed, {total_failed} failed")
        print(f"{'='*60}")


def main():
    default_ip = get_default_ip()

    parser = argparse.ArgumentParser(
        description="Run FujiNet-NIO integration tests against POSIX and ESP32 targets",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    # Run against POSIX only
    python suite.py --posix-port /dev/pts/2

    # Run against ESP32 only  
    python suite.py --esp32-port /dev/ttyUSB0

    # Run against both with explicit services IP
    python suite.py --posix-port /dev/pts/2 --esp32-port /dev/ttyUSB0 --services-ip 192.168.1.101

Prerequisites:
    - Test services must be running: ./scripts/start_test_services.sh all
    - ESP32 must be flashed with fujinet-nio firmware
        """
    )

    parser.add_argument("-p", "--posix-port", type=str, default="",
                        help="Serial port for POSIX target (e.g., /dev/pts/2)")
    parser.add_argument("-e", "--esp32-port", type=str, default="",
                        help="Serial port for ESP32 target (e.g., /dev/ttyUSB0)")
    parser.add_argument("-s", "--services-ip", type=str, default=default_ip,
                        help=f"IP address where test services are running (default: {default_ip})")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Show verbose output")

    args = parser.parse_args()

    if not args.posix_port and not args.esp32_port:
        parser.error("At least one of --posix-port or --esp32-port is required")

    suite = IntegrationSuite(
        posix_port=args.posix_port,
        esp32_port=args.esp32_port,
        services_ip=args.services_ip,
        verbose=args.verbose
    )

    success = suite.run_all()
    suite.print_summary()

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
