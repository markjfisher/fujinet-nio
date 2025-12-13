from __future__ import annotations

import datetime

from .fujibus import send_command
from . import fileproto as fp

# -----------------------
# clock commands
# -----------------------
CLOCK_DEVICE_ID = 0x45
CLOCK_CMD_GET = 0x01
CLOCK_CMD_SET = 0x02
CLOCKPROTO_VERSION = 1


def _build_clock_get_req() -> bytes:
    return bytes([CLOCKPROTO_VERSION])


def _build_clock_set_req(unix_seconds: int) -> bytes:
    if unix_seconds < 0:
        unix_seconds = 0
    unix_seconds &= (1 << 64) - 1
    b = bytearray()
    b.append(CLOCKPROTO_VERSION)
    for i in range(8):
        b.append((unix_seconds >> (8 * i)) & 0xFF)
    return bytes(b)


def _parse_clock_time_resp(payload: bytes) -> int:
    # u8 version, u8 flags, u16 reserved, u64 unixSeconds
    if len(payload) < 1 + 1 + 2 + 8:
        raise ValueError(f"clock response too short ({len(payload)} bytes)")
    ver = payload[0]
    if ver != CLOCKPROTO_VERSION:
        raise ValueError(f"clock version mismatch: got {ver}, want {CLOCKPROTO_VERSION}")
    # flags = payload[1]
    # reserved = payload[2:4]
    ts, _ = fp.read_u64le(payload, 4)
    return ts


def cmd_clock_get(args) -> int:
    req = _build_clock_get_req()
    pkt = send_command(
        port=args.port,
        device=CLOCK_DEVICE_ID,
        command=CLOCK_CMD_GET,
        payload=req,
        baud=args.baud,
        timeout=args.timeout,
        read_max=args.read_max,
        debug=args.debug,
    )
    if pkt is None:
        print("No response")
        return 2
    if not pkt.params or pkt.params[0] != 0:
        print(f"Device status={pkt.params[0] if pkt.params else '??'}")
        return 1

    try:
        ts = _parse_clock_time_resp(pkt.payload)
    except Exception as e:
        print(f"Bad clock response: {e}")
        return 1

    print(f"device unix: {ts}")
    print(f"device utc : {fp.fmt_utc(ts):>20}")
    return 0


def cmd_clock_set(args) -> int:
    if args.unix is not None:
        ts = int(args.unix)
    else:
        # local machine time -> UTC epoch seconds
        ts = int(datetime.datetime.now(tz=datetime.timezone.utc).timestamp())

    req = _build_clock_set_req(ts)
    pkt = send_command(
        port=args.port,
        device=CLOCK_DEVICE_ID,
        command=CLOCK_CMD_SET,
        payload=req,
        baud=args.baud,
        timeout=args.timeout,
        read_max=args.read_max,
        debug=args.debug,
    )
    if pkt is None:
        print("No response")
        return 2
    if not pkt.params or pkt.params[0] != 0:
        print(f"Device status={pkt.params[0] if pkt.params else '??'}")
        return 1

    try:
        echoed = _parse_clock_time_resp(pkt.payload)
    except Exception as e:
        print(f"Bad clock response: {e}")
        return 1

    print(f"set unix : {ts}")
    print(f"echo unix: {echoed}")
    print(f"utc      : {fp.fmt_utc(echoed):>20}")
    return 0


def register_subcommands(subparsers) -> None:
    """Register ClockDevice commands under `clock`."""
    pc = subparsers.add_parser("clock", help="Clock device commands")
    csub = pc.add_subparsers(dest="clock_cmd", required=True)

    pcg = csub.add_parser("get", help="Get device time")
    pcg.set_defaults(fn=cmd_clock_get)

    pcs = csub.add_parser("set", help="Set device time from this machine (UTC now)")
    pcs.add_argument("--unix", type=int, help="Override: set explicit unix seconds")
    pcs.set_defaults(fn=cmd_clock_set)


