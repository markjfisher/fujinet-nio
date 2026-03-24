# py/fujinet_tools/cli.py
from __future__ import annotations

import argparse

from . import file as file_cmds
from . import clock as clock_cmds
from . import net as net_cmds
from . import disk as disk_cmds
from . import bbc as bbc_cmds
from . import modem as modem_cmds
from . import monitor as monitor_cmds


def main() -> None:
    p = argparse.ArgumentParser(prog="fujinet")
    p.add_argument("--port", "-p", required=True)
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--timeout", type=float, default=5)
    p.add_argument("--read-max", type=int, default=2048)
    p.add_argument("-d", "--debug", action="store_true", help="Dump raw FujiBus packets")
    p.add_argument("-v", "--verbose", action="store_true")

    sub = p.add_subparsers(dest="cmd", required=True)

    file_cmds.register_subcommands(sub)
    clock_cmds.register_subcommands(sub)
    net_cmds.register_subcommands(sub)
    disk_cmds.register_subcommands(sub)
    bbc_cmds.register_subcommands(sub)
    modem_cmds.register_subcommands(sub)

    pm = sub.add_parser("monitor", help="Live FujiBus-over-SLIP serial monitor")
    pm.add_argument("--timeout", type=float, default=0.01, help="Serial read timeout for incremental reads")
    pm.add_argument("--ascii", action="store_true", help="Include short ASCII payload preview")
    pm.add_argument("--hex", action="store_true", help="Include short payload hex preview")
    pm.add_argument("--full-hex", action="store_true", help="Include full decoded FujiBus payload hex")
    pm.add_argument("--raw", action="store_true", help="Print full raw SLIP frame bytes")
    pm.add_argument("--json", action="store_true", help="Emit one JSON object per frame (JSONL)")
    pm.set_defaults(
        fn=lambda args: monitor_cmds.monitor_port(
            port=args.port,
            baud=args.baud,
            timeout=args.timeout,
            show_ascii=args.ascii,
            show_hex=args.hex,
            show_full_hex=args.full_hex,
            show_raw=args.raw,
            json_output=args.json,
        )
    )

    args = p.parse_args()
    raise SystemExit(args.fn(args))


if __name__ == "__main__":
    main()
