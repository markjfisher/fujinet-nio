#!/usr/bin/env python3

import argparse
import re
import sys
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path
from typing import TextIO


TIMESTAMP_RE = re.compile(
    r"^(?P<time>\d{2}:\d{2}:\d{2}\.\d{3})"
)

RECEIVE_RE = re.compile(
    r"fujibus:\s+receive:\s*(?P<details>.*)$"
)

SEND_RE = re.compile(
    r"fujibus:\s+send:\s*(?P<details>.*)$"
)


@dataclass
class Exchange:
    receive_timestamp: datetime
    receive_line_number: int
    receive_details: str
    previous_event_timestamp: datetime | None

    raw_lines: list[str] = field(default_factory=list)

    send_timestamp: datetime | None = None
    send_line_number: int | None = None
    send_details: str | None = None

    @property
    def gap_before_receive(self) -> timedelta | None:
        if self.previous_event_timestamp is None:
            return None

        return self.receive_timestamp - self.previous_event_timestamp

    @property
    def turnaround(self) -> timedelta | None:
        if self.send_timestamp is None:
            return None

        return self.send_timestamp - self.receive_timestamp


def parse_timestamp(
    value: str,
    previous: datetime | None,
) -> datetime:
    """
    Parse a time-of-day timestamp, accounting for logs crossing midnight.
    """
    timestamp = datetime.strptime(value, "%H:%M:%S.%f")

    if previous is not None:
        timestamp = timestamp.replace(
            year=previous.year,
            month=previous.month,
            day=previous.day,
        )

        # A large backwards movement probably means midnight was crossed.
        if timestamp < previous - timedelta(hours=12):
            timestamp += timedelta(days=1)

    return timestamp


def format_timestamp(timestamp: datetime) -> str:
    return timestamp.strftime("%H:%M:%S.%f")[:-3]


def format_delta(delta: timedelta | None) -> str:
    if delta is None:
        return "---"

    return f"{delta.total_seconds() * 1000:.3f} ms"


def find_jump_reasons(
    exchange: Exchange,
    threshold_ms: float,
) -> list[str]:
    reasons: list[str] = []

    gap = exchange.gap_before_receive

    if gap is not None:
        gap_ms = gap.total_seconds() * 1000

        if gap_ms >= threshold_ms:
            reasons.append(
                f"{gap_ms:.3f} ms before receive"
            )

    turnaround = exchange.turnaround

    if turnaround is not None:
        turnaround_ms = turnaround.total_seconds() * 1000

        if turnaround_ms >= threshold_ms:
            reasons.append(
                f"{turnaround_ms:.3f} ms receive-to-send turnaround"
            )

    return reasons


def print_exchange(
    exchange: Exchange,
    marker: str | None = None,
) -> None:
    print(
        f"Receive at {format_timestamp(exchange.receive_timestamp)}, "
        f"gap before: {format_delta(exchange.gap_before_receive)}, "
        f"turnaround: {format_delta(exchange.turnaround)}"
        + (f"  {marker}" if marker else "")
    )

    for raw_line in exchange.raw_lines:
        print(raw_line, end="" if raw_line.endswith("\n") else "\n")


def print_jump_report(
    previous_exchanges: deque[Exchange],
    replay_exchange: Exchange,
    reasons: list[str],
) -> None:
    print()
    print("=" * 100)
    print(f"JUMP DETECTED: {'; '.join(reasons)}")
    print("=" * 100)

    if previous_exchanges:
        print()
        print(
            f"Previous {len(previous_exchanges)} "
            f"receive/send exchange(s):"
        )

        for index, exchange in enumerate(previous_exchanges, start=1):
            print()
            print(
                f"--- Previous exchange "
                f"{index}/{len(previous_exchanges)} ---"
            )
            print_exchange(exchange)
    else:
        print()
        print("No previous complete exchanges are available.")

    print()
    print("--- Exchange following jump / possible replay ---")
    print_exchange(
        replay_exchange,
        marker="*** JUMP / POSSIBLE REPLAY ***",
    )

    print()
    print("=" * 100)
    print()


def analyse_log(
    input_file: TextIO,
    threshold_ms: float,
    only_jumps: bool,
    context_count: int,
) -> None:
    previous_timestamp: datetime | None = None
    previous_event_timestamp: datetime | None = None

    current_exchange: Exchange | None = None
    exchange_buffer: deque[Exchange] = deque(maxlen=context_count)

    def finish_exchange(exchange: Exchange) -> None:
        """
        Process a completed exchange before adding it to the history buffer.
        """
        reasons = find_jump_reasons(exchange, threshold_ms)

        if only_jumps:
            if reasons:
                print_jump_report(
                    previous_exchanges=exchange_buffer,
                    replay_exchange=exchange,
                    reasons=reasons,
                )
        else:
            marker = "*** JUMP ***" if reasons else None
            print_exchange(exchange, marker=marker)
            print()

        exchange_buffer.append(exchange)

    for line_number, raw_line in enumerate(input_file, start=1):
        timestamp_match = TIMESTAMP_RE.match(raw_line)

        line_timestamp: datetime | None = None

        if timestamp_match:
            line_timestamp = parse_timestamp(
                timestamp_match.group("time"),
                previous_timestamp,
            )
            previous_timestamp = line_timestamp

        receive_match = RECEIVE_RE.search(raw_line)
        send_match = SEND_RE.search(raw_line)

        if receive_match and line_timestamp is not None:
            # A new receive marks the end of the preceding exchange.
            if current_exchange is not None:
                finish_exchange(current_exchange)

            current_exchange = Exchange(
                receive_timestamp=line_timestamp,
                receive_line_number=line_number,
                receive_details=receive_match.group("details"),
                previous_event_timestamp=previous_event_timestamp,
                raw_lines=[raw_line],
            )

            previous_event_timestamp = line_timestamp
            continue

        if current_exchange is not None:
            # Preserve every original line belonging to this exchange,
            # including payload descriptions and hexadecimal dumps.
            current_exchange.raw_lines.append(raw_line)

            if send_match and line_timestamp is not None:
                current_exchange.send_timestamp = line_timestamp
                current_exchange.send_line_number = line_number
                current_exchange.send_details = send_match.group("details")

                previous_event_timestamp = line_timestamp

    # Flush the final exchange at end-of-file.
    if current_exchange is not None:
        finish_exchange(current_exchange)


def open_input(filename: str | None) -> TextIO:
    if filename is None or filename == "-":
        return sys.stdin

    path = Path(filename)

    try:
        return path.open(
            "r",
            encoding="utf-8",
            errors="replace",
        )
    except OSError as error:
        raise SystemExit(
            f"Could not open {path}: {error}"
        ) from error


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Analyse FujiBus receive/send timing and show full packet "
            "dumps around timing jumps."
        )
    )

    parser.add_argument(
        "logfile",
        nargs="?",
        help="Log file to analyse, or '-' for standard input",
    )

    parser.add_argument(
        "-t",
        "--threshold",
        type=float,
        default=100.0,
        metavar="MS",
        help=(
            "Treat gaps or response times at least this large as jumps; "
            "default: 100 ms"
        ),
    )

    parser.add_argument(
        "-j",
        "--only-jumps",
        action="store_true",
        help=(
            "Only print jumps, including preceding exchanges and all "
            "original payload/hexdump lines"
        ),
    )

    parser.add_argument(
        "-c",
        "--context",
        type=int,
        default=5,
        metavar="EXCHANGES",
        help=(
            "Number of preceding exchanges shown with -j; default: 5"
        ),
    )

    args = parser.parse_args()

    if args.threshold < 0:
        parser.error("--threshold cannot be negative")

    if args.context < 0:
        parser.error("--context cannot be negative")

    input_file = open_input(args.logfile)

    try:
        analyse_log(
            input_file=input_file,
            threshold_ms=args.threshold,
            only_jumps=args.only_jumps,
            context_count=args.context,
        )
    finally:
        if input_file is not sys.stdin:
            input_file.close()


if __name__ == "__main__":
    main()