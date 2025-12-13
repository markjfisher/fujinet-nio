# py/fujinet_tools/cli.py
from __future__ import annotations
import argparse
import datetime

from fujinet_tools.fujibus import send_command
from fujinet_tools.fileproto import build_listdir, parse_listdir

FILE_DEVICE_ID = 0xFE
CMD_LISTDIR = 0x02

def fmt_utc(ts: int) -> str:
    return datetime.datetime.fromtimestamp(ts, tz=datetime.timezone.utc).isoformat().replace("+00:00", "Z")

def cmd_list(args: argparse.Namespace) -> int:
    payload = build_listdir(args.fs, args.path, start_index=args.start, max_entries=args.max)
    pkt = send_command(
        port=args.port,
        device=FILE_DEVICE_ID,
        command=CMD_LISTDIR,
        payload=payload,
        baud=args.baud,
        timeout=args.timeout,
        read_max=args.read_max,
        debug=args.debug,
    )
    if pkt is None:
        print("No/invalid response")
        return 2

    # If we later expose StatusCode in the packet, check it here.
    # Right now FujiBus packet doesn’t carry status in the header; it’s part of IOResponse.
    # If the transport wraps status into the payload, parse it accordingly.
    result = parse_listdir(pkt.payload)

    print(f"{args.fs}:{args.path} (more={result.more}, count={len(result.entries)})")
    for e in result.entries:
        kind = "DIR " if e.is_dir else "FILE"
        print(f"{kind:4} {e.size_bytes:10}  {fmt_utc(e.mtime_unix):20}  {e.name}")

    return 0

def main() -> None:
    p = argparse.ArgumentParser(prog="fujinet")
    p.add_argument("--port", "-p", required=True)
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--timeout", type=float, default=2.0)
    p.add_argument("--read-max", type=int, default=2048)
    p.add_argument("--debug", "-d", action="store_true",
                   help="Print raw SLIP frame + decoded FujiBus packet")

    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("list", help="List directory entries via FileDevice")
    pl.add_argument("--start", type=int, default=0)
    pl.add_argument("--max", type=int, default=64)
    pl.add_argument("fs")
    pl.add_argument("path")
    pl.set_defaults(fn=cmd_list)

    args = p.parse_args()
    raise SystemExit(args.fn(args))


if __name__ == "__main__":
    main()
