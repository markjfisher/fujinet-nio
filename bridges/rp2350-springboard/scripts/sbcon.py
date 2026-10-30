"""Talk to the Springboard bridge firmware console.

  sbcon.py                    print console output until Ctrl-C
  sbcon.py status regs        send commands, print what comes back, exit
  sbcon.py -t 30 trace on     ...and keep listening for 30 seconds
  sbcon.py --port "NIO link"  print the device NIO should open

Ports are found by USB interface name ("Console", "NIO link") on macOS, and
under /dev/serial/by-id on Linux. SBNIO_CONSOLE overrides the console device;
SBNIO_SERIAL selects one board by USB serial number when several are attached.
"""

import argparse
import glob
import os
import re
import select
import subprocess
import sys
import termios
import time

# Interface numbers of the two CDC ports (src/usb_descriptors.c).
LINUX_INTERFACE = {"NIO link": "if00", "Console": "if02"}


def find_port_macos(name):
    """Map a CDC port's interface name to its /dev/cu.* path. The name is on
    the control interface; the serial device hangs off the data interface
    after it."""
    tree = subprocess.run(
        ["ioreg", "-r", "-n", "Springboard NIO", "-l", "-w0"],
        capture_output=True,
        text=True,
    ).stdout
    wanted = os.environ.get("SBNIO_SERIAL")
    for device in re.split(r"(?m)^\+-o ", tree)[1:]:
        m = re.search(r'"USB Serial Number" = "([^"]+)"', device)
        if wanted and (not m or m.group(1) != wanted):
            continue
        current = None
        for line in device.splitlines():
            m = re.search(r'"kUSBString" = "([^"]+)"', line)
            if m:
                current = m.group(1)
            m = re.search(r'"IOCalloutDevice" = "([^"]+)"', line)
            if m and current == name:
                return m.group(1)
    return None


def find_port_linux(name):
    serial = os.environ.get("SBNIO_SERIAL", "*")
    pattern = f"/dev/serial/by-id/usb-*Springboard_NIO_{serial}-{LINUX_INTERFACE[name]}"
    matches = sorted(glob.glob(pattern))
    return os.path.realpath(matches[0]) if matches else None


def find_port(name):
    if sys.platform == "darwin":
        return find_port_macos(name)
    return find_port_linux(name)


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = attrs[1] = attrs[3] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def reopen():
    while True:
        time.sleep(1)
        path = os.environ.get("SBNIO_CONSOLE") or find_port("Console")
        if path:
            try:
                fd = open_port(path)
                sys.stdout.write("[console reconnected]\n")
                sys.stdout.flush()
                return fd
            except OSError:
                pass


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "-t",
        "--time",
        type=float,
        help="seconds to listen (default: forever, or 1.5 after commands)",
    )
    parser.add_argument(
        "--port",
        metavar="NAME",
        choices=sorted(LINUX_INTERFACE),
        help='print the device for a port ("NIO link" or "Console") and exit',
    )
    parser.add_argument(
        "command", nargs="*", help="console commands; multi-word ones need quotes"
    )
    args = parser.parse_args()
    if args.port:
        path = find_port(args.port)
        if not path:
            sys.exit(f"no Springboard NIO {args.port!r} port found")
        print(path)
        return
    path = os.environ.get("SBNIO_CONSOLE") or find_port("Console")
    if not path:
        sys.exit("no Springboard NIO console port found")
    fd = open_port(path)
    for command in args.command:
        os.write(fd, command.encode() + b"\n")
        time.sleep(0.05)
    limit = args.time if args.time is not None else (1.5 if args.command else None)
    end = time.time() + limit if limit else None
    try:
        while end is None or time.time() < end:
            ready, _, _ = select.select([fd], [], [], 0.2)
            if not ready:
                continue
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                continue
            except OSError:
                # The board re-enumerated (flash or reset): follow it when listening forever.
                os.close(fd)
                if end is not None:
                    return
                fd = reopen()
                continue
            sys.stdout.write(data.decode(errors="replace"))
            sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    os.close(fd)


if __name__ == "__main__":
    main()
