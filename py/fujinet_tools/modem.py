from __future__ import annotations

import sys
import time
from typing import Optional

from .common import open_serial, status_ok
from .fujibus import FujiBusSession, FujiPacket
from . import modemproto as mp
from .byte_proto import u16le


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


