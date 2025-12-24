# py/fujinet_tools/net.py
from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Optional

from .fujibus import FujiBusSession, FujiPacket
from . import netproto as np
from . import net_tcp
from .common import open_serial, status_ok


# ----------------------------------------------------------------------
# Status helpers
# ----------------------------------------------------------------------

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

NET_COMMANDS = {
    1: "Open",
    2: "Read",
    3: "Write",
    4: "Close",
    5: "Info",
}

def _parse_header_kv(s: str) -> tuple[str, str]:
    """
    Accept either:
      - "Key=Value"
      - "Key: Value"
    """
    if ":" in s and (s.find(":") < s.find("=") or "=" not in s):
        k, v = s.split(":", 1)
        return k.strip(), v.lstrip()
    if "=" in s:
        k, v = s.split("=", 1)
        return k.strip(), v
    raise ValueError(f"Invalid header format (expected Key=Value or Key: Value): {s!r}")


def _load_headers_from_file(path: str) -> list[tuple[str, str]]:
    """
    File format: one header per line.
    Blank lines and lines starting with # are ignored.
    Each line is "Key=Value" or "Key: Value".
    """
    out: list[tuple[str, str]] = []
    p = Path(path)
    for raw in p.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        k, v = _parse_header_kv(line)
        if k:
            out.append((k, v))
    return out


def _collect_request_headers(args) -> list[tuple[str, str]]:
    """
    Merge headers from:
      - --set-header (repeatable)
      - --set-header-from-file (repeatable)
    Order is preserved: file headers first, then explicit CLI headers.
    """
    headers: list[tuple[str, str]] = []

    file_list = getattr(args, "set_header_from_file", None) or []
    for fp in file_list:
        headers.extend(_load_headers_from_file(fp))

    kv_list = getattr(args, "set_header", None) or []
    for kv in kv_list:
        k, v = _parse_header_kv(kv)
        if k:
            headers.append((k, v))

    return headers


def _status_str(code: int) -> str:
    return STATUS_TEXT.get(code, f"Unknown({code})")


def _cmd_str(code: int) -> str:
    return NET_COMMANDS.get(code, f"Unknown({code})")


def _pkt_status_code(pkt: Optional[FujiPacket]) -> int:
    if not pkt or not pkt.params:
        return -1
    return int(pkt.params[0])


# ----------------------------------------------------------------------
# Serial + FujiBusSession helpers
# ----------------------------------------------------------------------

def _send_retry_not_ready(
    *,
    bus: FujiBusSession,
    device: int,
    command: int,
    payload: bytes,
    timeout: float,
    retries: int = 200,
    sleep_s: float = 0.01,
) -> Optional[FujiPacket]:
    """
    Send a command and retry on:
      - StatusCode::NotReady (4)
      - pkt == None (no parseable response yet)

    Uses short per-attempt timeouts but enforces an overall wall-clock timeout.
    Applies backoff to avoid tight polling loops.
    """
    deadline = time.monotonic() + max(0.0, timeout)
    attempt = 0
    backoff = max(0.001, sleep_s)
    backoff_max = 0.05  # 50ms cap

    while True:
        attempt += 1
        remaining = max(0.0, deadline - time.monotonic())
        if remaining <= 0.0:
            return None

        per_try_timeout = min(0.05, max(0.005, remaining))
        pkt = bus.send_command_expect(
            device=device,
            command=command,
            payload=payload,
            expect_device=device,
            expect_command=command,
            timeout=per_try_timeout,
            cmd_txt=_cmd_str(command),
        )

        if pkt is None:
            if attempt >= retries or time.monotonic() >= deadline:
                return None
            time.sleep(backoff)
            backoff = min(backoff_max, backoff * 1.5)
            continue

        status = _pkt_status_code(pkt)
        if status != 4:  # NotReady
            return pkt

        if attempt >= retries or time.monotonic() >= deadline:
            return pkt

        time.sleep(backoff)
        backoff = min(backoff_max, backoff * 1.5)


# ----------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------

def cmd_net_open(args) -> int:
    req = np.build_open_req(
        method=args.method,
        flags=args.flags,
        url=args.url,
        headers=_collect_request_headers(args),
        body_len_hint=args.body_len_hint,
        response_headers=getattr(args, "resp_header", None) or [],
    )

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = _send_retry_not_ready(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_OPEN,
            payload=req,
            timeout=args.timeout,
            retries=50,
            sleep_s=0.01,
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        orr = np.parse_open_resp(pkt.payload)
        print(f"accepted={orr.accepted} needs_body_write={orr.needs_body_write} handle={orr.handle}")
        return 0


def cmd_net_close(args) -> int:
    req = np.build_close_req(args.handle)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = _send_retry_not_ready(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_CLOSE,
            payload=req,
            timeout=args.timeout,
            retries=50,
            sleep_s=0.01,
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        print("closed")
        return 0


def cmd_net_info(args) -> int:
    # NOTE: response headers (if any) must have been requested in Open().
    req = np.build_info_req(args.handle)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = _send_retry_not_ready(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_INFO,
            payload=req,
            timeout=args.timeout,
            retries=200,
            sleep_s=0.01,
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        ir = np.parse_info_resp(pkt.payload)
        print(f"handle={ir.handle} http_status={ir.http_status} content_length={ir.content_length}")
        if ir.header_bytes:
            try:
                print(
                    ir.header_bytes.decode("utf-8", errors="replace"),
                    end="" if ir.header_bytes.endswith(b"\n") else "\n",
                )
            except Exception:
                print(repr(ir.header_bytes))
        return 0


def cmd_net_write(args) -> int:
    data = Path(args.inp).read_bytes() if args.inp else (args.data.encode("utf-8") if args.data else b"")
    req = np.build_write_req(args.handle, args.offset, data)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = _send_retry_not_ready(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_WRITE,
            payload=req,
            timeout=args.timeout,
            retries=200,
            sleep_s=0.001,
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        wr = np.parse_write_resp(pkt.payload)
        print(f"handle={wr.handle} offset={wr.offset} written={wr.written}")
        return 0


def cmd_net_read(args) -> int:
    req = np.build_read_req(args.handle, args.offset, args.max_bytes)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = _send_retry_not_ready(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_READ,
            payload=req,
            timeout=args.timeout,
            retries=2000,
            sleep_s=0.005,
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        rr = np.parse_read_resp(pkt.payload)
        if args.out:
            Path(args.out).write_bytes(rr.data)
        else:
            sys.stdout.buffer.write(rr.data)
        if args.verbose:
            print(f"\n(offset={rr.offset} len={len(rr.data)} eof={rr.eof} truncated={rr.truncated})")
        return 0


def cmd_net_get(args) -> int:
    """
    Convenience wrapper:
      Open(GET) -> (optional Info) -> Read until EOF -> Close
    Uses ONE serial connection + ONE FujiBusSession for the whole sequence.
    """
    out_path = Path(args.out) if args.out else None
    if out_path:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        if out_path.exists() and not args.force:
            print(f"Refusing to overwrite {out_path} (use --force)")
            return 1
        out_path.write_bytes(b"")

    # If the user wants to show headers, request a default allowlist unless overridden.
    resp_headers = getattr(args, "resp_header", None) or []
    if args.show_headers and not resp_headers:
        resp_headers = ["Server", "Content-Type", "Content-Length", "Location", "ETag", "Last-Modified"]

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        # OPEN
        open_req = np.build_open_req(
            method=1,  # GET
            flags=args.flags,
            url=args.url,
            headers=_collect_request_headers(args),
            body_len_hint=0,
            response_headers=resp_headers,
        )
        pkt = _send_retry_not_ready(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_OPEN,
            payload=open_req,
            timeout=args.timeout,
            retries=50,
            sleep_s=0.01,
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        orr = np.parse_open_resp(pkt.payload)
        if not orr.accepted:
            print("Open not accepted")
            return 1
        handle = orr.handle

        # INFO (optional)
        if args.show_headers:
            for _ in range(max(1, args.info_retries)):
                info_req = np.build_info_req(handle)
                ipkt = _send_retry_not_ready(
                    bus=bus,
                    device=np.NETWORK_DEVICE_ID,
                    command=np.CMD_INFO,
                    payload=info_req,
                    timeout=args.timeout,
                    retries=200,
                    sleep_s=args.info_sleep,
                )
                if ipkt is None:
                    break
                if not status_ok(ipkt):
                    code = _pkt_status_code(ipkt)
                    if code != 4:
                        print(f"Device status={code} ({_status_str(code)})")
                        break
                    time.sleep(args.info_sleep)
                    continue

                ir = np.parse_info_resp(ipkt.payload)
                print(f"http_status={ir.http_status} content_length={ir.content_length}")
                if ir.header_bytes:
                    print(ir.header_bytes.decode("utf-8", errors="replace"), end="")
                break

        # READ loop (robust against NotReady / short stalls)
        offset = 0
        total = 0
        idle_deadline = time.monotonic() + max(0.25, float(getattr(args, "idle_timeout", 0.0) or 0.0)) if hasattr(args, "idle_timeout") else float("inf")

        while True:
            read_req = np.build_read_req(handle, offset, args.chunk)
            rpkt = _send_retry_not_ready(
                bus=bus,
                device=np.NETWORK_DEVICE_ID,
                command=np.CMD_READ,
                payload=read_req,
                timeout=args.timeout,
                retries=5000,
                sleep_s=0.005,
            )
            if rpkt is None:
                print("No response")
                return 2
            if not status_ok(rpkt):
                code = _pkt_status_code(rpkt)
                print(f"Device status={code} ({_status_str(code)})")
                return 1

            rr = np.parse_read_resp(rpkt.payload)
            if rr.offset != offset:
                print(f"Offset echo mismatch: expected {offset}, got {rr.offset}")
                return 1

            n = len(rr.data)
            if n:
                idle_deadline = time.monotonic() + 0.25  # progress resets idle
                if out_path:
                    with out_path.open("ab") as f:
                        f.write(rr.data)
                else:
                    sys.stdout.buffer.write(rr.data)
                    sys.stdout.buffer.flush()

                total += n
                offset += n

            if args.verbose:
                print(f"read: offset={rr.offset} len={n} eof={rr.eof} truncated={rr.truncated}")

            if rr.eof:
                break

            if n == 0:
                if time.monotonic() > idle_deadline:
                    break
                time.sleep(0.02)


        # CLOSE best-effort
        close_req = np.build_close_req(handle)
        _send_retry_not_ready(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_CLOSE,
            payload=close_req,
            timeout=args.timeout,
            retries=50,
            sleep_s=0.01,
        )

        if args.verbose:
            print(f"total read: {total} bytes")
        return 0


def cmd_net_head(args) -> int:
    # HEAD: Open(HEAD) -> Info -> Close (single bus)
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        open_req = np.build_open_req(
            method=5,  # HEAD
            flags=args.flags,
            url=args.url,
            headers=_collect_request_headers(args),
            body_len_hint=0,
            response_headers=getattr(args, "resp_header", None) or [],
        )
        pkt = _send_retry_not_ready(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_OPEN,
            payload=open_req,
            timeout=args.timeout,
            retries=50,
            sleep_s=0.01,
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        orr = np.parse_open_resp(pkt.payload)
        handle = orr.handle

        info_req = np.build_info_req(handle)
        ipkt = _send_retry_not_ready(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_INFO,
            payload=info_req,
            timeout=args.timeout,
            retries=300,
            sleep_s=0.01,
        )
        if ipkt is None:
            print("No response")
            return 2
        if not status_ok(ipkt):
            code = _pkt_status_code(ipkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        ir = np.parse_info_resp(ipkt.payload)
        print(f"http_status={ir.http_status} content_length={ir.content_length}")
        if ir.header_bytes:
            print(ir.header_bytes.decode("utf-8", errors="replace"), end="")

        close_req = np.build_close_req(handle)
        _send_retry_not_ready(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_CLOSE,
            payload=close_req,
            timeout=args.timeout,
            retries=50,
            sleep_s=0.01,
        )
        return 0


def cmd_net_send(args) -> int:
    """
    Open -> optional Write body -> optional Info -> optional Read response -> Close
    Uses ONE serial connection + ONE FujiBusSession for the whole sequence.
    """
    data = Path(args.inp).read_bytes() if args.inp else (args.data.encode("utf-8") if args.data else b"")
    body_len_hint = len(data) if args.body_len_hint < 0 else args.body_len_hint

    resp_headers = getattr(args, "resp_header", None) or []
    if args.show_headers and not resp_headers:
        resp_headers = ["Server", "Content-Type", "Content-Length", "Location", "ETag", "Last-Modified"]

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        open_req = np.build_open_req(
            method=args.method,
            flags=args.flags,
            url=args.url,
            headers=_collect_request_headers(args),
            body_len_hint=body_len_hint if (args.method in (2, 3)) else 0,
            response_headers=resp_headers,
        )

        # OPEN
        pkt = _send_retry_not_ready(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_OPEN,
            payload=open_req,
            timeout=args.timeout,
            retries=50,
            sleep_s=0.01,
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        orr = np.parse_open_resp(pkt.payload)
        if not orr.accepted:
            print("Open not accepted")
            return 1

        handle = orr.handle

        # Optional body upload (POST/PUT only)
        if args.method in (2, 3) and body_len_hint > 0:
            if not orr.needs_body_write:
                print("Device did not request body write, but body_len_hint > 0 was provided")
                return 1

            offset = 0
            while offset < len(data):
                chunk = data[offset : offset + args.write_chunk]
                wreq = np.build_write_req(handle, offset, chunk)
                wpkt = _send_retry_not_ready(
                    bus=bus,
                    device=np.NETWORK_DEVICE_ID,
                    command=np.CMD_WRITE,
                    payload=wreq,
                    timeout=args.timeout,
                    retries=2000,
                    sleep_s=0.001,
                )
                if wpkt is None:
                    print("No response")
                    return 2
                if not status_ok(wpkt):
                    code = _pkt_status_code(wpkt)
                    print(f"Device status={code} ({_status_str(code)})")
                    return 1

                wr = np.parse_write_resp(wpkt.payload)
                if wr.written == 0:
                    print("Write returned 0 bytes written; aborting")
                    return 1
                offset += wr.written

        # Optional INFO
        if args.show_headers:
            info_req = np.build_info_req(handle)
            ipkt = _send_retry_not_ready(
                bus=bus,
                device=np.NETWORK_DEVICE_ID,
                command=np.CMD_INFO,
                payload=info_req,
                timeout=args.timeout,
                retries=args.info_retries,
                sleep_s=args.info_sleep,
            )
            if ipkt and status_ok(ipkt):
                ir = np.parse_info_resp(ipkt.payload)
                print(f"http_status={ir.http_status} content_length={ir.content_length}")
                if ir.header_bytes:
                    print(ir.header_bytes.decode("utf-8", errors="replace"), end="")

        # Optionally stream response body
        if args.read_response:
            out_path = Path(args.out) if args.out else None
            if out_path:
                out_path.parent.mkdir(parents=True, exist_ok=True)
                if out_path.exists() and not args.force:
                    print(f"Refusing to overwrite {out_path} (use --force)")
                    return 1
                out_path.write_bytes(b"")

            offset = 0
            total = 0
            while True:
                rreq = np.build_read_req(handle, offset, args.chunk)
                rpkt = _send_retry_not_ready(
                    bus=bus,
                    device=np.NETWORK_DEVICE_ID,
                    command=np.CMD_READ,
                    payload=rreq,
                    timeout=args.timeout,
                    retries=20000,
                    sleep_s=0.005,
                )
                if rpkt is None:
                    print("No response")
                    return 2
                if not status_ok(rpkt):
                    code = _pkt_status_code(rpkt)
                    print(f"Device status={code} ({_status_str(code)})")
                    return 1

                rr = np.parse_read_resp(rpkt.payload)
                if rr.offset != offset:
                    print(f"Offset echo mismatch: expected {offset}, got {rr.offset}")
                    return 1

                if out_path:
                    with out_path.open("ab") as f:
                        f.write(rr.data)
                else:
                    sys.stdout.buffer.write(rr.data)

                n = len(rr.data)
                total += n
                offset += n

                if args.verbose:
                    print(f"read: offset={rr.offset} len={n} eof={rr.eof} truncated={rr.truncated}")

                if rr.eof:
                    break
                if n == 0:
                    time.sleep(0.01)

            if args.verbose:
                print(f"total read: {total} bytes")

        # CLOSE (best-effort)
        close_req = np.build_close_req(handle)
        _send_retry_not_ready(
            bus=bus,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_CLOSE,
            payload=close_req,
            timeout=args.timeout,
            retries=50,
            sleep_s=0.01,
        )
        return 0


def register_subcommands(subparsers) -> None:
    pn = subparsers.add_parser("net", help="Network device commands (binary protocol)")
    nsub = pn.add_subparsers(dest="net_cmd", required=True)

    pno = nsub.add_parser("open", help="Open a URL and return a handle")
    pno.add_argument("--method", type=int, default=1, help="1=GET,2=POST,3=PUT,4=DELETE,5=HEAD")
    pno.add_argument("--flags", type=int, default=0, help="bit0=tls, bit1=follow_redirects")
    pno.add_argument("--body-len-hint", type=int, default=0, help="Expected request body length (POST/PUT)")
    pno.add_argument("--resp-header", action="append", default=[], help="Response header name to capture (repeatable)")
    pno.add_argument("--set-header", action="append", default=[],
                help="Request header to send (repeatable). Format: Key=Value or Key: Value")
    pno.add_argument("--set-header-from-file", action="append", default=[],
                help="Read request headers from file (repeatable). One per line: Key=Value or Key: Value")
    pno.add_argument("url")
    pno.set_defaults(fn=cmd_net_open)

    pni = nsub.add_parser("info", help="Fetch response metadata and headers")
    pni.add_argument("handle", type=int)
    pni.set_defaults(fn=cmd_net_info)

    pnr = nsub.add_parser("read", help="Read a single response chunk")
    pnr.add_argument("--offset", type=int, default=0)
    pnr.add_argument("--max-bytes", type=int, default=512)
    pnr.add_argument("--out", help="Write chunk to this file (else stdout)")
    pnr.add_argument("handle", type=int)
    pnr.set_defaults(fn=cmd_net_read)

    pnw = nsub.add_parser("write", help="Write request body bytes (single chunk)")
    pnw.add_argument("--offset", type=int, default=0)
    src = pnw.add_mutually_exclusive_group(required=True)
    src.add_argument("--inp", help="Read bytes from this file")
    src.add_argument("--data", help="Send these UTF-8 bytes")
    pnw.add_argument("handle", type=int)
    pnw.set_defaults(fn=cmd_net_write)

    pnc = nsub.add_parser("close", help="Close a handle")
    pnc.add_argument("handle", type=int)
    pnc.set_defaults(fn=cmd_net_close)

    png = nsub.add_parser("get", help="GET a URL and stream body to stdout or --out")
    png.add_argument("--flags", type=int, default=0)
    png.add_argument("--chunk", type=int, default=512)
    png.add_argument("--out", help="Write to this file (else stdout)")
    png.add_argument("--force", action="store_true", help="Overwrite --out if it exists")
    png.add_argument("--show-headers", action="store_true")
    png.add_argument("--resp-header", action="append", default=[], help="Response header name to capture (repeatable)")
    png.add_argument("--info-retries", type=int, default=10)
    png.add_argument("--info-sleep", type=float, default=0.1)
    png.add_argument("--set-header", action="append", default=[],
                help="Request header to send (repeatable). Format: Key=Value or Key: Value")
    png.add_argument("--set-header-from-file", action="append", default=[],
                help="Read request headers from file (repeatable). One per line: Key=Value or Key: Value")
    png.add_argument("url")
    png.set_defaults(fn=cmd_net_get)

    pnh = nsub.add_parser("head", help="HEAD a URL and print metadata/headers")
    pnh.add_argument("--flags", type=int, default=0)
    pnh.add_argument("--resp-header", action="append", default=[], help="Response header name to capture (repeatable)")
    pnh.add_argument("--set-header", action="append", default=[],
                help="Request header to send (repeatable). Format: Key=Value or Key: Value")
    pnh.add_argument("--set-header-from-file", action="append", default=[],
                help="Read request headers from file (repeatable). One per line: Key=Value or Key: Value")
    pnh.add_argument("url")
    pnh.set_defaults(fn=cmd_net_head)

    pns = nsub.add_parser("send", help="Send a request (GET/POST/PUT/DELETE/HEAD) with optional body + response streaming")
    pns.add_argument("--method", type=int, required=True, help="1=GET,2=POST,3=PUT,4=DELETE,5=HEAD")
    pns.add_argument("--flags", type=int, default=0, help="bit0=tls, bit1=follow_redirects")
    pns.add_argument("--show-headers", action="store_true")
    pns.add_argument("--resp-header", action="append", default=[], help="Response header name to capture (repeatable)")
    pns.add_argument("--info-retries", type=int, default=50)
    pns.add_argument("--info-sleep", type=float, default=0.005)
    pns.add_argument("--read-response", action="store_true", help="Read and stream response body after request completes")
    pns.add_argument("--chunk", type=int, default=512, help="Read chunk size")
    pns.add_argument("--out", help="Write response body to this file (else stdout)")
    pns.add_argument("--force", action="store_true", help="Overwrite --out if it exists")
    pns.add_argument("--body-len-hint", type=int, default=-1, help="POST/PUT only: -1=use len(data), else fixed expected length")
    pns.add_argument("--write-chunk", type=int, default=1024, help="Write chunk size for request body upload")
    src = pns.add_mutually_exclusive_group(required=False)
    src.add_argument("--inp", help="Read request body bytes from this file")
    src.add_argument("--data", help="Send these UTF-8 bytes as request body")
    pns.add_argument("--set-header", action="append", default=[],
                help="Request header to send (repeatable). Format: Key=Value or Key: Value")
    pns.add_argument("--set-header-from-file", action="append", default=[],
                help="Read request headers from file (repeatable). One per line: Key=Value or Key: Value")
    pns.add_argument("url")
    pns.set_defaults(fn=cmd_net_send)

    net_tcp.register_tcp_subcommands(nsub)
