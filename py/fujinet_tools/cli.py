# py/fujinet_tools/cli.py
from __future__ import annotations

import argparse

from . import file as file_cmds
from . import clock as clock_cmds
from . import net as net_cmds


def main() -> None:
    p = argparse.ArgumentParser(prog="fujinet")
    p.add_argument("--port", "-p", required=True)
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--timeout", type=float, default=0.1)
    p.add_argument("--read-max", type=int, default=2048)
    p.add_argument("-d", "--debug", action="store_true", help="Dump raw FujiBus packets")
    p.add_argument("-v", "--verbose", action="store_true")

    sub = p.add_subparsers(dest="cmd", required=True)

    file_cmds.register_subcommands(sub)
    clock_cmds.register_subcommands(sub)
    net_cmds.register_subcommands(sub)

    args = p.parse_args()
    raise SystemExit(args.fn(args))


if __name__ == "__main__":
    main()
