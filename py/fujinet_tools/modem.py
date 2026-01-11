from __future__ import annotations

import sys
import time
from typing import Optional

from .common import open_serial, status_ok
from .fujibus import FujiBusSession, FujiPacket
from . import modemproto as mp
from .byte_proto import u16le

try:
    import select  # type: ignore
    import termios  # type: ignore
    import tty  # type: ignore
except Exception:
    select = None  # type: ignore
    termios = None  # type: ignore
    tty = None  # type: ignore


STATUS_TEXT = {
    0: "Ok",
    1: "DeviceNotFound",
    2: "InvalidRequest",
    3: "DeviceBusy",
    4: "NotReady",
    5: "IOError",
    6: "Timeout",
    7: "InternalError",
    8: "Unsupported",
}


def _status_str(code: int) -> str:
    return STATUS_TEXT.get(code, f"Unknown({code})")


def _pkt_status_code(pkt: Optional[FujiPacket]) -> int:
    if not pkt or not pkt.params:
        return -1
    return int(pkt.params[0])


def _send(
    *,
    bus: FujiBusSession,
    device: int,
    command: int,
    payload: bytes,
    timeout: float,
    cmd_txt: str,
) -> Optional[FujiPacket]:
    return bus.send_command_expect(
        device=device,
        command=command,
        payload=payload,
        expect_device=device,
        expect_command=command,
        timeout=timeout,
        cmd_txt=cmd_txt,
    )


def _get_status(bus: FujiBusSession, timeout: float, debug: bool) -> mp.StatusResp:
    pkt = _send(
        bus=bus,
        device=mp.MODEM_DEVICE_ID,
        command=mp.CMD_STATUS,
        payload=mp.build_status_req(),
        timeout=timeout,
        cmd_txt="MODEM STATUS",
    )
    if pkt is None:
        raise RuntimeError("No response")
    if not status_ok(pkt):
        code = _pkt_status_code(pkt)
        raise RuntimeError(f"Device status={code} ({_status_str(code)})")
    return mp.parse_status_resp(pkt.payload)


def _drain(
    *,
    bus: FujiBusSession,
    timeout: float,
    max_total: int = 8192,
    quiet: bool = False,
    as_text: bool = True,
) -> bytes:
    """
    Drain modem output by reading sequentially using the device-reported cursor.
    Stops when:
      - no new bytes arrive for a short time, or
      - max_total bytes drained.
    """
    st = _get_status(bus, timeout=timeout, debug=False)
    off = st.host_read_cursor

    out = bytearray()
    idle_deadline = time.monotonic() + 0.25
    while len(out) < max_total:
        pkt = _send(
            bus=bus,
            device=mp.MODEM_DEVICE_ID,
            command=mp.CMD_READ,
            payload=mp.build_read_req(offset=off, max_bytes=512),
            timeout=min(timeout, 0.2),
            cmd_txt="MODEM READ",
        )
        if pkt is None:
            break
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            raise RuntimeError(f"Device status={code} ({_status_str(code)})")

        rr = mp.parse_read_resp(pkt.payload)
        if rr.offset != off:
            raise RuntimeError(f"Offset mismatch: expected {off}, got {rr.offset}")

        if rr.data:
            out += rr.data
            off += len(rr.data)
            idle_deadline = time.monotonic() + 0.25
            continue

        if time.monotonic() > idle_deadline:
            break
        time.sleep(0.02)

    return bytes(out)


class _RawTerminal:
    """
    Put stdin into raw mode (POSIX) for interactive modem sessions.
    Safe no-op on non-POSIX platforms (command will error earlier).
    """
    def __init__(self) -> None:
        self._fd = None
        self._old = None

    def __enter__(self):
        if termios is None or tty is None:
            return self
        self._fd = sys.stdin.fileno()
        self._old = termios.tcgetattr(self._fd)
        tty.setraw(self._fd)
        return self

    def __exit__(self, exc_type, exc, tb):
        if termios is None:
            return False
        if self._fd is not None and self._old is not None:
            termios.tcsetattr(self._fd, termios.TCSADRAIN, self._old)
        return False


# ----------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------


def cmd_modem_status(args) -> int:
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        st = _get_status(bus, timeout=args.timeout, debug=args.debug)
        print(f"cmd_mode={int(st.cmd_mode)} connected={int(st.connected)} listen_port={st.listen_port}")
        print(f"host_rx_avail={st.host_rx_avail} host_write_cursor={st.host_write_cursor}")
        print(f"net_read_cursor={st.net_read_cursor} net_write_cursor={st.net_write_cursor}")
        print(f"flags=0x{st.flags:02X}")
        return 0


def cmd_modem_drain(args) -> int:
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        out = _drain(bus=bus, timeout=args.timeout, max_total=args.max_bytes)
        if args.out:
            from pathlib import Path
            Path(args.out).write_bytes(out)
        else:
            # default: write to stdout as raw bytes
            sys.stdout.buffer.write(out)
        return 0


def cmd_modem_at(args) -> int:
    # Build the AT command line
    s = args.command
    if not (s.upper().startswith("AT")):
        s = "AT" + s
    if not s.endswith("\r") and not s.endswith("\n"):
        s += "\r"

    data = s.encode("utf-8", errors="replace")

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        st = _get_status(bus, timeout=args.timeout, debug=args.debug)
        off = st.host_write_cursor

        pkt = _send(
            bus=bus,
            device=mp.MODEM_DEVICE_ID,
            command=mp.CMD_WRITE,
            payload=mp.build_write_req(offset=off, data=data),
            timeout=args.timeout,
            cmd_txt="MODEM WRITE",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        # Drain response text
        out = _drain(bus=bus, timeout=args.timeout, max_total=args.read_max)
        sys.stdout.buffer.write(out)
        return 0


def cmd_modem_dial(args) -> int:
    hostport = mp.normalize_hostport(args.target)
    payload = hostport.encode("utf-8", errors="replace")
    if len(payload) > 0xFFFF:
        print("Target too long")
        return 2

    # op=0x02 Dial: lp_u16 string host[:port]
    req = mp.build_control_req(0x02, data=u16le(len(payload)) + payload)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = _send(
            bus=bus,
            device=mp.MODEM_DEVICE_ID,
            command=mp.CMD_CONTROL,
            payload=req,
            timeout=args.timeout,
            cmd_txt="MODEM DIAL",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        # Drain until we see CONNECT or timeout
        deadline = time.monotonic() + args.timeout
        out = bytearray()
        while time.monotonic() < deadline:
            chunk = _drain(bus=bus, timeout=args.timeout, max_total=4096)
            if chunk:
                out += chunk
                if b"CONNECT" in out or b"NO CARRIER" in out:
                    break
            else:
                time.sleep(0.02)
        sys.stdout.buffer.write(out)
        return 0


def cmd_modem_write(args) -> int:
    data = args.data.encode("utf-8", errors="replace") if args.data is not None else b""
    if args.inp:
        from pathlib import Path
        data = Path(args.inp).read_bytes()

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        st = _get_status(bus, timeout=args.timeout, debug=args.debug)
        off = st.host_write_cursor if args.offset is None else int(args.offset)

        pkt = _send(
            bus=bus,
            device=mp.MODEM_DEVICE_ID,
            command=mp.CMD_WRITE,
            payload=mp.build_write_req(offset=off, data=data),
            timeout=args.timeout,
            cmd_txt="MODEM WRITE",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        wr = mp.parse_write_resp(pkt.payload)
        print(f"offset={wr.offset} written={wr.written}")
        return 0


def cmd_modem_read(args) -> int:
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        st = _get_status(bus, timeout=args.timeout, debug=args.debug)
        off = st.host_read_cursor if args.offset is None else int(args.offset)

        pkt = _send(
            bus=bus,
            device=mp.MODEM_DEVICE_ID,
            command=mp.CMD_READ,
            payload=mp.build_read_req(offset=off, max_bytes=args.max_bytes),
            timeout=args.timeout,
            cmd_txt="MODEM READ",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        rr = mp.parse_read_resp(pkt.payload)
        if args.out:
            from pathlib import Path
            Path(args.out).write_bytes(rr.data)
        else:
            sys.stdout.buffer.write(rr.data)
        if args.verbose:
            print(f"\n(offset={rr.offset} len={len(rr.data)})")
        return 0


def cmd_modem_sendrecv(args) -> int:
    """
    Convenience: write bytes, then read until we got the same number of bytes back.
    Intended for use with the TCP echo service.
    """
    data = args.data.encode("utf-8", errors="replace")
    want = len(data)
    if want == 0:
        print("empty data")
        return 2

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        # Drain any pending modem output (CONNECT banners, etc.) so the echo read is clean.
        try:
            _ = _drain(bus=bus, timeout=args.timeout, max_total=4096)
        except Exception:
            # best-effort; proceed
            pass

        # write at current cursor
        st = _get_status(bus, timeout=args.timeout, debug=args.debug)
        woff = st.host_write_cursor
        pkt = _send(
            bus=bus,
            device=mp.MODEM_DEVICE_ID,
            command=mp.CMD_WRITE,
            payload=mp.build_write_req(offset=woff, data=data),
            timeout=args.timeout,
            cmd_txt="MODEM WRITE",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        # read until we have want bytes
        out = bytearray()
        deadline = time.monotonic() + args.timeout
        while len(out) < want and time.monotonic() < deadline:
            st2 = _get_status(bus, timeout=args.timeout, debug=args.debug)
            roff = st2.host_read_cursor
            pkt = _send(
                bus=bus,
                device=mp.MODEM_DEVICE_ID,
                command=mp.CMD_READ,
                payload=mp.build_read_req(offset=roff, max_bytes=min(512, want - len(out))),
                timeout=min(args.timeout, 0.2),
                cmd_txt="MODEM READ",
            )
            if pkt is None:
                time.sleep(0.02)
                continue
            if not status_ok(pkt):
                code = _pkt_status_code(pkt)
                print(f"Device status={code} ({_status_str(code)})")
                return 1
            rr = mp.parse_read_resp(pkt.payload)
            if rr.data:
                out += rr.data
            else:
                time.sleep(0.02)

        if len(out) < want:
            print("timeout waiting for echo")
            return 1

        sys.stdout.buffer.write(bytes(out))
        return 0


def cmd_modem_hangup(args) -> int:
    # op=0x01 Hangup
    req = mp.build_control_req(0x01)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = _send(
            bus=bus,
            device=mp.MODEM_DEVICE_ID,
            command=mp.CMD_CONTROL,
            payload=req,
            timeout=args.timeout,
            cmd_txt="MODEM HANGUP",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        # Drain any textual result the device emitted (NO CARRIER / OK).
        out = _drain(bus=bus, timeout=args.timeout, max_total=args.read_max)
        sys.stdout.buffer.write(out)
        return 0


def cmd_modem_term(args) -> int:
    """
    Interactive terminal session:
      - reads from stdin and writes to modem (Write)
      - reads from modem output (Read) and writes to stdout

    Exit: Ctrl-] (0x1D) or Ctrl-C.
    """
    if select is None:
        print("error: interactive mode not supported on this platform")
        return 2

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        # Optional pre-dial.
        if args.dial:
            # Keep telnet enabled by default (most BBSes are telnet on 23).
            # User can run: modem at ATNET0 before this if they want raw TCP.
            hostport = mp.normalize_hostport(args.dial)
            payload = hostport.encode("utf-8", errors="replace")
            req = mp.build_control_req(0x02, data=u16le(len(payload)) + payload)
            pkt = _send(
                bus=bus,
                device=mp.MODEM_DEVICE_ID,
                command=mp.CMD_CONTROL,
                payload=req,
                timeout=args.timeout,
                cmd_txt="MODEM DIAL",
            )
            if pkt is None:
                print("No response")
                return 2
            if not status_ok(pkt):
                code = _pkt_status_code(pkt)
                print(f"Device status={code} ({_status_str(code)})")
                return 1

        # Prime cursors from status.
        st = _get_status(bus, timeout=args.timeout, debug=args.debug)
        woff = st.host_write_cursor
        roff = st.host_read_cursor
        was_connected = bool(st.connected)

        # Status polling cadence + idle backoff (reduces FujiBus chatter).
        next_status_at = time.monotonic()
        idle_sleep = 0.002
        idle_sleep_max = 0.05

        sys.stdout.write("\n[fujinet modem] interactive session (exit: Ctrl-])\n")
        sys.stdout.flush()

        with _RawTerminal():
            while True:
                now = time.monotonic()

                # Periodic status poll: used to decide if we should issue READ requests at all,
                # and to detect disconnect transitions.
                if now >= next_status_at:
                    try:
                        st = _get_status(bus, timeout=min(args.timeout, 0.2), debug=args.debug)
                        woff = st.host_write_cursor

                        # If we were connected and now we're not, exit (server hangup / local hangup).
                        if was_connected and not st.connected:
                            # Drain any remaining output (e.g. "NO CARRIER") then exit.
                            try:
                                out = _drain(bus=bus, timeout=min(args.timeout, 0.5), max_total=4096)
                                if out:
                                    sys.stdout.buffer.write(out)
                                    sys.stdout.buffer.flush()
                            except Exception:
                                pass
                            sys.stdout.write("\n[fujinet modem] disconnected\n")
                            sys.stdout.flush()
                            return 0

                        was_connected = bool(st.connected)
                    except Exception:
                        # If status fails transiently, don't abort the session immediately.
                        pass
                    next_status_at = now + 0.10  # 10Hz

                # Read from modem -> stdout
                did_read = False
                if getattr(st, "host_rx_avail", 0) > 0:
                    pkt = _send(
                        bus=bus,
                        device=mp.MODEM_DEVICE_ID,
                        command=mp.CMD_READ,
                        payload=mp.build_read_req(offset=roff, max_bytes=args.read_chunk),
                        timeout=0.05,
                        cmd_txt="MODEM READ",
                    )
                    if pkt is not None:
                        if not status_ok(pkt):
                            code = _pkt_status_code(pkt)
                            # If our read offset got out of sync, resync and continue.
                            if code == 2:  # InvalidRequest
                                try:
                                    st2 = _get_status(bus, timeout=args.timeout, debug=args.debug)
                                    roff = st2.host_read_cursor
                                    continue
                                except Exception:
                                    pass
                            print(f"\nDevice status={code} ({_status_str(code)})")
                            return 1
                        rr = mp.parse_read_resp(pkt.payload)
                        if rr.offset != roff:
                            # Resync offsets (e.g. if another tool consumed output).
                            st2 = _get_status(bus, timeout=args.timeout, debug=args.debug)
                            roff = st2.host_read_cursor
                        else:
                            if rr.data:
                                sys.stdout.buffer.write(rr.data)
                                sys.stdout.buffer.flush()
                                roff += len(rr.data)
                                did_read = True

                # Read from stdin -> modem
                rlist, _, _ = select.select([sys.stdin], [], [], 0.0)
                if rlist:
                    b = sys.stdin.buffer.read1(1024)
                    if not b:
                        time.sleep(0.01)
                        continue

                    # Exit key: Ctrl-]
                    if b and 0x1D in b:
                        return 0

                    pkt = _send(
                        bus=bus,
                        device=mp.MODEM_DEVICE_ID,
                        command=mp.CMD_WRITE,
                        payload=mp.build_write_req(offset=woff, data=b),
                        timeout=args.timeout,
                        cmd_txt="MODEM WRITE",
                    )
                    if pkt is None:
                        continue
                    if not status_ok(pkt):
                        code = _pkt_status_code(pkt)
                        # Try to resync write cursor once.
                        if code == 2:  # InvalidRequest
                            st = _get_status(bus, timeout=args.timeout, debug=args.debug)
                            woff = st.host_write_cursor
                            continue
                        print(f"\nDevice status={code} ({_status_str(code)})")
                        return 1
                    wr = mp.parse_write_resp(pkt.payload)
                    woff = wr.offset + wr.written
                    # Writing implies we're active; avoid long idle sleeps.
                    did_read = True

                # Backoff when idle to reduce bus chatter and improve throughput.
                if did_read:
                    idle_sleep = 0.002
                else:
                    idle_sleep = min(idle_sleep_max, idle_sleep * 1.25)
                time.sleep(idle_sleep)


def register_subcommands(subparsers) -> None:
    pm = subparsers.add_parser("modem", help="Modem device commands (AT + binary protocol)")
    msub = pm.add_subparsers(dest="modem_cmd", required=True)

    ps = msub.add_parser("status", help="Show modem status")
    ps.set_defaults(fn=cmd_modem_status)

    pa = msub.add_parser("at", help="Send an AT command and print modem output")
    pa.add_argument("command", help="AT command (with or without leading AT)")
    pa.add_argument("--read-max", type=int, default=2048)
    pa.set_defaults(fn=cmd_modem_at)

    pd = msub.add_parser("dial", help="Dial a host:port or tcp://host:port and print output (CONNECT/NO CARRIER)")
    pd.add_argument("target")
    pd.set_defaults(fn=cmd_modem_dial)

    pdr = msub.add_parser("drain", help="Drain modem output bytes")
    pdr.add_argument("--max-bytes", type=int, default=4096)
    pdr.add_argument("--out", help="Write bytes to file (else stdout)")
    pdr.set_defaults(fn=cmd_modem_drain)

    pw = msub.add_parser("write", help="Write bytes to modem (connected or command mode)")
    pw.add_argument("--offset", type=int, default=None, help="Override write offset (default: use status cursor)")
    src = pw.add_mutually_exclusive_group(required=True)
    src.add_argument("--data", help="Send these UTF-8 bytes")
    src.add_argument("--inp", help="Read bytes from this file")
    pw.set_defaults(fn=cmd_modem_write)

    pr = msub.add_parser("read", help="Read bytes from modem output buffer (single chunk)")
    pr.add_argument("--offset", type=int, default=None, help="Override read offset (default: use status cursor)")
    pr.add_argument("--max-bytes", type=int, default=512)
    pr.add_argument("--out", help="Write bytes to file (else stdout)")
    pr.set_defaults(fn=cmd_modem_read)

    psr = msub.add_parser("sendrecv", help="Write bytes, then read back same number of bytes (TCP echo convenience)")
    psr.add_argument("--data", required=True, help="UTF-8 bytes to send")
    psr.set_defaults(fn=cmd_modem_sendrecv)

    ph = msub.add_parser("hangup", help="Hang up (binary control op; emits NO CARRIER if connected)")
    ph.add_argument("--read-max", type=int, default=2048)
    ph.set_defaults(fn=cmd_modem_hangup)

    pt = msub.add_parser("term", help="Interactive terminal bridge to the modem (stdin/stdout)")
    pt.add_argument("--dial", default=None, help="Optional: dial host (host[:port] or tcp://host:port) before entering terminal mode")
    pt.add_argument("--read-chunk", type=int, default=512, help="Read chunk size from modem")
    pt.set_defaults(fn=cmd_modem_term)


