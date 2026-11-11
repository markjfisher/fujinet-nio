#!/usr/bin/env python3
"""Offline enabled/disabled build audit. Reads binaries; never executes NIO/USB."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def dependencies(binary):
    command = ['otool', '-L'] if sys.platform == 'darwin' else ['readelf', '-d']
    return subprocess.check_output(command + [str(binary)], text=True)


def audit(usb_build, ordinary_build):
    report = {}
    for mode, root in [('usb', usb_build), ('disabled', ordinary_build)]:
        commands = json.loads((root / 'compile_commands.json').read_text())
        visor = [r['file'] for r in commands if '/visor_usb/' in r['file'] or
                 r['file'].endswith('/build_profile/visor_usb.cpp')]
        binary = root / 'fujinet-nio'
        linked = 'libusb' in dependencies(binary)
        if mode == 'usb':
            assert visor and linked, 'enabled build missing Visor/libusb'
        else:
            assert not visor and not linked, 'USB leaked into ordinary build'
            assert all('libusb' not in r['command'] for r in commands), 'USB headers leaked'
        report[mode] = dict(binary=str(binary), visor_compile_units=visor,
                            links_libusb=linked,
                            sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    mock = usb_build / 'tests/visor-usb-tests'
    assert 'libusb' not in dependencies(mock), 'USB test binary links real libusb'
    report['mock_tests_link_libusb'] = False
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('usb_build', type=Path)
    parser.add_argument('ordinary_build', type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.usb_build, args.ordinary_build), indent=2))


if __name__ == '__main__':
    main()
