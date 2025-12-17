# py/fujinet_tools/net.py
from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Optional, Union

from .fujibus import send_command
from . import netproto as np

try:
    import serial  # type: ignore
except Exception:
    serial = None  # pyright: ignore


# ----------------------------------------------------------------------
# Status helpers
# ----------------------------------------------------------------------

def _status_ok(pkt) -> bool:
    return bool(pkt.params) and pkt.params[0] == 0


# You said you already have this in your tree; keep it if so.
# Fallback included so this file is standalone.
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


# ----------------------------------------------------------------------
# Low-level send wrappers
# ----------------------------------------------------------------------

def _open_serial(port: str, baud: int, timeout_s: float):
    if serial is None:
        raise RuntimeError("pyserial not available, cannot open persistent serial port")
    # Small per-read timeout; we do our own overall deadline retries.
    return serial.Serial(port=port, baudrate=baud, timeout=timeout_s, write_timeout=timeout_s)


def _send_retry_not_ready(
    *,
    port: Union[str, object],
    device: int,
    command: int,
    payload: bytes,
    baud: int,
    timeout: float,
    read_max: int,
    debug: bool,
    retries: int = 200,
    sleep_s: float = 0.01,
):
    """
    Send a command; if device returns NotReady (status=4), retry quickly
    up to (retries * sleep_s) wall time or until timeout expires.
    """
    deadline = time.monotonic() + max(timeout, 0.0)

    attempt = 0
    while True:
        attempt += 1
        pkt = send_command(
            port=port,
            device=device,
            command=command,
            payload=payload,
            baud=baud,
            timeout=timeout,
            read_max=read_max,
            debug=debug,
        )
        if pkt is None:
            return None

        st = _pkt_status_code(pkt)
        if st != 4:
            return pkt

        # NotReady -> retry
        if attempt >= retries or time.monotonic() >= deadline:
            return pkt

        time.sleep(sleep_s)


def _send(
    *,
    port: Union[str, object],
    device: int,
    command: int,
    payload: bytes,
    baud: int,
    timeout: float,
    read_max: int,
    debug: bool,
):
    return send_command(
        port=port,
        device=device,
        command=command,
        payload=payload,
        baud=baud,
        timeout=timeout,
        read_max=read_max,
        debug=debug,
    )


# ----------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------

def cmd_net_open(args) -> int:
    req = np.build_open_req(
        method=args.method,
        flags=args.flags,
        url=args.url,
        headers=[],
        body_len_hint=args.body_len_hint,
    )

    pkt = _send_retry_not_ready(
        port=args.port,
        device=np.NETWORK_DEVICE_ID,
        command=np.CMD_OPEN,
        payload=req,
        baud=args.baud,
        timeout=args.timeout,
        read_max=args.read_max,
        debug=args.debug,
        retries=50,
        sleep_s=0.01,
    )
    if pkt is None:
        print("No response")
        return 2

    if not _status_ok(pkt):
        code = _pkt_status_code(pkt)
        print(f"Device status={code} ({_status_str(code)})")
        return 1

    orr = np.parse_open_resp(pkt.payload)
    print(f"accepted={orr.accepted} needs_body_write={orr.needs_body_write} handle={orr.handle}")
    return 0


def cmd_net_close(args) -> int:
    req = np.build_close_req(args.handle)

    pkt = _send_retry_not_ready(
        port=args.port,
        device=np.NETWORK_DEVICE_ID,
        command=np.CMD_CLOSE,
        payload=req,
        baud=args.baud,
        timeout=args.timeout,
        read_max=args.read_max,
        debug=args.debug,
        retries=50,
        sleep_s=0.01,
    )
    if pkt is None:
        print("No response")
        return 2

    if not _status_ok(pkt):
        code = _pkt_status_code(pkt)
        print(f"Device status={code} ({_status_str(code)})")
        return 1

    print("closed")
    return 0


def cmd_net_info(args) -> int:
    req = np.build_info_req(args.handle, args.max_headers)

    pkt = _send_retry_not_ready(
        port=args.port,
        device=np.NETWORK_DEVICE_ID,
        command=np.CMD_INFO,
        payload=req,
        baud=args.baud,
        timeout=args.timeout,
        read_max=args.read_max,
        debug=args.debug,
        retries=200,
        sleep_s=0.01,
    )
    if pkt is None:
        print("No response")
        return 2

    if not _status_ok(pkt):
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

    pkt = _send_retry_not_ready(
        port=args.port,
        device=np.NETWORK_DEVICE_ID,
        command=np.CMD_WRITE,
        payload=req,
        baud=args.baud,
        timeout=args.timeout,
        read_max=args.read_max,
        debug=args.debug,
        retries=50,
        sleep_s=0.01,
    )
    if pkt is None:
        print("No response")
        return 2

    if not _status_ok(pkt):
        code = _pkt_status_code(pkt)
        print(f"Device status={code} ({_status_str(code)})")
        return 1

    wr = np.parse_write_resp(pkt.payload)
    print(f"handle={wr.handle} offset={wr.offset} written={wr.written}")
    return 0


def cmd_net_read(args) -> int:
    req = np.build_read_req(args.handle, args.offset, args.max_bytes)

    pkt = _send_retry_not_ready(
        port=args.port,
        device=np.NETWORK_DEVICE_ID,
        command=np.CMD_READ,
        payload=req,
        baud=args.baud,
        timeout=args.timeout,
        read_max=args.read_max,
        debug=args.debug,
        retries=500,
        sleep_s=0.005,
    )
    if pkt is None:
        print("No response")
        return 2

    if not _status_ok(pkt):
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
    Convenience wrapper: Open(GET) -> (Info) -> Read until EOF -> Close

    Uses ONE serial connection for the whole sequence to avoid per-command open/close latency.
    Also retries NotReady aggressively with tiny sleeps.
    """
    out_path = Path(args.out) if args.out else None
    if out_path:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        if out_path.exists() and not args.force:
            print(f"Refusing to overwrite {out_path} (use --force)")
            return 1
        out_path.write_bytes(b"")

    ser = None
    port_obj: Union[str, object] = args.port
    if isinstance(args.port, str) and serial is not None:
        # Use a short serial timeout; our retry loop handles overall time.
        ser = _open_serial(args.port, args.baud, timeout_s=min(0.05, max(0.001, args.timeout)))
        port_obj = ser

    try:
        # OPEN
        open_req = np.build_open_req(
            method=1,  # GET
            flags=args.flags,
            url=args.url,
            headers=[],
            body_len_hint=0,
        )
        pkt = _send_retry_not_ready(
            port=port_obj,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_OPEN,
            payload=open_req,
            baud=args.baud,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
            retries=50,
            sleep_s=0.01,
        )
        if pkt is None:
            print("No response")
            return 2
        if not _status_ok(pkt):
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
                info_req = np.build_info_req(handle, args.max_headers)
                ipkt = _send_retry_not_ready(
                    port=port_obj,
                    device=np.NETWORK_DEVICE_ID,
                    command=np.CMD_INFO,
                    payload=info_req,
                    baud=args.baud,
                    timeout=args.timeout,
                    read_max=args.read_max,
                    debug=args.debug,
                    retries=200,
                    sleep_s=args.info_sleep,
                )
                if ipkt is None:
                    break
                if not _status_ok(ipkt):
                    # Non-ready or other error; let retry loop handle NotReady.
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

        # READ loop
        offset = 0
        total = 0
        while True:
            read_req = np.build_read_req(handle, offset, args.chunk)
            rpkt = _send_retry_not_ready(
                port=port_obj,
                device=np.NETWORK_DEVICE_ID,
                command=np.CMD_READ,
                payload=read_req,
                baud=args.baud,
                timeout=args.timeout,
                read_max=args.read_max,
                debug=args.debug,
                retries=1000,
                sleep_s=0.005,
            )
            if rpkt is None:
                print("No response")
                return 2
            if not _status_ok(rpkt):
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
                # defensive: if backend ever sends 0 bytes without eof, avoid a tight infinite loop
                time.sleep(0.005)

        # CLOSE best-effort
        close_req = np.build_close_req(handle)
        _send_retry_not_ready(
            port=port_obj,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_CLOSE,
            payload=close_req,
            baud=args.baud,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
            retries=50,
            sleep_s=0.01,
        )

        if args.verbose:
            print(f"total read: {total} bytes")
        return 0

    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass


def cmd_net_head(args) -> int:
    # HEAD: Open(HEAD) -> Info -> Close (single serial session)
    ser = None
    port_obj: Union[str, object] = args.port
    if isinstance(args.port, str) and serial is not None:
        ser = _open_serial(args.port, args.baud, timeout_s=min(0.05, max(0.001, args.timeout)))
        port_obj = ser

    try:
        open_req = np.build_open_req(
            method=5,  # HEAD
            flags=args.flags,
            url=args.url,
            headers=[],
            body_len_hint=0,
        )
        pkt = _send_retry_not_ready(
            port=port_obj,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_OPEN,
            payload=open_req,
            baud=args.baud,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
            retries=50,
            sleep_s=0.01,
        )
        if pkt is None:
            print("No response")
            return 2
        if not _status_ok(pkt):
            code = _pkt_status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        orr = np.parse_open_resp(pkt.payload)
        handle = orr.handle

        info_req = np.build_info_req(handle, args.max_headers)
        ipkt = _send_retry_not_ready(
            port=port_obj,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_INFO,
            payload=info_req,
            baud=args.baud,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
            retries=300,
            sleep_s=0.01,
        )
        if ipkt is None:
            print("No response")
            return 2
        if not _status_ok(ipkt):
            code = _pkt_status_code(ipkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        ir = np.parse_info_resp(ipkt.payload)
        print(f"http_status={ir.http_status} content_length={ir.content_length}")
        if ir.header_bytes:
            print(ir.header_bytes.decode("utf-8", errors="replace"), end="")

        close_req = np.build_close_req(handle)
        _send_retry_not_ready(
            port=port_obj,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_CLOSE,
            payload=close_req,
            baud=args.baud,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
            retries=50,
            sleep_s=0.01,
        )
        return 0

    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass

def cmd_net_send(args) -> int:
    data = Path(args.inp).read_bytes() if args.inp else (args.data.encode("utf-8") if args.data else b"")
    body_len_hint = len(data) if args.body_len_hint < 0 else args.body_len_hint

    open_req = np.build_open_req(
        method=args.method,
        flags=args.flags,
        url=args.url,
        headers=[],
        body_len_hint=body_len_hint if (args.method in (2, 3)) else 0,
    )

    with serial.Serial(args.port, args.baud, timeout=0.01) as ser:
        # OPEN
        pkt = send_command(
            port=ser, device=np.NETWORK_DEVICE_ID, command=np.CMD_OPEN, payload=open_req,
            timeout=args.timeout, read_max=args.read_max, debug=args.debug,
        )
        if pkt is None:
            print("No response")
            return 2
        if not _status_ok(pkt):
            code = _status_code(pkt)
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
                chunk = data[offset: offset + args.write_chunk]
                wreq = np.build_write_req(handle, offset, chunk)
                wpkt = _send_retry_not_ready(
                    ser, np.NETWORK_DEVICE_ID, np.CMD_WRITE, wreq,
                    timeout=args.timeout, read_max=args.read_max, debug=args.debug,
                    retries=200, sleep_s=0.001,
                )
                if wpkt is None:
                    print("No response")
                    return 2
                if not _status_ok(wpkt):
                    code = _status_code(wpkt)
                    print(f"Device status={code} ({_status_str(code)})")
                    return 1
                wr = np.parse_write_resp(wpkt.payload)
                if wr.written == 0:
                    print("Write returned 0 bytes written; aborting")
                    return 1
                offset += wr.written

        # Optional INFO
        if args.show_headers:
            info_req = np.build_info_req(handle, args.max_headers)
            ipkt = _send_retry_not_ready(
                ser, np.NETWORK_DEVICE_ID, np.CMD_INFO, info_req,
                timeout=args.timeout, read_max=args.read_max, debug=args.debug,
                retries=args.info_retries, sleep_s=args.info_sleep,
            )
            if ipkt and _status_ok(ipkt):
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
                    ser, np.NETWORK_DEVICE_ID, np.CMD_READ, rreq,
                    timeout=args.timeout, read_max=args.read_max, debug=args.debug,
                    retries=500, sleep_s=0.001,
                )
                if rpkt is None:
                    print("No response")
                    return 2
                if not _status_ok(rpkt):
                    code = _status_code(rpkt)
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
                    import sys
                    sys.stdout.buffer.write(rr.data)

                n = len(rr.data)
                total += n
                offset += n

                if args.verbose:
                    print(f"read: offset={rr.offset} len={n} eof={rr.eof} truncated={rr.truncated}")

                if rr.eof or n == 0:
                    break

            if args.verbose:
                print(f"total read: {total} bytes")

        # CLOSE (best-effort)
        close_req = np.build_close_req(handle)
        send_command(
            port=ser, device=np.NETWORK_DEVICE_ID, command=np.CMD_CLOSE, payload=close_req,
            timeout=args.timeout, read_max=args.read_max, debug=args.debug,
        )
        return 0


def register_subcommands(subparsers) -> None:
    pn = subparsers.add_parser("net", help="Network device commands (binary protocol)")
    nsub = pn.add_subparsers(dest="net_cmd", required=True)

    pno = nsub.add_parser("open", help="Open a URL and return a handle")
    pno.add_argument("--method", type=int, default=1, help="1=GET,2=POST,3=PUT,4=DELETE,5=HEAD")
    pno.add_argument("--flags", type=int, default=0, help="bit0=tls, bit1=follow_redirects, bit2=want_headers")
    pno.add_argument("--body-len-hint", type=int, default=0, help="Expected request body length (POST/PUT)")
    pno.add_argument("url")
    pno.set_defaults(fn=cmd_net_open)

    pni = nsub.add_parser("info", help="Fetch response metadata and headers")
    pni.add_argument("--max-headers", type=int, default=1024)
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
    png.add_argument("--max-headers", type=int, default=1024)
    png.add_argument("--info-retries", type=int, default=10)
    png.add_argument("--info-sleep", type=float, default=0.1)
    png.add_argument("url")
    png.set_defaults(fn=cmd_net_get)

    pnh = nsub.add_parser("head", help="HEAD a URL and print metadata/headers")
    pnh.add_argument("--flags", type=int, default=0)
    pnh.add_argument("--max-headers", type=int, default=2048)
    pnh.add_argument("url")
    pnh.set_defaults(fn=cmd_net_head)

    pns = nsub.add_parser("send", help="Send a request (GET/POST/PUT/DELETE/HEAD) with optional body + response streaming")
    pns.add_argument("--method", type=int, required=True, help="1=GET,2=POST,3=PUT,4=DELETE,5=HEAD")
    pns.add_argument("--flags", type=int, default=0, help="bit0=tls, bit1=follow_redirects, bit2=want_headers")
    pns.add_argument("--show-headers", action="store_true")
    pns.add_argument("--max-headers", type=int, default=2048)
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
    pns.add_argument("url")
    pns.set_defaults(fn=cmd_net_send)
