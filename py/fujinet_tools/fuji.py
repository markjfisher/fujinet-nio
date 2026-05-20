from __future__ import annotations

import sys

from .common import open_serial, status_ok
from .fujibus import FujiBusSession
from . import fujiproto as fp


def cmd_mounts(args) -> int:
    flags = 0
    if args.formatted:
        flags |= fp.GET_MOUNTS_FLAG_FORMATTED

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        if flags == 0 and args.first_slot == 0 and args.last_slot == 0:
            req = fp.build_get_mounts_req(
                flags=flags,
                first_slot=args.first_slot,
                last_slot=args.last_slot,
                start_index=0,
                max_payload_bytes=args.max_payload,
            )
            expect_legacy = False
        else:
            req = fp.build_get_mounts_req(
                flags=flags,
                first_slot=args.first_slot,
                last_slot=args.last_slot,
                start_index=0,
                max_payload_bytes=args.max_payload,
            )
            expect_legacy = False

        start = 0
        all_entries: list[fp.MountEntry] = []

        while True:
            req = fp.build_get_mounts_req(
                flags=flags,
                first_slot=args.first_slot,
                last_slot=args.last_slot,
                start_index=start,
                max_payload_bytes=args.max_payload,
            )

            pkt = bus.send_command_expect(
                device=fp.FUJI_DEVICE_ID,
                command=fp.CMD_GET_MOUNTS,
                payload=req,
                expect_device=fp.FUJI_DEVICE_ID,
                expect_command=fp.CMD_GET_MOUNTS,
                timeout=args.timeout,
                cmd_txt="GETMOUNTS",
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

            resp = fp.parse_get_mounts_resp(pkt.payload, expect_legacy=expect_legacy)
            if resp.start_index != start:
                print(
                    f"startIndex echo mismatch: expected {start}, got {resp.start_index}",
                    file=sys.stderr,
                )
                return 1

            if resp.formatted:
                print(resp.text, end="" if resp.text.endswith("\n") or not resp.text else "\n")
            elif resp.legacy:
                all_entries.extend(resp.entries)
            else:
                all_entries.extend(resp.entries)

            if args.verbose:
                print(
                    f"mounts chunk: legacy={resp.legacy} formatted={resp.formatted} "
                    f"first_slot={resp.first_slot} start={resp.start_index} "
                    f"count={resp.entry_count} bytes={resp.entries_len} more={resp.more}"
                )

            start += resp.entry_count
            if not resp.more:
                break

        if not args.formatted:
            for entry in all_entries:
                state = "on" if entry.enabled else "off"
                print(f"slot={entry.slot} state={state} mode={entry.mode} uri={entry.uri}")

    return 0


def register_subcommands(subparsers) -> None:
    pm = subparsers.add_parser("mounts", help="List configured Fuji mounts")
    pm.add_argument(
        "--formatted",
        action="store_true",
        help="Request text lines directly from FujiDevice",
    )
    pm.add_argument(
        "--max-payload",
        type=int,
        default=512,
        help="Max bytes for the variable formatted/binary blob per request",
    )
    pm.add_argument(
        "--first-slot",
        type=int,
        default=0,
        help="First 1-based slot number to include (0 = device default)",
    )
    pm.add_argument(
        "--last-slot",
        type=int,
        default=0,
        help="Last 1-based slot number to include (0 = device default)",
    )
    pm.add_argument("--verbose", action="store_true")
    pm.set_defaults(fn=cmd_mounts)
