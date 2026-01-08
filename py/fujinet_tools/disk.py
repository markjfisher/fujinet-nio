# py/fujinet_tools/disk.py
from __future__ import annotations

import sys
from pathlib import Path

from .common import open_serial, status_ok
from .fujibus import FujiBusSession
from . import diskproto as dp


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


TYPE_TEXT = {
    dp.TYPE_AUTO: "auto",
    dp.TYPE_ATR: "atr",
    dp.TYPE_SSD: "ssd",
    dp.TYPE_DSD: "dsd",
    dp.TYPE_RAW: "raw",
}


def _status_str(code: int) -> str:
    return STATUS_TEXT.get(code, f"Unknown({code})")


def _type_str(t: int) -> str:
    return TYPE_TEXT.get(t, f"unknown({t})")


def _type_parse(s: str) -> int:
    sl = (s or "").strip().lower()
    if sl in ("auto", ""):
        return dp.TYPE_AUTO
    if sl == "atr":
        return dp.TYPE_ATR
    if sl == "ssd":
        return dp.TYPE_SSD
    if sl == "dsd":
        return dp.TYPE_DSD
    if sl == "raw":
        return dp.TYPE_RAW
    raise ValueError(f"unknown type {s!r} (expected auto|atr|ssd|dsd|raw)")


def _send_expect(*, args, command: int, payload: bytes, cmd_txt: str):
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = bus.send_command_expect(
            device=dp.DISK_DEVICE_ID,
            command=command,
            payload=payload,
            expect_device=dp.DISK_DEVICE_ID,
            expect_command=command,
            timeout=args.timeout,
            cmd_txt=cmd_txt,
        )
        return pkt


def cmd_mount(args) -> int:
    req = dp.build_mount_req(
        slot=args.slot,
        fs=args.fs,
        path=args.path,
        readonly=args.ro,
        type_override=_type_parse(args.type),
        sector_size_hint=args.sector_size,
    )

    pkt = _send_expect(args=args, command=dp.CMD_MOUNT, payload=req, cmd_txt="DISK_MOUNT")
    if pkt is None:
        print("No response", file=sys.stderr)
        return 2
    if not status_ok(pkt):
        st = int(pkt.params[0]) if pkt.params else -1
        print(f"status={st} ({_status_str(st)})", file=sys.stderr)
        return 1

    mr = dp.parse_mount_resp(pkt.payload)
    print(
        "mounted=%d readonly=%d slot=%d type=%s sector_size=%d sector_count=%d"
        % (1 if mr.mounted else 0, 1 if mr.readonly else 0, mr.slot, _type_str(mr.img_type), mr.sector_size, mr.sector_count)
    )
    return 0


def cmd_unmount(args) -> int:
    req = dp.build_unmount_req(slot=args.slot)
    pkt = _send_expect(args=args, command=dp.CMD_UNMOUNT, payload=req, cmd_txt="DISK_UNMOUNT")
    if pkt is None:
        print("No response", file=sys.stderr)
        return 2
    if not status_ok(pkt):
        st = int(pkt.params[0]) if pkt.params else -1
        print(f"status={st} ({_status_str(st)})", file=sys.stderr)
        return 1
    print(f"unmounted=1 slot={args.slot}")
    return 0


def cmd_info(args) -> int:
    req = dp.build_info_req(slot=args.slot)
    pkt = _send_expect(args=args, command=dp.CMD_INFO, payload=req, cmd_txt="DISK_INFO")
    if pkt is None:
        print("No response", file=sys.stderr)
        return 2
    if not status_ok(pkt):
        st = int(pkt.params[0]) if pkt.params else -1
        print(f"status={st} ({_status_str(st)})", file=sys.stderr)
        return 1

    ir = dp.parse_info_resp(pkt.payload)
    print(
        "inserted=%d readonly=%d dirty=%d changed=%d slot=%d type=%s sector_size=%d sector_count=%d last_error=%d"
        % (
            1 if ir.inserted else 0,
            1 if ir.readonly else 0,
            1 if ir.dirty else 0,
            1 if ir.changed else 0,
            ir.slot,
            _type_str(ir.img_type),
            ir.sector_size,
            ir.sector_count,
            ir.last_error,
        )
    )
    return 0


def cmd_clear_changed(args) -> int:
    req = dp.build_clear_changed_req(slot=args.slot)
    pkt = _send_expect(args=args, command=dp.CMD_CLEAR_CHANGED, payload=req, cmd_txt="DISK_CLEAR_CHANGED")
    if pkt is None:
        print("No response", file=sys.stderr)
        return 2
    if not status_ok(pkt):
        st = int(pkt.params[0]) if pkt.params else -1
        print(f"status={st} ({_status_str(st)})", file=sys.stderr)
        return 1
    print(f"changed_cleared=1 slot={args.slot}")
    return 0


def cmd_read_sector(args) -> int:
    req = dp.build_read_sector_req(slot=args.slot, lba=args.lba, max_bytes=args.max_bytes)
    pkt = _send_expect(args=args, command=dp.CMD_READ_SECTOR, payload=req, cmd_txt="DISK_READ_SECTOR")
    if pkt is None:
        print("No response", file=sys.stderr)
        return 2
    if not status_ok(pkt):
        st = int(pkt.params[0]) if pkt.params else -1
        print(f"status={st} ({_status_str(st)})", file=sys.stderr)
        return 1

    rr = dp.parse_read_sector_resp(pkt.payload)
    if args.out:
        Path(args.out).write_bytes(rr.data)
    else:
        sys.stdout.buffer.write(rr.data)

    if args.verbose:
        print(
            f"\n(slot={rr.slot} lba={rr.lba} len={len(rr.data)} truncated={1 if rr.truncated else 0})"
        )
    return 0


def cmd_write_sector(args) -> int:
    data = Path(args.inp).read_bytes()
    req = dp.build_write_sector_req(slot=args.slot, lba=args.lba, data=data)

    pkt = _send_expect(args=args, command=dp.CMD_WRITE_SECTOR, payload=req, cmd_txt="DISK_WRITE_SECTOR")
    if pkt is None:
        print("No response", file=sys.stderr)
        return 2
    if not status_ok(pkt):
        st = int(pkt.params[0]) if pkt.params else -1
        print(f"status={st} ({_status_str(st)})", file=sys.stderr)
        return 1

    wr = dp.parse_write_sector_resp(pkt.payload)
    print(f"written_len={wr.written_len} slot={wr.slot} lba={wr.lba}")
    return 0


def cmd_create(args) -> int:
    req = dp.build_create_req(
        fs=args.fs,
        path=args.path,
        img_type=_type_parse(args.type),
        sector_size=args.sector_size,
        sector_count=args.sector_count,
        overwrite=args.force,
    )

    pkt = _send_expect(args=args, command=dp.CMD_CREATE, payload=req, cmd_txt="DISK_CREATE")
    if pkt is None:
        print("No response", file=sys.stderr)
        return 2
    if not status_ok(pkt):
        st = int(pkt.params[0]) if pkt.params else -1
        print(f"status={st} ({_status_str(st)})", file=sys.stderr)
        return 1

    cr = dp.parse_create_resp(pkt.payload)
    print(f"created=1 type={_type_str(cr.img_type)} sector_size={cr.sector_size} sector_count={cr.sector_count}")
    return 0


def register_subcommands(subparsers) -> None:
    pd = subparsers.add_parser("disk", help="Disk device commands (DiskDevice v1)")
    sd = pd.add_subparsers(dest="disk_cmd", required=True)

    pm = sd.add_parser("mount", help="Mount an image into a slot")
    pm.add_argument("--slot", type=int, required=True)
    pm.add_argument("--fs", required=True)
    pm.add_argument("--path", required=True)
    pm.add_argument("--ro", action="store_true", help="Request readonly")
    pm.add_argument("--type", default="auto", help="auto|atr|ssd|dsd|raw")
    pm.add_argument("--sector-size", type=int, default=256, help="Sector size hint (used for raw)")
    pm.set_defaults(fn=cmd_mount)

    pu = sd.add_parser("unmount", help="Unmount a slot")
    pu.add_argument("--slot", type=int, required=True)
    pu.set_defaults(fn=cmd_unmount)

    pi = sd.add_parser("info", help="Query slot status and geometry")
    pi.add_argument("--slot", type=int, required=True)
    pi.set_defaults(fn=cmd_info)

    pc = sd.add_parser("clear-changed", help="Clear the slot changed flag")
    pc.add_argument("--slot", type=int, required=True)
    pc.set_defaults(fn=cmd_clear_changed)

    pr = sd.add_parser("read-sector", help="Read a sector by LBA")
    pr.add_argument("--slot", type=int, required=True)
    pr.add_argument("--lba", type=int, required=True)
    pr.add_argument("--max-bytes", type=int, default=256)
    pr.add_argument("--out", help="Write sector to file (else stdout)")
    pr.set_defaults(fn=cmd_read_sector)

    pw = sd.add_parser("write-sector", help="Write a sector by LBA (from local file)")
    pw.add_argument("--slot", type=int, required=True)
    pw.add_argument("--lba", type=int, required=True)
    pw.add_argument("inp", help="Local input file containing sector bytes")
    pw.set_defaults(fn=cmd_write_sector)

    pc = sd.add_parser("create", help="Create a new disk image file (raw/ssd/atr)")
    pc.add_argument("--fs", required=True)
    pc.add_argument("--path", required=True)
    pc.add_argument("--type", required=True, help="raw|ssd|atr (or auto but will fail)")
    pc.add_argument("--sector-size", type=int, required=True)
    pc.add_argument("--sector-count", type=int, required=True)
    pc.add_argument("--force", action="store_true", help="Overwrite if exists")
    pc.set_defaults(fn=cmd_create)


