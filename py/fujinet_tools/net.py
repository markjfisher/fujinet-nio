# ✅ Replace file: py/fujinet_tools/net.py
# Goals:
# - Open ONE serial connection per command (net get/head/open/info/read/write/close)
# - Always pass read_max through
# - Retry NotReady with short sleep WITHOUT paying serial timeout cost each time
# - Add status text mapping

from __future__ import annotations

import time
from pathlib import Path
import serial

from .fujibus import send_command
from . import netproto as np


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


def _status_ok(pkt) -> bool:
    return bool(pkt.params) and pkt.params[0] == 0


def _status_code(pkt) -> int:
    if not pkt or not pkt.params:
        return 255
    return int(pkt.params[0])


def _status_str(code: int) -> str:
    return STATUS_TEXT.get(code, f"Unknown({code})")


def _send_retry_not_ready(
    ser: serial.Serial,
    device: int,
    command: int,
    payload: bytes,
    *,
    timeout: float,
    read_max: int,
    debug: bool,
    retries: int = 20,
    sleep_s: float = 0.005,
):
    """
    Send command; if device returns NotReady, retry quickly.
    This avoids paying a large serial timeout per attempt.
    """
    for _ in range(retries):
        pkt = send_command(
            port=ser,
            device=device,
            command=command,
            payload=payload,
            timeout=timeout,
            read_max=read_max,
            debug=debug,
        )
        if pkt is None:
            return None
        code = _status_code(pkt)
        if code == 4:  # NotReady
            time.sleep(sleep_s)
            continue
        return pkt
    return pkt  # last one


def cmd_net_open(args) -> int:
    req = np.build_open_req(
        method=args.method,
        flags=args.flags,
        url=args.url,
        headers=[],
        body_len_hint=0,
    )

    with serial.Serial(args.port, args.baud, timeout=0.01) as ser:
        pkt = send_command(
            port=ser,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_OPEN,
            payload=req,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
        )

    if pkt is None:
        print("No response")
        return 2
    if not _status_ok(pkt):
        code = _status_code(pkt)
        print(f"Device status={code} ({_status_str(code)})")
        return 1

    orr = np.parse_open_resp(pkt.payload)
    print(f"accepted={orr.accepted} needs_body_write={orr.needs_body_write} handle={orr.handle}")
    return 0


def cmd_net_close(args) -> int:
    req = np.build_close_req(args.handle)

    with serial.Serial(args.port, args.baud, timeout=0.01) as ser:
        pkt = send_command(
            port=ser,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_CLOSE,
            payload=req,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
        )

    if pkt is None:
        print("No response")
        return 2
    if not _status_ok(pkt):
        code = _status_code(pkt)
        print(f"Device status={code} ({_status_str(code)})")
        return 1

    print("closed")
    return 0


def cmd_net_info(args) -> int:
    req = np.build_info_req(args.handle, args.max_headers)

    with serial.Serial(args.port, args.baud, timeout=0.01) as ser:
        pkt = _send_retry_not_ready(
            ser,
            np.NETWORK_DEVICE_ID,
            np.CMD_INFO,
            req,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
            retries=50,
            sleep_s=0.005,
        )

    if pkt is None:
        print("No response")
        return 2
    if not _status_ok(pkt):
        code = _status_code(pkt)
        print(f"Device status={code} ({_status_str(code)})")
        return 1

    ir = np.parse_info_resp(pkt.payload)
    print(f"handle={ir.handle} http_status={ir.http_status} content_length={ir.content_length}")
    if ir.header_bytes:
        print(ir.header_bytes.decode("utf-8", errors="replace"), end="" if ir.header_bytes.endswith(b"\n") else "\n")
    return 0


def cmd_net_write(args) -> int:
    data = Path(args.inp).read_bytes() if args.inp else (args.data.encode("utf-8") if args.data else b"")
    req = np.build_write_req(args.handle, args.offset, data)

    with serial.Serial(args.port, args.baud, timeout=0.01) as ser:
        pkt = _send_retry_not_ready(
            ser,
            np.NETWORK_DEVICE_ID,
            np.CMD_WRITE,
            req,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
            retries=50,
            sleep_s=0.002,
        )

    if pkt is None:
        print("No response")
        return 2
    if not _status_ok(pkt):
        code = _status_code(pkt)
        print(f"Device status={code} ({_status_str(code)})")
        return 1

    wr = np.parse_write_resp(pkt.payload)
    print(f"handle={wr.handle} offset={wr.offset} written={wr.written}")
    return 0


def cmd_net_read(args) -> int:
    req = np.build_read_req(args.handle, args.offset, args.max_bytes)

    with serial.Serial(args.port, args.baud, timeout=0.01) as ser:
        pkt = _send_retry_not_ready(
            ser,
            np.NETWORK_DEVICE_ID,
            np.CMD_READ,
            req,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
            retries=100,
            sleep_s=0.001,
        )

    if pkt is None:
        print("No response")
        return 2
    if not _status_ok(pkt):
        code = _status_code(pkt)
        print(f"Device status={code} ({_status_str(code)})")
        return 1

    rr = np.parse_read_resp(pkt.payload)
    if args.out:
        Path(args.out).write_bytes(rr.data)
    else:
        import sys
        sys.stdout.buffer.write(rr.data)

    if args.verbose:
        print(f"\n(offset={rr.offset} len={len(rr.data)} eof={rr.eof} truncated={rr.truncated})")
    return 0


def cmd_net_get(args) -> int:
    open_req = np.build_open_req(
        method=1,  # GET
        flags=args.flags,
        url=args.url,
        headers=[],
        body_len_hint=0,
    )

    out_path = Path(args.out) if args.out else None
    if out_path:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        if out_path.exists() and not args.force:
            print(f"Refusing to overwrite {out_path} (use --force)")
            return 1
        out_path.write_bytes(b"")

    with serial.Serial(args.port, args.baud, timeout=0.01) as ser:
        # OPEN
        pkt = send_command(
            port=ser,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_OPEN,
            payload=open_req,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
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

        # Optional INFO
        if args.show_headers:
            info_req = np.build_info_req(handle, args.max_headers)
            ipkt = _send_retry_not_ready(
                ser,
                np.NETWORK_DEVICE_ID,
                np.CMD_INFO,
                info_req,
                timeout=args.timeout,
                read_max=args.read_max,
                debug=args.debug,
                retries=args.info_retries,
                sleep_s=args.info_sleep,
            )
            if ipkt and _status_ok(ipkt):
                ir = np.parse_info_resp(ipkt.payload)
                print(f"http_status={ir.http_status} content_length={ir.content_length}")
                if ir.header_bytes:
                    print(ir.header_bytes.decode("utf-8", errors="replace"), end="")

        # READ loop
        offset = 0
        total = 0
        while True:
            read_req = np.build_read_req(handle, offset, args.chunk)
            rpkt = _send_retry_not_ready(
                ser,
                np.NETWORK_DEVICE_ID,
                np.CMD_READ,
                read_req,
                timeout=args.timeout,
                read_max=args.read_max,
                debug=args.debug,
                retries=200,
                sleep_s=0.001,
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

        # CLOSE (best-effort)
        close_req = np.build_close_req(handle)
        send_command(
            port=ser,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_CLOSE,
            payload=close_req,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
        )

    if args.verbose:
        print(f"total read: {total} bytes")
    return 0


def cmd_net_head(args) -> int:
    open_req = np.build_open_req(
        method=5,  # HEAD
        flags=args.flags,
        url=args.url,
        headers=[],
        body_len_hint=0,
    )

    with serial.Serial(args.port, args.baud, timeout=0.01) as ser:
        pkt = send_command(
            port=ser,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_OPEN,
            payload=open_req,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
        )
        if pkt is None:
            print("No response")
            return 2
        if not _status_ok(pkt):
            code = _status_code(pkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        orr = np.parse_open_resp(pkt.payload)
        handle = orr.handle

        info_req = np.build_info_req(handle, args.max_headers)
        ipkt = _send_retry_not_ready(
            ser,
            np.NETWORK_DEVICE_ID,
            np.CMD_INFO,
            info_req,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
            retries=50,
            sleep_s=0.005,
        )
        if ipkt is None:
            print("No response")
            return 2
        if not _status_ok(ipkt):
            code = _status_code(ipkt)
            print(f"Device status={code} ({_status_str(code)})")
            return 1

        ir = np.parse_info_resp(ipkt.payload)
        print(f"http_status={ir.http_status} content_length={ir.content_length}")
        if ir.header_bytes:
            print(ir.header_bytes.decode("utf-8", errors="replace"), end="")

        close_req = np.build_close_req(handle)
        send_command(
            port=ser,
            device=np.NETWORK_DEVICE_ID,
            command=np.CMD_CLOSE,
            payload=close_req,
            timeout=args.timeout,
            read_max=args.read_max,
            debug=args.debug,
        )

    return 0


def register_subcommands(subparsers) -> None:
    pn = subparsers.add_parser("net", help="Network device commands (binary protocol)")
    nsub = pn.add_subparsers(dest="net_cmd", required=True)

    pno = nsub.add_parser("open", help="Open a URL and return a handle")
    pno.add_argument("--method", type=int, default=1, help="1=GET,2=POST,3=PUT,4=DELETE,5=HEAD")
    pno.add_argument("--flags", type=int, default=0, help="bit0=tls, bit1=follow_redirects, bit2=want_headers")
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
    png.add_argument("--info-retries", type=int, default=8)
    png.add_argument("--info-sleep", type=float, default=0.01)
    png.add_argument("url")
    png.set_defaults(fn=cmd_net_get)

    pnh = nsub.add_parser("head", help="HEAD a URL and print metadata/headers")
    pnh.add_argument("--flags", type=int, default=0)
    pnh.add_argument("--max-headers", type=int, default=2048)
    pnh.add_argument("url")
    pnh.set_defaults(fn=cmd_net_head)
