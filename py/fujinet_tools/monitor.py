from __future__ import annotations

import argparse
import json
import time
from datetime import datetime, timezone
from typing import Any

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


def _bytes_hex(data: bytes) -> str:
    return data.hex()


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


def _infer_direction(pkt: FujiPacket) -> str:
    """
    Conservative heuristic only.

    FujiBus exchanges are host-initiated request/response, but a passive monitor on a
    single serial stream cannot know direction with certainty without extra context.
    We therefore default to "unknown" and only label likely responses when the usual
    FujiBus convention is present: param[0] is a small status code.
    """
    if pkt.params and pkt.params[0] in (0, 1, 2, 3, 4):
        return "fujinet_to_host?"
    return "unknown"


def _packet_to_dict(
    *,
    timestamp: str,
    frame_no: int,
    direction: str,
    raw_frame: bytes,
    decoded: bytes,
    pkt: FujiPacket | None,
) -> dict[str, Any]:
    base: dict[str, Any] = {
        "timestamp": timestamp,
        "frame": frame_no,
        "direction": direction,
        "raw_frame_hex": _bytes_hex(raw_frame),
        "decoded_hex": _bytes_hex(decoded),
    }

    if pkt is None:
        base["valid"] = False
        base["payload"] = {
            "length": max(0, len(decoded) - 6),
            "hex": _bytes_hex(decoded[6:]) if len(decoded) > 6 else "",
            "ascii": _ascii_preview(decoded[6:], limit=len(decoded[6:])) if len(decoded) > 6 else "",
        }
        return base

    base.update(
        {
            "valid": True,
            "device": pkt.device,
            "device_hex": f"0x{pkt.device:02X}",
            "command": pkt.command,
            "command_hex": f"0x{pkt.command:02X}",
            "length": pkt.length,
            "checksum": pkt.checksum,
            "checksum_hex": f"0x{pkt.checksum:02X}",
            "checksum_computed": pkt.checksum_computed,
            "checksum_computed_hex": f"0x{pkt.checksum_computed:02X}",
            "checksum_ok": pkt.checksum_ok,
            "payload_length": len(pkt.payload),
            "params": pkt.params,
            "payload": {
                "hex": _bytes_hex(pkt.payload),
                "ascii": _ascii_preview(pkt.payload, limit=len(pkt.payload)),
            },
        }
    )
    return base


def _format_packet_line(
    frame_no: int,
    pkt: FujiPacket,
    *,
    timestamp: str,
    direction: str,
    show_ascii: bool,
    show_hex: bool,
) -> str:
    parts = [
        f"[{timestamp}]",
        f"frame={frame_no}",
        f"dir={direction}",
        f"chk={'OK' if pkt.checksum_ok else 'BAD'}",
        f"dev=0x{pkt.device:02X}",
        f"cmd=0x{pkt.command:02X}",
        f"len={pkt.length}",
        f"payload={len(pkt.payload)}",
    ]

    if show_ascii and pkt.payload:
        parts.append(f"ascii='{_ascii_preview(pkt.payload)}'")
    if show_hex and pkt.payload:
        parts.append(f"hex={_hex_preview(pkt.payload)}")

    return " ".join(parts)


def _format_invalid_line(
    *,
    frame_no: int,
    timestamp: str,
    direction: str,
    raw_frame: bytes,
    decoded: bytes,
    show_raw: bool,
    show_hex: bool,
    show_ascii: bool,
) -> str:
    parts = [
        f"[{timestamp}]",
        f"frame={frame_no}",
        f"dir={direction}",
        "chk=BAD",
        "invalid_fuji_packet",
        f"decoded_len={len(decoded)}",
    ]
    if show_hex:
        parts.append(f"decoded_hex={_hex_preview(decoded, 24)}")
    if show_ascii and decoded:
        parts.append(f"decoded_ascii='{_ascii_preview(decoded, 24)}'")
    if show_raw:
        parts.append(f"raw={pretty_hex(raw_frame)}")
    return " ".join(parts)


def monitor_port(
    *,
    port: str,
    baud: int,
    timeout: float,
    show_ascii: bool = False,
    show_hex: bool = False,
    show_raw: bool = False,
    json_output: bool = False,
) -> int:
    rx = bytearray()
    frame_no = 0

    with open_serial(port=port, baud=baud, timeout_s=timeout) as ser:
        try:
            while True:
                frame = _extract_frame_from_rx(rx)
                if frame is not None:
                    frame_no += 1
                    timestamp = _timestamp_now()
                    decoded = slip_decode(frame)
                    if not decoded:
                        continue

                    pkt = parse_fuji_packet(decoded)
                    direction = _infer_direction(pkt) if pkt is not None else "unknown"

                    if json_output:
                        print(
                            json.dumps(
                                _packet_to_dict(
                                    timestamp=timestamp,
                                    frame_no=frame_no,
                                    direction=direction,
                                    raw_frame=frame,
                                    decoded=decoded,
                                    pkt=pkt,
                                ),
                                separators=(",", ":"),
                            ),
                            flush=True,
                        )
                        continue

                    if pkt is None:
                        print(
                            _format_invalid_line(
                                frame_no=frame_no,
                                timestamp=timestamp,
                                direction=direction,
                                raw_frame=frame,
                                decoded=decoded,
                                show_raw=show_raw,
                                show_hex=show_hex,
                                show_ascii=show_ascii,
                            ),
                            flush=True,
                        )
                        continue

                    print(
                        _format_packet_line(
                            frame_no,
                            pkt,
                            timestamp=timestamp,
                            direction=direction,
                            show_ascii=show_ascii,
                            show_hex=show_hex,
                        ),
                        flush=True,
                    )
                    if show_raw:
                        print(f"  raw={pretty_hex(frame)}", flush=True)
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
    p.add_argument("--ascii", action="store_true", help="Include short ASCII payload preview")
    p.add_argument("--hex", action="store_true", help="Include short payload hex preview")
    p.add_argument("--raw", action="store_true", help="Print full raw SLIP frame bytes")
    p.add_argument("--json", action="store_true", help="Emit one JSON object per frame (JSONL)")
    return p


def main() -> int:
    args = build_arg_parser().parse_args()
    return monitor_port(
        port=args.port,
        baud=args.baud,
        timeout=args.timeout,
        show_ascii=args.ascii,
        show_hex=args.hex,
        show_raw=args.raw,
        json_output=args.json,
    )


if __name__ == "__main__":
    raise SystemExit(main())
