# py/fujinet_tools/net_tcp.py
from __future__ import annotations

import sys
import time
import serial # type: ignore
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Tuple

from .fujibus import FujiBusSession
from . import netproto as np
from .common import status_ok, open_serial

# Reuse status text mapping from net.py style (keep local so this file is standalone)
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

def _pkt_status_code(pkt) -> int:
    if not pkt or not pkt.params:
        return -1
    return int(pkt.params[0])

def _send_retry(
    *,
    bus: FujiBusSession,
    device: int,
    command: int,
    payload: bytes,
    timeout: float,
    retries: int,
    sleep_s: float,
    cmd_txt: str = "",
):
    """
    Send a FujiBus command and wait for the matching response packet.

    Retries on:
      - NotReady (4)
      - DeviceBusy (3)
      - None (no response packet within per-attempt timeout)

    Uses FujiBusSession.send_command_expect(), which:
      - sends the request
      - reads SLIP frames until it sees (expect_device, expect_command)
      - stashes other packets
    """
    deadline = time.monotonic() + max(timeout, 0.0)
    backoff = max(0.001, sleep_s)
    backoff_max = 0.05

    attempt = 0
    while True:
        attempt += 1

        now = time.monotonic()
        remaining = deadline - now
        if remaining <= 0:
            return None

        # keep each attempt short so we can handle NotReady/DeviceBusy with backoff
        per_attempt_timeout = min(0.05, remaining)

        pkt = bus.send_command_expect(
            device=device,
            command=command,
            payload=payload,
            expect_device=device,
            expect_command=command,
            timeout=per_attempt_timeout,
            cmd_txt=cmd_txt,
        )

        if pkt is None:
            if attempt >= retries or time.monotonic() >= deadline:
                return None
            time.sleep(backoff)
            backoff = min(backoff_max, backoff * 1.5)
            continue

        sc = _pkt_status_code(pkt)
        if sc not in (3, 4):  # DeviceBusy, NotReady
            return pkt

        if attempt >= retries or time.monotonic() >= deadline:
            return pkt

        time.sleep(backoff)
        backoff = min(backoff_max, backoff * 1.5)


def _tcp_url_with_opts(
    host: str,
    port: int,
    *,
    connect_timeout_ms: Optional[int] = None,
    nodelay: Optional[bool] = None,
    keepalive: Optional[bool] = None,
    rx_buf: Optional[int] = None,
    halfclose: Optional[bool] = None,
) -> str:
    base = f"tcp://{host}:{port}"
    opts = []
    if connect_timeout_ms is not None:
        opts.append(f"connect_timeout_ms={int(connect_timeout_ms)}")
    if nodelay is not None:
        opts.append(f"nodelay={1 if nodelay else 0}")
    if keepalive is not None:
        opts.append(f"keepalive={1 if keepalive else 0}")
    if rx_buf is not None:
        opts.append(f"rx_buf={int(rx_buf)}")
    if halfclose is not None:
        opts.append(f"halfclose={1 if halfclose else 0}")
    if not opts:
        return base
    return base + "?" + "&".join(opts)


def _parse_tcp_connected_from_info(ir: np.InfoResp) -> Tuple[bool, bool]:
    """
    Returns (connected, connecting) from TCP pseudo headers, if present.
    If headers are absent or unparsable, returns (False, False).
    """
    if not ir.header_bytes:
        return (False, False)

    text = ir.header_bytes.decode("utf-8", errors="replace")
    connected = False
    connecting = False
    for line in text.splitlines():
        if line.startswith("X-FujiNet-Connected:"):
            connected = line.split(":", 1)[1].strip() == "1"
        elif line.startswith("X-FujiNet-Connecting:"):
            connecting = line.split(":", 1)[1].strip() == "1"
    return (connected, connecting)


@dataclass
class TcpStreamSession:
    handle: int
    read_offset: int = 0
    write_offset: int = 0


def tcp_open(
    *,
    bus: FujiBusSession,
    url: str,
    timeout: float,
    wait_connected: bool,
    info_poll_s: float,
) -> TcpStreamSession:
    # method is ignored by TCP backend; use GET (1) for consistency
    open_req = np.build_open_req(method=1, flags=0, url=url, headers=[], body_len_hint=0)
    pkt = _send_retry(
        bus=bus,
        device=np.NETWORK_DEVICE_ID,
        command=np.CMD_OPEN,
        payload=open_req,
        timeout=timeout,
        retries=100,
        sleep_s=0.01,
        cmd_txt="OPEN",
    )
    if pkt is None:
        raise RuntimeError("No response to Open")
    if not status_ok(pkt):
        code = _pkt_status_code(pkt)
        raise RuntimeError(f"Open failed: status={code} ({_status_str(code)})")

    orr = np.parse_open_resp(pkt.payload)
    if not orr.accepted:
        raise RuntimeError("Open not accepted")

    sess = TcpStreamSession(handle=orr.handle)

    if not wait_connected:
        return sess

    # poll Info until connected=1 or timeout
    deadline = time.monotonic() + max(timeout, 0.0)
    while time.monotonic() < deadline:
        info_req = np.build_info_req(sess.handle)
        ipkt = _send_retry(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_INFO,
            payload=info_req,
            timeout=timeout,
            retries=50,
            sleep_s=0.01,
            cmd_txt="INFO",
        )
        if ipkt is None:
            time.sleep(info_poll_s)
            continue
        if not status_ok(ipkt):
            code = _pkt_status_code(ipkt)
            # NotReady just means "try again"; any other error is fatal
            if code != 4:
                raise RuntimeError(f"Info failed: status={code} ({_status_str(code)})")
            time.sleep(info_poll_s)
            continue

        ir = np.parse_info_resp(ipkt.payload)
        connected, connecting = _parse_tcp_connected_from_info(ir)
        if connected:
            return sess

        # If device doesn't provide the pseudo headers yet, we still keep polling.
        time.sleep(info_poll_s)

    raise RuntimeError("Timed out waiting for TCP connect")


def tcp_send(
    *,
    bus: FujiBusSession,
    sess: TcpStreamSession,
    data: bytes,
    timeout: float,
    chunk: int,
) -> int:
    """
    Send bytes over TCP stream, respecting sequential offsets.
    Returns number of bytes accepted (should equal len(data) unless error).
    """
    total = 0
    while total < len(data):
        part = data[total : total + chunk]
        wreq = np.build_write_req(sess.handle, sess.write_offset, part)
        wpkt = _send_retry(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_WRITE,
            payload=wreq,
            timeout=timeout,
            retries=5000,
            sleep_s=0.001,
            cmd_txt="WRITE",
        )
        if wpkt is None:
            raise RuntimeError("No response to Write")
        if not status_ok(wpkt):
            code = _pkt_status_code(wpkt)
            raise RuntimeError(f"Write failed: status={code} ({_status_str(code)})")

        wr = np.parse_write_resp(wpkt.payload)
        if wr.written <= 0:
            raise RuntimeError("Write returned 0 bytes written")
        sess.write_offset += wr.written
        total += wr.written

    return total


def tcp_halfclose(
    *,
    bus: FujiBusSession,
    sess: TcpStreamSession,
    timeout: float,
) -> None:
    """
    Optional send-finish: zero-length write at current cursor.
    Backend may map this to shutdown(SHUT_WR) if enabled.
    """
    wreq = np.build_write_req(sess.handle, sess.write_offset, b"")
    wpkt = _send_retry(
        bus=bus,
        device=np.NETWORK_DEVICE_ID,
        command=np.CMD_WRITE,
        payload=wreq,
        timeout=timeout,
        retries=200,
        sleep_s=0.01,
        cmd_txt="WRITE",
    )
    if wpkt is None:
        raise RuntimeError("No response to halfclose Write")
    if not status_ok(wpkt):
        code = _pkt_status_code(wpkt)
        raise RuntimeError(f"Halfclose failed: status={code} ({_status_str(code)})")


def tcp_recv_some(
    *,
    bus: FujiBusSession,
    sess: TcpStreamSession,
    timeout: float,
    max_bytes: int,
) -> Tuple[bytes, bool]:
    """
    Receive up to max_bytes. Returns (data, eof).
    If no data is currently available, returns (b"", False) after NotReady.
    """
    rreq = np.build_read_req(sess.handle, sess.read_offset, max_bytes)
    rpkt = _send_retry(
        bus=bus,
        device=np.NETWORK_DEVICE_ID,
        command=np.CMD_READ,
        payload=rreq,
        timeout=timeout,
        retries=200,
        sleep_s=0.005,
        cmd_txt="READ",
    )
    if rpkt is None:
        raise RuntimeError("No response to Read")
    if not status_ok(rpkt):
        code = _pkt_status_code(rpkt)
        # Treat NotReady as "no data" rather than an exception for interactive use
        if code == 4:
            return (b"", False)
        raise RuntimeError(f"Read failed: status={code} ({_status_str(code)})")

    rr = np.parse_read_resp(rpkt.payload)
    if rr.offset != sess.read_offset:
        raise RuntimeError(f"Offset echo mismatch: expected {sess.read_offset}, got {rr.offset}")

    sess.read_offset += len(rr.data)
    return (rr.data, rr.eof)


def tcp_info_print(
    *,
    bus: FujiBusSession,
    handle: int,
    timeout: float,
    max_headers: int,
) -> None:
    req = np.build_info_req(handle)
    pkt = _send_retry(
        bus=bus,
        device=np.NETWORK_DEVICE_ID,
        command=np.CMD_INFO,
        payload=req,
        timeout=timeout,
        retries=200,
        sleep_s=0.01,
        cmd_txt="INFO",
    )
    if pkt is None:
        print("No response")
        return
    if not status_ok(pkt):
        code = _pkt_status_code(pkt)
        print(f"Device status={code} ({_status_str(code)})")
        return
    ir = np.parse_info_resp(pkt.payload)
    print(f"handle={ir.handle} http_status={ir.http_status} content_length={ir.content_length}")
    if ir.header_bytes:
        sys.stdout.write(ir.header_bytes.decode("utf-8", errors="replace"))
        if not ir.header_bytes.endswith(b"\n"):
            sys.stdout.write("\n")


def tcp_close(*, bus: FujiBusSession, handle: int, timeout: float) -> None:
    req = np.build_close_req(handle)
    pkt = _send_retry(
        bus=bus,
        device=np.NETWORK_DEVICE_ID,
        command=np.CMD_CLOSE,
        payload=req,
        timeout=timeout,
        retries=100,
        sleep_s=0.01,
        cmd_txt="CLOSE",
    )
    if pkt is None:
        raise RuntimeError("No response to Close")
    if not status_ok(pkt):
        code = _pkt_status_code(pkt)
        raise RuntimeError(f"Close failed: status={code} ({_status_str(code)})")


def _hexdump(b: bytes, max_len: int = 256) -> str:
    s = b[:max_len].hex()
    if len(b) > max_len:
        s += f"... (+{len(b)-max_len} bytes)"
    return s

def _parse_hex_bytes(s: str) -> bytes:
    # accepts "aa bb cc" or "aabbcc"
    cleaned = "".join(ch for ch in s if ch.strip()).replace(" ", "")
    if len(cleaned) % 2 != 0:
        raise ValueError("hex string must have even length")
    return bytes.fromhex(cleaned)


# ----------------------------------------------------------------------
# CLI commands (plug into net.py under: fujinet net tcp ...)
# ----------------------------------------------------------------------

def cmd_net_tcp_repl(args) -> int:
    """
    Interactive TCP session.

    Commands:
      help
      open <tcp://...>          (or just start with URL arg)
      info                      (prints pseudo headers)
      send <text...>            (UTF-8)
      sendhex <aabbcc...>       (raw hex)
      recv [n]                  (read up to n bytes once; default read-chunk)
      drain [n]                 (keep reading until idle-timeout; optional max total bytes)
      halfclose                 (zero-length write)
      offsets                   (show read/write cursors)
      close
      quit / exit
    """
    url = args.url

    with open_serial(port=args.port, baud=args.baud, timeout_s=args.timeout) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        sess = None
        if url:
            sess = tcp_open(
                bus=bus,
                url=url,
                timeout=args.timeout,
                wait_connected=args.wait_connected,
                info_poll_s=args.info_poll,
            )
            if args.show_info:
                tcp_info_print(bus=bus, handle=sess.handle, timeout=args.timeout, max_headers=args.max_headers)
            print(f"[tcp] opened handle={sess.handle}")

        def ensure_open() -> TcpStreamSession:
            nonlocal sess
            if sess is None:
                raise RuntimeError("No open session. Use: open tcp://host:port")
            return sess

        print("TCP REPL. Type 'help' for commands.")
        while True:
            try:
                line = input("tcp> ").strip()
            except (EOFError, KeyboardInterrupt):
                line = "quit"

            if not line:
                continue

            parts = line.split()
            cmd = parts[0].lower()
            rest = line[len(parts[0]):].lstrip()

            try:
                if cmd in ("quit", "exit"):
                    if sess is not None:
                        try:
                            tcp_close(bus=bus, handle=sess.handle, timeout=args.timeout)
                            print("[tcp] closed")
                        except Exception:
                            pass
                    return 0

                if cmd == "help":
                    print(cmd_net_tcp_repl.__doc__ or "")
                    continue

                if cmd == "open":
                    if not rest:
                        print("usage: open tcp://host:port[?opts]")
                        continue
                    if sess is not None:
                        try:
                            tcp_close(bus=bus, handle=sess.handle, timeout=args.timeout)
                        except Exception:
                            pass
                    sess = tcp_open(
                        bus=bus,
                        url=rest,
                        timeout=args.timeout,
                        wait_connected=args.wait_connected,
                        info_poll_s=args.info_poll,
                    )
                    print(f"[tcp] opened handle={sess.handle}")
                    if args.show_info:
                        tcp_info_print(bus=bus, handle=sess.handle, timeout=args.timeout, max_headers=args.max_headers)
                    continue

                if cmd == "close":
                    s = ensure_open()
                    tcp_close(bus=bus, handle=s.handle, timeout=args.timeout)
                    print("[tcp] closed")
                    sess = None
                    continue

                if cmd == "info":
                    s = ensure_open()
                    tcp_info_print(bus=bus, handle=s.handle, timeout=args.timeout, max_headers=args.max_headers)
                    continue

                if cmd == "offsets":
                    s = ensure_open()
                    print(f"read_offset={s.read_offset} write_offset={s.write_offset}")
                    continue

                if cmd == "send":
                    s = ensure_open()
                    if not rest:
                        print("usage: send <text...>")
                        continue
                    data = rest.encode("utf-8")
                    n = tcp_send(bus=bus, sess=s, data=data, timeout=args.timeout, chunk=args.write_chunk)
                    print(f"[tcp] sent {n} bytes")
                    continue

                if cmd == "sendhex":
                    s = ensure_open()
                    if not rest:
                        print("usage: sendhex <aabbcc...>  (spaces ok)")
                        continue
                    data = _parse_hex_bytes(rest)
                    n = tcp_send(bus=bus, sess=s, data=data, timeout=args.timeout, chunk=args.write_chunk)
                    print(f"[tcp] sent {n} bytes")
                    continue

                if cmd == "halfclose":
                    s = ensure_open()
                    tcp_halfclose(bus=bus, sess=s, timeout=args.timeout)
                    print("[tcp] halfclosed (TX)")
                    continue

                if cmd == "recv":
                    s = ensure_open()
                    n = args.read_chunk
                    if len(parts) >= 2:
                        n = int(parts[1])
                    data, eof = tcp_recv_some(bus=bus, sess=s, timeout=args.timeout, max_bytes=n)
                    if data:
                        if args.binary:
                            # raw bytes to stdout
                            sys.stdout.buffer.write(data)
                            sys.stdout.buffer.flush()
                            print("")
                        else:
                            print(f"[tcp] {len(data)} bytes: {data!r}")
                            print(f"hex: {_hexdump(data)}")
                    else:
                        print("[tcp] (no data)")
                    if eof:
                        print("[tcp] EOF")
                    continue

                if cmd == "drain":
                    s = ensure_open()
                    max_total = None
                    if len(parts) >= 2:
                        max_total = int(parts[1])

                    total = 0
                    idle_deadline = time.monotonic() + max(args.idle_timeout, 0.25)
                    while True:
                        data, eof = tcp_recv_some(bus=bus, sess=s, timeout=args.timeout, max_bytes=args.read_chunk)
                        if data:
                            total += len(data)
                            idle_deadline = time.monotonic() + max(args.idle_timeout, 0.25)
                            if args.binary:
                                sys.stdout.buffer.write(data)
                                sys.stdout.buffer.flush()
                            else:
                                print(data.decode("utf-8", errors="replace"), end="")
                            if max_total is not None and total >= max_total:
                                break
                        if eof:
                            print("\n[tcp] EOF")
                            break
                        if time.monotonic() > idle_deadline:
                            break
                    if not args.binary:
                        print(f"\n[tcp] drained {total} bytes")
                    continue

                print(f"unknown command: {cmd} (try 'help')")

            except Exception as e:
                print(f"error: {e}")

    return 0

def cmd_net_tcp_connect(args) -> int:
    with open_serial(port=args.port, baud=args.baud, timeout_s=args.timeout) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        url = args.url
        if args.host and args.port:
            url = _tcp_url_with_opts(
                args.host,
                args.port,
                connect_timeout_ms=args.connect_timeout_ms,
                nodelay=args.nodelay,
                keepalive=args.keepalive,
                rx_buf=args.rx_buf,
                halfclose=args.halfclose,
            )

        sess = tcp_open(
            bus=bus,
            url=url,
            timeout=args.timeout,
            wait_connected=args.wait_connected,
            info_poll_s=args.info_poll,
        )
        print(f"handle={sess.handle} connected={'yes' if args.wait_connected else 'maybe'}")
    return 0


def cmd_net_tcp_sendrecv(args) -> int:
    data = Path(args.inp).read_bytes() if args.inp else (args.data.encode("utf-8") if args.data else b"")
    out_path = Path(args.out) if args.out else None
    if out_path:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        if out_path.exists() and not args.force:
            print(f"Refusing to overwrite {out_path} (use --force)")
            return 1
        out_path.write_bytes(b"")

    with open_serial(port=args.port, baud=args.baud, timeout_s=args.timeout) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        sess = tcp_open(
            bus=bus,
            url=args.url,
            timeout=args.timeout,
            wait_connected=True,
            info_poll_s=args.info_poll,
        )

        if args.show_info:
            tcp_info_print(bus=bus, handle=sess.handle, timeout=args.timeout, max_headers=args.max_headers)

        # send
        if data:
            tcp_send(bus=bus, sess=sess, data=data, timeout=args.timeout, chunk=args.write_chunk)

        if args.halfclose:
            tcp_halfclose(bus=bus, sess=sess, timeout=args.timeout)

        # receive loop (until eof, or until no data for --idle-timeout)
        idle_deadline = time.monotonic() + max(args.idle_timeout, 0.0)
        total = 0
        while True:
            try:
                chunk, eof = tcp_recv_some(bus=bus, sess=sess, timeout=args.timeout, max_bytes=args.read_chunk)
            except RuntimeError as e:
                # If we've already received some bytes, and the peer disappears abruptly (RST/close),
                # the device may report IOError. For "sendrecv" this should behave like EOF.
                msg = str(e)
                if ("status=5" in msg or "IOError" in msg) and total > 0:
                    eof = True
                    chunk = b""
                else:
                    raise

            if chunk:
                total += len(chunk)
                idle_deadline = time.monotonic() + max(args.idle_timeout, 0.0)
                if out_path:
                    with out_path.open("ab") as f:
                        f.write(chunk)
                else:
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.buffer.flush()

            if eof:
                break

            if args.idle_timeout > 0 and time.monotonic() > idle_deadline:
                break

        if args.verbose:
            print(f"\nread_total={total} bytes read_offset={sess.read_offset} write_offset={sess.write_offset}")

        tcp_close(bus=bus, handle=sess.handle, timeout=args.timeout)

    return 0


def register_tcp_subcommands(nsub) -> None:
    """
    Register under: fujinet net tcp ...
    """
    pt = nsub.add_parser("tcp", help="TCP stream helpers (tcp:// scheme)")
    tsub = pt.add_subparsers(dest="tcp_cmd", required=True)

    pc = tsub.add_parser("connect", help="Open a tcp:// URL and print the handle")
    pc.add_argument("url", nargs="?", help="tcp://host:port[?opts]")
    pc.add_argument("--host", help="Alternative to url: host")
    pc.add_argument("--port", type=int, help="Alternative to url: port")
    pc.add_argument("--connect-timeout-ms", type=int, default=None)
    pc.add_argument("--nodelay", type=int, choices=[0, 1], default=None)
    pc.add_argument("--keepalive", type=int, choices=[0, 1], default=None)
    pc.add_argument("--rx-buf", type=int, default=None)
    pc.add_argument("--halfclose", type=int, choices=[0, 1], default=None)
    pc.add_argument("--wait-connected", action="store_true", help="Poll Info until connected=1")
    pc.add_argument("--info-poll", type=float, default=0.01, help="Sleep between Info polls")
    pc.set_defaults(fn=cmd_net_tcp_connect)

    ps = tsub.add_parser("sendrecv", help="Connect, send bytes, then read response stream")
    ps.add_argument("url", help="tcp://host:port[?opts]")
    src = ps.add_mutually_exclusive_group(required=False)
    src.add_argument("--inp", help="Read bytes to send from file")
    src.add_argument("--data", help="Send these UTF-8 bytes")
    ps.add_argument("--write-chunk", type=int, default=1024)
    ps.add_argument("--read-chunk", type=int, default=512)
    ps.add_argument("--halfclose", action="store_true", help="Send zero-length write to half-close TX")
    ps.add_argument("--idle-timeout", type=float, default=0.25, help="Stop reading if idle for this many seconds (0=never)")
    ps.add_argument("--show-info", action="store_true", help="Print Info() pseudo headers after connect")
    ps.add_argument("--info-poll", type=float, default=0.01)
    ps.add_argument("--out", help="Write received bytes to this file (else stdout)")
    ps.add_argument("--force", action="store_true", help="Overwrite --out if it exists")
    ps.set_defaults(fn=cmd_net_tcp_sendrecv)

    pr = tsub.add_parser("repl", help="Interactive TCP REPL")
    pr.add_argument("url", nargs="?", help="tcp://host:port[?opts] (optional; can also 'open' inside repl)")
    pr.add_argument("--wait-connected", action="store_true", help="Poll Info until connected=1 when opening")
    pr.add_argument("--show-info", action="store_true", help="Print Info() pseudo headers after opening")
    pr.add_argument("--info-poll", type=float, default=0.01)
    pr.add_argument("--write-chunk", type=int, default=1024)
    pr.add_argument("--read-chunk", type=int, default=512)
    pr.add_argument("--idle-timeout", type=float, default=0.25)
    pr.add_argument("--binary", action="store_true", help="For recv/drain, write raw bytes to stdout")
    pr.set_defaults(fn=cmd_net_tcp_repl)