# py/fujinet_tools/bbc.py
from __future__ import annotations

import sys
from pathlib import Path

from .common import open_serial, status_ok
from .fujibus import FujiBusSession
from . import diskproto as dp
from .bbc_dfs import find_entry, parse_dfs_catalogue_090


BOOT_TEXT = {
    0: "none",
    1: "load",
    2: "run",
    3: "exec",
}


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


def _read_sector(*, args, slot: int, lba: int, max_bytes: int = 256) -> bytes:
    req = dp.build_read_sector_req(slot=slot, lba=lba, max_bytes=max_bytes)
    pkt = _send_expect(args=args, command=dp.CMD_READ_SECTOR, payload=req, cmd_txt="DISK_READ_SECTOR")
    if pkt is None:
        raise RuntimeError("no response")
    if not status_ok(pkt):
        st = int(pkt.params[0]) if pkt.params else -1
        raise RuntimeError(f"status={st}")
    rr = dp.parse_read_sector_resp(pkt.payload)
    return rr.data


def _read_catalogue(*, args, slot: int):
    s0 = _read_sector(args=args, slot=slot, lba=0, max_bytes=256)
    s1 = _read_sector(args=args, slot=slot, lba=1, max_bytes=256)
    desc, entries = parse_dfs_catalogue_090(sector0=s0, sector1=s1)
    return desc, entries


def cmd_dfs_info(args) -> int:
    try:
        desc, entries = _read_catalogue(args=args, slot=args.slot)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    boot = BOOT_TEXT.get(desc.boot_option, f"unknown({desc.boot_option})")
    print(f"title={desc.title!s} cycle={desc.cycle_bcd} files={len(entries)} boot={boot} sectors={desc.disc_sectors}")
    return 0


def cmd_dfs_cat(args) -> int:
    try:
        desc, entries = _read_catalogue(args=args, slot=args.slot)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    # A reasonably “DFS-ish” plain listing, stable for tests:
    # D.NAME  L=hhhh  E=hhhh  Len=nnnn  S=sss  [L]
    print(f"Disk: {desc.title}  Files: {len(entries)}  Sectors: {desc.disc_sectors}")
    for e in entries:
        lock = " L" if e.locked else ""
        print(
            "%-10s  load=%05X exec=%05X len=%05X start=%04X%s"
            % (e.full_name, e.load_addr, e.exec_addr, e.length, e.start_sector, lock)
        )
    return 0


def cmd_dfs_read(args) -> int:
    try:
        desc, entries = _read_catalogue(args=args, slot=args.slot)
        ent = find_entry(entries, args.name)
        if ent is None:
            print(f"error: file not found: {args.name!r}", file=sys.stderr)
            return 2
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    out_path: Path | None = Path(args.out) if args.out else None
    remaining = ent.length
    lba = ent.start_sector

    try:
        if out_path:
            f = out_path.open("wb")
        else:
            f = None

        while remaining > 0:
            sec = _read_sector(args=args, slot=args.slot, lba=lba, max_bytes=256)
            take = min(remaining, len(sec))
            chunk = sec[:take]
            if f:
                f.write(chunk)
            else:
                sys.stdout.buffer.write(chunk)
            remaining -= take
            lba += 1
    finally:
        if out_path and "f" in locals() and f:
            f.close()

    return 0


def register_subcommands(subparsers) -> None:
    pb = subparsers.add_parser("bbc", help="BBC helpers (DFS client emulator over DiskDevice)")
    sb = pb.add_subparsers(dest="bbc_cmd", required=True)

    pdfs = sb.add_parser("dfs", help="Acorn DFS 0.90 helpers")
    sdfs = pdfs.add_subparsers(dest="dfs_cmd", required=True)

    pinfo = sdfs.add_parser("info", help="Read and decode the DFS catalogue header")
    pinfo.add_argument("--slot", type=int, required=True)
    pinfo.set_defaults(fn=cmd_dfs_info)

    pcat = sdfs.add_parser("cat", help="List files from the DFS catalogue")
    pcat.add_argument("--slot", type=int, required=True)
    pcat.set_defaults(fn=cmd_dfs_cat)

    pread = sdfs.add_parser("read", help="Read a file from a DFS disk by name (D.NAME or NAME)")
    pread.add_argument("--slot", type=int, required=True)
    pread.add_argument("name", help="D.NAME or NAME (defaults to $ directory)")
    pread.add_argument("--out", help="Write to file (else stdout)")
    pread.set_defaults(fn=cmd_dfs_read)


