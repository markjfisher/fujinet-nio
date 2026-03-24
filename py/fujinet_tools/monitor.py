from __future__ import annotations

import argparse
import time
from datetime import datetime, timezone

from .common import open_serial
from .fujibus import (
    FujiPacket,
    _extract_frame_from_rx,
    parse_fuji_packet,
    pretty_ascii,
    pretty_hex,
    slip_decode,
)


def _timestamp_now() -> str:
    return datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def _hex_preview(data: bytes, limit: int = 16) -> str:
    if not data:
        return ""
    head = data[:limit]
    txt = pretty_hex(head)
    if len(data) > limit:
        txt += " ..."
    return txt


def _ascii_preview(data: bytes, limit: int = 16) -> str:
    if not data:
        return ""
    head = data[:limit]
    txt = pretty_ascii(head)
    if len(data) > limit:
        txt += "..."
    return txt


def _format_packet_line(frame_no: int, pkt: FujiPacket) -> str:
    parts = [
        f"[{_timestamp_now()}]",
        f"frame={frame_no}",
        f"dev=0x{pkt.device:02X}",
        f"cmd=0x{pkt.command:02X}",
        f"len={pkt.length}",
        f"chk={'OK' if pkt.checksum_ok else 'BAD'}",
    ]

    if pkt.payload:
        parts.append(f"payload={len(pkt.payload)}")
        parts.append(f"ascii='{_ascii_preview(pkt.payload)}'")
        parts.append(f"hex={_hex_preview(pkt.payload)}")
    else:
        parts.append("payload=0")

    return " ".join(parts)


def monitor_port(*, port: str, baud: int, timeout: float) -> int:
    rx = bytearray()
    frame_no = 0

    with open_serial(port=port, baud=baud, timeout_s=timeout) as ser:
        try:
            while True:
                frame = _extract_frame_from_rx(rx)
                if frame is not None:
                    decoded = slip_decode(frame)
                    if not decoded:
                        continue

                    pkt = parse_fuji_packet(decoded)
                    if pkt is None:
                        print(f"[{_timestamp_now()}] frame=? invalid_fuji_packet raw={_hex_preview(decoded, 24)}")
                        continue

                    frame_no += 1
                    print(_format_packet_line(frame_no, pkt), flush=True)
                    continue

                n_wait = getattr(ser, "in_waiting", 0) or 0
                n = max(1, min(n_wait, 256))
                chunk = ser.read(n)
                if chunk:
                    rx.extend(chunk)
                    continue

                time.sleep(0.001)
        except KeyboardInterrupt:
            return 0


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Live FujiBus-over-SLIP serial monitor")
    p.add_argument("--port", required=True, help="Serial device path, e.g. /dev/ttyUSB0")
    p.add_argument("--baud", type=int, default=115200, help="Baud rate")
    p.add_argument(
        "--timeout",
        type=float,
        default=0.01,
        help="Serial read timeout in seconds for incremental low-latency reads",
    )
    return p


def main() -> int:
    args = build_arg_parser().parse_args()
    return monitor_port(port=args.port, baud=args.baud, timeout=args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
