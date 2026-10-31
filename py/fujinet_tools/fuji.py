from __future__ import annotations

from dataclasses import dataclass

from .common import open_serial, status_ok
from .fujibus import FujiBusSession

# -----------------------
# FujiDevice commands (docs/fuji_device_protocol.md)
# -----------------------
FUJI_DEVICE_ID = 0x70
FUJI_CMD_GET_INFO = 0x01
FUJI_CMD_GET_HSIO = 0x02
FUJI_CMD_SET_HSIO = 0x03
FUJI_INFO_VERSION = 1  # the highest GetInfo version this client understands


@dataclass
class FujiInfo:
    firmware_version: str
    build_profile: str


def build_get_info_req() -> bytes:
    return bytes([FUJI_INFO_VERSION])


def build_hsio_req(index: int | None = None) -> bytes:
    if index is None:
        return b"\x01"
    if index not in (*range(10), 255):
        raise ValueError("HSIO index must be 0-9 or 255 (off)")
    return bytes((1, index))


def parse_hsio_resp(payload: bytes) -> int:
    if len(payload) != 2 or payload[0] != 1 or payload[1] not in (*range(10), 255):
        raise ValueError("invalid HSIO response")
    return payload[1]


def cmd_fuji_hsio(args) -> int:
    index = None if args.index is None else (255 if args.index == 'off' else int(args.index))
    cmd = FUJI_CMD_GET_HSIO if index is None else FUJI_CMD_SET_HSIO
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = bus.send_command_expect(device=FUJI_DEVICE_ID, command=cmd,
                payload=build_hsio_req(index), expect_device=FUJI_DEVICE_ID,
                expect_command=cmd, timeout=args.timeout, cmd_txt="FUJI_HSIO")
        if pkt is None or not status_ok(pkt):
            print("HSIO request failed or is unsupported")
            return 1
        try:
            value = parse_hsio_resp(pkt.payload)
        except ValueError as e:
            print(f"Bad HSIO response: {e}")
            return 1
        print("off" if value == 255 else value)
    return 0


def _string8(payload: bytes, at: int) -> tuple[str, int]:
    if at >= len(payload):
        raise ValueError("truncated string length")
    n = payload[at]
    end = at + 1 + n
    if end > len(payload):
        raise ValueError("truncated string")
    return payload[at + 1 : end].decode("utf-8", errors="replace"), end


def parse_get_info_resp(payload: bytes) -> FujiInfo:
    """version, firmware version (u8 len + text), build profile (u8 len + text).
    The reply's version is the lower of ours and the firmware's
    (protocol_reference.md, "Extending commands"); version 1 has exactly
    these fields, though this parser tolerates trailing bytes."""
    if not payload or not 1 <= payload[0] <= FUJI_INFO_VERSION:
        raise ValueError("unsupported GetInfo response version")
    firmware, at = _string8(payload, 1)
    profile, _ = _string8(payload, at)
    return FujiInfo(firmware, profile)


def cmd_fuji_info(args) -> int:
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = bus.send_command_expect(
            device=FUJI_DEVICE_ID,
            command=FUJI_CMD_GET_INFO,
            payload=build_get_info_req(),
            expect_device=FUJI_DEVICE_ID,
            expect_command=FUJI_CMD_GET_INFO,
            timeout=args.timeout,
            cmd_txt="FUJI_GET_INFO",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            print(f"Device status={pkt.params[0] if pkt.params else '??'} (older firmware answers Unsupported)")
            return 1
        try:
            info = parse_get_info_resp(pkt.payload)
        except ValueError as e:
            print(f"Bad GetInfo response: {e}")
            return 1
        print(f"firmware: {info.firmware_version}")
        print(f"profile : {info.build_profile}")
    return 0


def register_subcommands(subparsers) -> None:
    """Register FujiDevice commands under `fuji`."""
    pf = subparsers.add_parser("fuji", help="FujiDevice commands")
    fsub = pf.add_subparsers(dest="fuji_cmd", required=True)
    pfi = fsub.add_parser(
        "info",
        help="Show the firmware version and build profile",
        description="Ask FujiDevice GetInfo for the firmware version and build profile.",
    )
    pfi.set_defaults(fn=cmd_fuji_info)
    ph = fsub.add_parser("hsio", help="Get or set Atari HSIO index")
    ph.add_argument("index", nargs="?", choices=[str(i) for i in range(10)] + ["off"])
    ph.set_defaults(fn=cmd_fuji_hsio)
