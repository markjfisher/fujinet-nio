from __future__ import annotations

from pathlib import Path

from .fujibus import send_command
from . import fileproto as fp


def cmd_list(args) -> int:
    req = fp.build_list_req(args.fs, args.path, args.start, args.max)
    pkt = send_command(
        port=args.port,
        device=fp.FILE_DEVICE_ID,
        command=fp.CMD_LIST,
        payload=req,
        baud=args.baud,
        timeout=args.timeout,
        read_max=args.read_max,
        debug=args.debug,
    )
    if pkt is None:
        print("No response")
        return 2

    # FujiBus convention: status is param[0] on responses
    if not pkt.params or pkt.params[0] != 0:
        print(f"Device status={pkt.params[0] if pkt.params else '??'}")
        return 1

    lr = fp.parse_list_resp(pkt.payload)

    print(f"{args.fs}:{args.path} (more={lr.more}, count={len(lr.entries)})")
    for e in lr.entries:
        kind = "DIR " if e.is_dir else "FILE"
        print(f"{kind} {e.size_bytes:10d}  {fp.fmt_utc(e.mtime_unix):>20}  {e.name}")

    return 0


def cmd_stat(args) -> int:
    req = fp.build_stat_req(args.fs, args.path)
    pkt = send_command(
        port=args.port,
        device=fp.FILE_DEVICE_ID,
        command=fp.CMD_STAT,
        payload=req,
        baud=args.baud,
        timeout=args.timeout,
        read_max=args.read_max,
        debug=args.debug,
    )
    if pkt is None:
        print("No response")
        return 2

    # FujiBus convention: status is param[0] on responses
    if not pkt.params or pkt.params[0] != 0:
        print(f"Device status={pkt.params[0] if pkt.params else '??'}")
        return 1

    st = fp.parse_stat_resp(pkt.payload)
    print(f"{args.fs}:{args.path}")
    print(f"  exists: {st.exists}")
    print(f"  dir:    {st.is_dir}")
    print(f"  size:   {st.size_bytes}")
    print(f"  mtime:  {fp.fmt_utc(st.mtime_unix)}")
    return 0


def cmd_read(args) -> int:
    # One-shot read (single request)
    req = fp.build_read_req(args.fs, args.path, args.offset, args.max_bytes)
    pkt = send_command(
        port=args.port,
        device=fp.FILE_DEVICE_ID,
        command=fp.CMD_READ,
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

    rr = fp.parse_read_resp(pkt.payload)

    if args.out:
        Path(args.out).write_bytes(rr.data)
    else:
        # default: write to stdout as raw bytes
        import sys

        sys.stdout.buffer.write(rr.data)

    if args.verbose:
        print(f"\n(offset={rr.offset}, len={len(rr.data)}, eof={rr.eof}, truncated={rr.truncated})")
    return 0


def cmd_read_all(args) -> int:
    # Streaming read in chunks to exercise “split packets”
    out_path = Path(args.out) if args.out else None
    if out_path:
        out_path.parent.mkdir(parents=True, exist_ok=True)

    offset = 0
    chunks = []
    total = 0

    while True:
        req = fp.build_read_req(args.fs, args.path, offset, args.chunk)
        pkt = send_command(
            port=args.port,
            device=fp.FILE_DEVICE_ID,
            command=fp.CMD_READ,
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

        rr = fp.parse_read_resp(pkt.payload)
        if rr.offset != offset:
            print(f"Offset echo mismatch: expected {offset}, got {rr.offset}")
            return 1

        if out_path:
            with out_path.open("ab") as f:
                f.write(rr.data)
        else:
            chunks.append(rr.data)

        n = len(rr.data)
        total += n
        offset += n

        if args.verbose:
            print(f"read chunk: offset={rr.offset} len={n} eof={rr.eof} truncated={rr.truncated}")

        # Stop when device signals eof/truncated with no more data
        if rr.eof or n == 0:
            break

    if not out_path:
        import sys

        sys.stdout.buffer.write(b"".join(chunks))
    if args.verbose:
        print(f"total read: {total} bytes")
    return 0


def cmd_write(args) -> int:
    data = Path(args.inp).read_bytes()

    # Chunked write to exercise “split packets”
    offset = args.offset
    idx = 0
    total_written = 0

    while idx < len(data):
        chunk = data[idx : idx + args.chunk]
        req = fp.build_write_req(args.fs, args.path, offset, chunk)

        pkt = send_command(
            port=args.port,
            device=fp.FILE_DEVICE_ID,
            command=fp.CMD_WRITE,
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

        wr = fp.parse_write_resp(pkt.payload)
        if wr.offset != offset:
            print(f"Offset echo mismatch: expected {offset}, got {wr.offset}")
            return 1

        # Best-effort: device may write fewer bytes than requested
        wrote = int(wr.written)
        if wrote < 0:
            wrote = 0

        if args.verbose:
            print(f"write chunk: offset={offset} requested={len(chunk)} written={wrote}")

        total_written += wrote
        offset += wrote
        idx += wrote

        if wrote == 0:
            print("write stalled (0 bytes written), stopping")
            break

    if args.verbose:
        print(f"total written: {total_written} bytes")
    return 0


def register_subcommands(subparsers) -> None:
    """Register FileDevice commands (list/stat/read/read-all/write) on a top-level subparser."""
    pl = subparsers.add_parser("list", help="List directory entries via FileDevice")
    pl.add_argument("--start", type=int, default=0)
    pl.add_argument("--max", type=int, default=64)
    pl.add_argument("fs")
    pl.add_argument("path")
    pl.set_defaults(fn=cmd_list)

    ps = subparsers.add_parser("stat", help="Stat a file/dir via FileDevice")
    ps.add_argument("fs")
    ps.add_argument("path")
    ps.set_defaults(fn=cmd_stat)

    pr = subparsers.add_parser("read", help="Read a single chunk")
    pr.add_argument("--offset", type=int, default=0)
    pr.add_argument("--max-bytes", type=int, default=512)
    pr.add_argument("--out", help="Write chunk to this file (else stdout)")
    pr.add_argument("fs")
    pr.add_argument("path")
    pr.set_defaults(fn=cmd_read)

    pra = subparsers.add_parser("read-all", help="Read whole file in chunks")
    pra.add_argument("--chunk", type=int, default=512)
    pra.add_argument("--out", help="Write to this file (else stdout)")
    pra.add_argument("fs")
    pra.add_argument("path")
    pra.set_defaults(fn=cmd_read_all)

    pw = subparsers.add_parser("write", help="Write a local file to the remote path in chunks")
    pw.add_argument("--offset", type=int, default=0)
    pw.add_argument("--chunk", type=int, default=512)
    pw.add_argument("fs")
    pw.add_argument("path")
    pw.add_argument("inp", help="Local input file")
    pw.set_defaults(fn=cmd_write)


