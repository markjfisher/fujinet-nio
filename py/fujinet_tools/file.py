from __future__ import annotations
from pathlib import Path
import sys

from .fujibus import FujiBusSession
from . import fileproto as fp
from .common import open_serial, status_ok

# ----------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------


def _parse_uri(arg: str) -> str:
    """
    Parse a single URI argument.

    If the argument contains "://" or starts with "/", it's already a URI.
    Otherwise, treat it as a path on the "host" filesystem.
    """
    if "://" in arg or arg.startswith("/"):
        # Already a URI or absolute path
        return arg
    else:
        # Treat as path on host filesystem
        return arg


def _mkdir_p(*, args, bus: FujiBusSession, uri: str) -> bool:
    req = fp.build_mkdir_req(uri=uri, parents=True, exist_ok=True)
    pkt = bus.send_command_expect(
        device=fp.FILE_DEVICE_ID,
        command=fp.CMD_MAKE_DIRECTORY,
        payload=req,
        expect_device=fp.FILE_DEVICE_ID,
        expect_command=fp.CMD_MAKE_DIRECTORY,
        timeout=args.timeout,
        cmd_txt="MKDIR",
    )
    if pkt is None:
        print("No response")
        return False
    if not status_ok(pkt):
        print(f"Device status={pkt.params[0] if pkt.params else '??'}")
        return False
    fp.parse_mkdir_resp(pkt.payload)
    return True


def _send_file_command(
    *,
    args,
    bus: FujiBusSession,
    command: int,
    payload: bytes,
    cmd_txt: str,
):
    pkt = bus.send_command_expect(
        device=fp.FILE_DEVICE_ID,
        command=command,
        payload=payload,
        expect_device=fp.FILE_DEVICE_ID,
        expect_command=command,
        timeout=args.timeout,
        cmd_txt=cmd_txt,
    )
    if pkt is None:
        print("No response", file=sys.stderr)
        return None
    if not status_ok(pkt):
        print(
            f"Device status={pkt.params[0] if pkt.params else '??'}",
            file=sys.stderr,
        )
        return None
    return pkt


def cmd_list(args) -> int:
    uri = _parse_uri(args.uri)
    start = args.start
    all_entries: list[fp.ListEntry] = []
    list_flags = fp.LIST_FLAG_SORT_BY_NAME
    if args.compact:
        list_flags |= fp.LIST_FLAG_COMPACT
    elif args.long:
        list_flags |= fp.LIST_FLAG_FORMATTED

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        while True:
            req = fp.build_list_req(
                uri,
                start,
                args.max_payload,
                list_flags=list_flags,
            )
            pkt = bus.send_command_expect(
                device=fp.FILE_DEVICE_ID,
                command=fp.CMD_LIST,
                payload=req,
                expect_device=fp.FILE_DEVICE_ID,
                expect_command=fp.CMD_LIST,
                timeout=args.timeout,
                cmd_txt="LIST",
            )
            if pkt is None:
                print("No response", file=sys.stderr)
                return 2

            if not status_ok(pkt):
                print(
                    f"Device status={pkt.params[0] if pkt.params else '??'}",
                    file=sys.stderr,
                )
                return 1

            lr = fp.parse_list_resp(pkt.payload)
            if lr.start_index != start:
                print(
                    f"startIndex echo mismatch: expected {start}, got {lr.start_index}",
                    file=sys.stderr,
                )
                return 1

            if lr.formatted:
                print(lr.text, end="" if lr.text.endswith("\n") else "\n")
                start += lr.entry_count
            else:
                all_entries.extend(lr.entries)
                start += lr.entry_count

            if args.verbose:
                print(
                    f"list chunk: start={lr.start_index} count={lr.entry_count} "
                    f"bytes={lr.entries_len} more={lr.more} compact={lr.compact} "
                    f"formatted={lr.formatted}"
                )

            if not lr.more:
                break

    if not args.long:
        mode = "compact" if args.compact else "binary"
        print(f"{args.uri} (entries={len(all_entries)}, {mode})")
        for e in all_entries:
            kind = "DIR " if e.is_dir else "FILE"
            if args.compact:
                print(f"{kind} {e.name}")
            else:
                print(
                    f"{kind} {e.size_bytes:10d}  {fp.fmt_utc(e.mtime_unix):>20}  {e.name}"
                )

    return 0


def cmd_stat(args) -> int:
    uri = _parse_uri(args.uri)
    req = fp.build_stat_req(uri)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        pkt = bus.send_command_expect(
            device=fp.FILE_DEVICE_ID,
            command=fp.CMD_STAT,
            payload=req,
            expect_device=fp.FILE_DEVICE_ID,
            expect_command=fp.CMD_STAT,
            timeout=args.timeout,
            cmd_txt="STAT",
        )
        if pkt is None:
            print("No response", file=sys.stderr)
            return 2

        # FujiBus convention: status is param[0] on responses
        if not status_ok(pkt):
            print(
                f"Device status={pkt.params[0] if pkt.params else '??'}",
                file=sys.stderr,
            )
            return 1

        st = fp.parse_stat_resp(pkt.payload)
        print(f"{args.uri}")
        print(f"  exists: {st.exists}")
        print(f"  dir:    {st.is_dir}")
        print(f"  size:   {st.size_bytes}")
        print(f"  mtime:  {fp.fmt_utc(st.mtime_unix)}")
    return 0


def cmd_read(args) -> int:
    # One-shot read (single request)
    uri = _parse_uri(args.uri)
    req = fp.build_read_req(uri, args.offset, args.max_bytes)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        pkt = bus.send_command_expect(
            device=fp.FILE_DEVICE_ID,
            command=fp.CMD_READ,
            payload=req,
            expect_device=fp.FILE_DEVICE_ID,
            expect_command=fp.CMD_READ,
            timeout=args.timeout,
            cmd_txt="READ",
        )
        if pkt is None:
            print("No response", file=sys.stderr)
            return 2
        if not status_ok(pkt):
            print(
                f"Device status={pkt.params[0] if pkt.params else '??'}",
                file=sys.stderr,
            )
            return 1

        rr = fp.parse_read_resp(pkt.payload)

        if args.out:
            Path(args.out).write_bytes(rr.data)
        else:
            # default: write to stdout as raw bytes
            sys.stdout.buffer.write(rr.data)

        if args.verbose:
            print(
                f"\n(offset={rr.offset}, len={len(rr.data)}, eof={rr.eof}, truncated={rr.truncated})"
            )
    return 0


def cmd_read_all(args) -> int:
    uri = _parse_uri(args.uri)
    out_path = Path(args.out) if args.out else None
    if out_path:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"")

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        offset = 0
        chunks = []
        total = 0

        while True:
            req = fp.build_read_req(uri, offset, args.chunk)
            pkt = bus.send_command_expect(
                device=fp.FILE_DEVICE_ID,
                command=fp.CMD_READ,
                payload=req,
                expect_device=fp.FILE_DEVICE_ID,
                expect_command=fp.CMD_READ,
                timeout=args.timeout,
                cmd_txt="READ",
            )

            if pkt is None:
                print("No response", file=sys.stderr)
                return 2

            if not status_ok(pkt):
                print(
                    f"Device status={pkt.params[0] if pkt.params else '??'}",
                    file=sys.stderr,
                )
                return 1

            rr = fp.parse_read_resp(pkt.payload)
            if rr.offset != offset:
                print(
                    f"Offset echo mismatch: expected {offset}, got {rr.offset}",
                    file=sys.stderr,
                )
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
                print(
                    f"read chunk: offset={rr.offset} len={n} eof={rr.eof} truncated={rr.truncated}"
                )

            if rr.eof or n == 0:
                break

        if not out_path:
            sys.stdout.buffer.write(b"".join(chunks))

        if args.verbose:
            print(f"total read: {total} bytes")

    return 0


def _parent_dir(path: str) -> str:
    """Return parent directory for a path."""
    p = path.rstrip("/")
    if p == "":
        return "/"
    i = p.rfind("/")
    if i <= 0:
        return "/"
    return p[:i]


def cmd_write(args) -> int:
    uri = _parse_uri(args.uri)
    src_path = Path(args.inp)

    # If uri is a directory (ends with /), append the source filename
    if uri.endswith("/"):
        uri = uri + src_path.name

    data = src_path.read_bytes()

    # Reuse one Serial for the whole transfer (robust for CDC + faster).
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        if args.mkdirs:
            parent = _parent_dir(uri)
            if parent and parent != "/":
                if not _mkdir_p(args=args, bus=bus, uri=parent):
                    return 1

        offset = args.offset
        idx = 0
        total_written = 0

        while idx < len(data):
            chunk = data[idx : idx + args.chunk]
            req = fp.build_write_req(uri, offset, chunk)

            pkt = bus.send_command_expect(
                device=fp.FILE_DEVICE_ID,
                command=fp.CMD_WRITE,
                payload=req,
                expect_device=fp.FILE_DEVICE_ID,
                expect_command=fp.CMD_WRITE,
                timeout=args.timeout,
                cmd_txt="WRITE",
            )

            if pkt is None:
                print("No response", file=sys.stderr)
                return 2

            # FujiBus convention: status is param[0] on responses
            if not status_ok(pkt):
                print(
                    f"Device status={pkt.params[0] if pkt.params else '??'}",
                    file=sys.stderr,
                )
                return 1

            wr = fp.parse_write_resp(pkt.payload)
            if wr.offset != offset:
                print(
                    f"Offset echo mismatch: expected {offset}, got {wr.offset}",
                    file=sys.stderr,
                )
                return 1

            wrote = int(wr.written)
            if wrote < 0:
                wrote = 0

            if args.verbose:
                print(
                    f"write chunk: offset={offset} requested={len(chunk)} written={wrote}"
                )

            total_written += wrote
            offset += wrote
            idx += wrote

            if wrote == 0:
                print("write stalled (0 bytes written), stopping", file=sys.stderr)
                break

        if args.verbose:
            print(f"total written: {total_written} bytes")

    return 0


def cmd_appstore_stat(args) -> int:
    req = fp.build_appstore_stat_req(args.namespace, args.key)
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = _send_file_command(
            args=args,
            bus=bus,
            command=fp.CMD_APPSTORE_STAT,
            payload=req,
            cmd_txt="APPSTORE_STAT",
        )
        if pkt is None:
            return 1
        st = fp.parse_appstore_stat_resp(pkt.payload)
        print(f"{args.namespace}/{args.key}")
        print(f"  exists: {st.exists}")
        print(f"  size:   {st.size_bytes}")
        print(f"  mtime:  {fp.fmt_utc(st.mtime_unix)}")
    return 0


def cmd_appstore_get(args) -> int:
    out_path = Path(args.out) if args.out else None
    if out_path:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"")

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        offset = args.offset
        chunks: list[bytes] = []
        total = 0

        while True:
            req = fp.build_appstore_read_req(args.namespace, args.key, offset, args.chunk)
            pkt = _send_file_command(
                args=args,
                bus=bus,
                command=fp.CMD_APPSTORE_READ,
                payload=req,
                cmd_txt="APPSTORE_READ",
            )
            if pkt is None:
                return 1

            rr = fp.parse_appstore_read_resp(pkt.payload)
            if rr.offset != offset:
                print(
                    f"Offset echo mismatch: expected {offset}, got {rr.offset}",
                    file=sys.stderr,
                )
                return 1
            if not rr.exists:
                print("Key not found", file=sys.stderr)
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
                print(f"read chunk: offset={rr.offset} len={n} eof={rr.eof}")

            if rr.eof or n == 0 or args.once:
                break

        if not out_path:
            sys.stdout.buffer.write(b"".join(chunks))

        if args.verbose:
            print(f"total read: {total} bytes", file=sys.stderr)

    return 0


def cmd_appstore_put(args) -> int:
    if args.file:
        data = Path(args.file).read_bytes()
    elif args.text is not None:
        data = args.text.encode("utf-8")
    else:
        data = sys.stdin.buffer.read()

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        offset = args.offset
        idx = 0
        total_written = 0

        if not data:
            req = fp.build_appstore_write_req(args.namespace, args.key, offset, b"")
            pkt = _send_file_command(
                args=args,
                bus=bus,
                command=fp.CMD_APPSTORE_WRITE,
                payload=req,
                cmd_txt="APPSTORE_WRITE",
            )
            if pkt is None:
                return 1
            wr = fp.parse_appstore_write_resp(pkt.payload)
            if args.verbose:
                print(f"write chunk: offset={wr.offset} requested=0 written={wr.written}")
        else:
            while idx < len(data):
                chunk = data[idx : idx + args.chunk]
                req = fp.build_appstore_write_req(args.namespace, args.key, offset, chunk)
                pkt = _send_file_command(
                    args=args,
                    bus=bus,
                    command=fp.CMD_APPSTORE_WRITE,
                    payload=req,
                    cmd_txt="APPSTORE_WRITE",
                )
                if pkt is None:
                    return 1

                wr = fp.parse_appstore_write_resp(pkt.payload)
                if wr.offset != offset:
                    print(
                        f"Offset echo mismatch: expected {offset}, got {wr.offset}",
                        file=sys.stderr,
                    )
                    return 1

                wrote = int(wr.written)
                if args.verbose:
                    print(
                        f"write chunk: offset={offset} requested={len(chunk)} written={wrote}"
                    )
                if wrote == 0:
                    print("write stalled (0 bytes written), stopping", file=sys.stderr)
                    return 1

                total_written += wrote
                offset += wrote
                idx += wrote

        if args.verbose:
            print(f"total written: {total_written} bytes")

    return 0


def cmd_appstore_delete(args) -> int:
    req = fp.build_appstore_delete_req(args.namespace, args.key)
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = _send_file_command(
            args=args,
            bus=bus,
            command=fp.CMD_APPSTORE_DELETE,
            payload=req,
            cmd_txt="APPSTORE_DELETE",
        )
        if pkt is None:
            return 1
        dr = fp.parse_appstore_delete_resp(pkt.payload)
        if args.verbose:
            print(f"deleted: {dr.deleted}")
    return 0


def cmd_appstore_list(args) -> int:
    start = args.start
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        while True:
            req = fp.build_appstore_list_req(args.namespace, start, args.max_payload)
            pkt = _send_file_command(
                args=args,
                bus=bus,
                command=fp.CMD_APPSTORE_LIST,
                payload=req,
                cmd_txt="APPSTORE_LIST",
            )
            if pkt is None:
                return 1

            lr = fp.parse_appstore_list_resp(pkt.payload)
            if lr.start_index != start:
                print(
                    f"startIndex echo mismatch: expected {start}, got {lr.start_index}",
                    file=sys.stderr,
                )
                return 1
            for key in lr.keys:
                print(key)

            if args.verbose:
                print(
                    f"list chunk: start={lr.start_index} count={lr.key_count} "
                    f"bytes={lr.keys_len} more={lr.more}",
                    file=sys.stderr,
                )

            start += lr.key_count
            if not lr.more:
                break
    return 0


def register_subcommands(subparsers) -> None:
    """Register FileDevice commands (list/stat/read/read-all/write) on a top-level subparser."""
    pl = subparsers.add_parser("list", help="List directory entries via FileDevice")
    pl.add_argument("--start", type=int, default=0)
    pl.add_argument(
        "--max-payload",
        type=int,
        default=512,
        help="Max bytes for the variable entries blob per request",
    )
    list_mode = pl.add_mutually_exclusive_group()
    list_mode.add_argument(
        "--compact",
        action="store_true",
        help="Omit per-entry size and mtime (names and dir/file type only)",
    )
    list_mode.add_argument(
        "--long",
        action="store_true",
        help="Request ls-style formatted lines (variable width, like ls -l)",
    )
    pl.add_argument("--verbose", action="store_true")
    pl.add_argument("uri", help="URI (e.g., tnfs://host:port/, /path, sd0:/path)")
    pl.set_defaults(fn=cmd_list)

    ps = subparsers.add_parser("stat", help="Stat a file/dir via FileDevice")
    ps.add_argument("uri", help="URI (e.g., tnfs://host:port/file, /path, sd0:/path)")
    ps.set_defaults(fn=cmd_stat)

    pr = subparsers.add_parser("read", help="Read a single chunk")
    pr.add_argument("--offset", type=int, default=0)
    pr.add_argument("--max-bytes", type=int, default=512)
    pr.add_argument("--out", help="Write chunk to this file (else stdout)")
    pr.add_argument("uri", help="URI (e.g., tnfs://host:port/file, /path, sd0:/path)")
    pr.set_defaults(fn=cmd_read)

    pra = subparsers.add_parser("read-all", help="Read whole file in chunks")
    pra.add_argument("--chunk", type=int, default=512)
    pra.add_argument("--out", help="Write to this file (else stdout)")
    pra.add_argument("uri", help="URI (e.g., tnfs://host:port/file, /path, sd0:/path)")
    pra.set_defaults(fn=cmd_read_all)

    pw = subparsers.add_parser(
        "write", help="Write a local file to the remote path in chunks"
    )
    pw.add_argument("--offset", type=int, default=0)
    pw.add_argument("--chunk", type=int, default=512)
    pw.add_argument(
        "--mkdirs",
        action="store_true",
        help="Create parent directories if needed (mkdir -p)",
    )
    pw.add_argument("uri", help="URI (e.g., tnfs://host:port/file, /path, sd0:/path)")
    pw.add_argument("inp", help="Local input file")
    pw.set_defaults(fn=cmd_write)

    pas = subparsers.add_parser("appstore", help="Application storage commands")
    appsub = pas.add_subparsers(dest="appstore_cmd", required=True)

    ast = appsub.add_parser("stat", help="Stat an application storage key")
    ast.add_argument("namespace")
    ast.add_argument("key")
    ast.set_defaults(fn=cmd_appstore_stat)

    ag = appsub.add_parser("get", help="Read an application storage key")
    ag.add_argument("--offset", type=int, default=0)
    ag.add_argument("--chunk", type=int, default=512)
    ag.add_argument("--out", help="Write to this file (else stdout)")
    ag.add_argument("--once", action="store_true", help="Read one chunk only")
    ag.add_argument("namespace")
    ag.add_argument("key")
    ag.set_defaults(fn=cmd_appstore_get)

    ap = appsub.add_parser("put", help="Write an application storage key")
    ap.add_argument("--offset", type=int, default=0)
    ap.add_argument("--chunk", type=int, default=512)
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--file", help="Local file to write")
    src.add_argument("--text", help="UTF-8 text to write")
    ap.add_argument("namespace")
    ap.add_argument("key")
    ap.set_defaults(fn=cmd_appstore_put)

    ad = appsub.add_parser("delete", help="Delete an application storage key")
    ad.add_argument("namespace")
    ad.add_argument("key")
    ad.set_defaults(fn=cmd_appstore_delete)

    al = appsub.add_parser("list", help="List keys in an application storage namespace")
    al.add_argument("--start", type=int, default=0)
    al.add_argument("--max-payload", type=int, default=512)
    al.add_argument("namespace")
    al.set_defaults(fn=cmd_appstore_list)
