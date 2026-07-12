#!/usr/bin/env python3

import argparse
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from typing import TextIO


EVENT_RE = re.compile(
    r"""
    ^(?P<time>\d{2}:\d{2}:\d{2}\.\d{3})
    .*?
    fujibus:\s+
    (?P<direction>receive|send):
    \s*
    (?P<details>.*)
    $
    """,
    re.VERBOSE,
)


@dataclass
class Event:
    timestamp: datetime
    direction: str
    details: str
    line_number: int


def parse_timestamp(value: str, previous: datetime | None) -> datetime:
    """Parse a log timestamp and account for logs crossing midnight."""
    timestamp = datetime.strptime(value, "%H:%M:%S.%f")

    if previous is not None:
        timestamp = timestamp.replace(
            year=previous.year,
            month=previous.month,
            day=previous.day,
        )

        # The clock moving substantially backwards probably means midnight.
        if timestamp < previous - timedelta(hours=12):
            timestamp += timedelta(days=1)

    return timestamp


def format_delta(delta: timedelta) -> str:
    milliseconds = delta.total_seconds() * 1000
    return f"{milliseconds:10.3f} ms"


def analyse_log(
    input_file: TextIO,
    threshold_ms: float,
    only_jumps: bool,
) -> None:
    previous_event: Event | None = None
    pending_receive: Event | None = None
    previous_timestamp: datetime | None = None

    for line_number, line in enumerate(input_file, start=1):
        match = EVENT_RE.search(line.rstrip("\n"))
        if not match:
            continue

        timestamp = parse_timestamp(match.group("time"), previous_timestamp)
        previous_timestamp = timestamp

        event = Event(
            timestamp=timestamp,
            direction=match.group("direction"),
            details=match.group("details"),
            line_number=line_number,
        )

        event_delta = (
            event.timestamp - previous_event.timestamp
            if previous_event is not None
            else None
        )

        pair_delta: timedelta | None = None

        if event.direction == "receive":
            pending_receive = event

        elif event.direction == "send" and pending_receive is not None:
            pair_delta = event.timestamp - pending_receive.timestamp
            pending_receive = None

        event_ms = (
            event_delta.total_seconds() * 1000
            if event_delta is not None
            else None
        )
        pair_ms = (
            pair_delta.total_seconds() * 1000
            if pair_delta is not None
            else None
        )

        is_jump = (
            (event_ms is not None and event_ms >= threshold_ms)
            or (pair_ms is not None and pair_ms >= threshold_ms)
        )

        if only_jumps and not is_jump:
            previous_event = event
            continue

        marker = " *** JUMP ***" if is_jump else ""

        event_delta_text = (
            format_delta(event_delta)
            if event_delta is not None
            else "       ---"
        )

        pair_text = (
            f"  receive→send={format_delta(pair_delta)}"
            if pair_delta is not None
            else ""
        )

        print(
            f"{event.timestamp:%H:%M:%S.%f}"[:-3]
            + f"  {event.direction:7}"
            + f"  since previous={event_delta_text}"
            + pair_text
            + marker
        )
        print(f"    line {event.line_number}: {event.details}")

        previous_event = event


def open_input(filename: str | None) -> TextIO:
    if filename is None or filename == "-":
        return sys.stdin

    path = Path(filename)

    try:
        return path.open("r", encoding="utf-8", errors="replace")
    except OSError as error:
        raise SystemExit(f"Could not open {path}: {error}") from error


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Show timing deltas between FujiBus receive/send log entries."
        )
    )
    parser.add_argument(
        "logfile",
        nargs="?",
        help="Log file to analyse, or '-' for stdin",
    )
    parser.add_argument(
        "-t",
        "--threshold",
        type=float,
        default=100.0,
        metavar="MS",
        help="Mark deltas at least this large as jumps; default: 100 ms",
    )
    parser.add_argument(
        "-j",
        "--only-jumps",
        action="store_true",
        help="Only print entries exceeding the threshold",
    )

    args = parser.parse_args()

    input_file = open_input(args.logfile)

    try:
        analyse_log(
            input_file=input_file,
            threshold_ms=args.threshold,
            only_jumps=args.only_jumps,
        )
    finally:
        if input_file is not sys.stdin:
            input_file.close()


if __name__ == "__main__":
    main()