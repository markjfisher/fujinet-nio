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
    parse_fuji_packet_ex,
    pretty_ascii,
    pretty_hex,
    slip_decode_ex,
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


def _payload_ascii_full(data: bytes) -> str:
    return pretty_ascii(data)


def _param_hex(v: int) -> str:
    if 0 <= v <= 0xFF:
        return f"{v:02X}"
    if 0 <= v <= 0xFFFF:
        return f"{v:04X}"
    return f"{v:08X}"


def _params_hex(values: list[int]) -> str:
    if not values:
        return "-"
    return " ".join(_param_hex(v) for v in values)


def _delta_ms(prev_ts: float | None, now_ts: float) -> float | None:
    if prev_ts is None:
        return None
    return round((now_ts - prev_ts) * 1000.0, 3)


def _packet_to_dict(
    *,
    timestamp: str,
    frame_no: int,
    direction: str,
    delta_ms: float | None,
    raw_frame: bytes,
    slip_status: str,
    slip_reason: str | None,
    slip_warnings: list[str],
    decoded: bytes,
    parse_status: str,
    parse_reason: str | None,
    pkt: FujiPacket | None,
) -> dict[str, Any]:
    base: dict[str, Any] = {
        "timestamp": timestamp,
        "frame": frame_no,
        "direction": direction,
        "delta_ms": delta_ms,
        "raw_frame_hex": _bytes_hex(raw_frame),
        "decoded_hex": _bytes_hex(decoded),
        "slip": {
            "status": slip_status,
            "reason": slip_reason,
            "warnings": slip_warnings,
        },
        "parse": {
            "status": parse_status,
            "reason": parse_reason,
        },
    }

    if pkt is None:
        base["payload"] = {
            "length": max(0, len(decoded) - 6),
            "hex": _bytes_hex(decoded[6:]) if len(decoded) > 6 else "",
            "ascii": _ascii_preview(decoded[6:], limit=len(decoded[6:])) if len(decoded) > 6 else "",
            "payload_ascii_full": _payload_ascii_full(decoded[6:]) if len(decoded) > 6 else "",
        }
        return base

    base.update(
        {
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
            "params_hex": [_param_hex(v) for v in pkt.params],
            "payload": {
                "hex": _bytes_hex(pkt.payload),
                "ascii": _ascii_preview(pkt.payload, limit=len(pkt.payload)),
                "payload_ascii_full": _payload_ascii_full(pkt.payload),
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
    delta_ms: float | None,
    show_ascii: bool,
    show_hex: bool,
    show_full_hex: bool,
    show_decoded_hex: bool,
) -> str:
    parts = [
        f"[{timestamp}]",
        f"frame={frame_no}",
        f"delta_ms={'-' if delta_ms is None else f'{delta_ms:.3f}'}",
        f"dir={direction}",
        f"chk={'OK' if pkt.checksum_ok else 'BAD'}",
        f"dev=0x{pkt.device:02X}",
        f"cmd=0x{pkt.command:02X}",
        f"pkt_len={pkt.length}",
        f"params={_params_hex(pkt.params)}",
        f"payload_len={len(pkt.payload)}",
    ]

    if show_ascii and pkt.payload:
        parts.append(f"ascii='{_ascii_preview(pkt.payload)}'")
    if show_full_hex and pkt.payload:
        parts.append(f"hex={pretty_hex(pkt.payload)}")
    elif show_hex and pkt.payload:
        parts.append(f"hex={_hex_preview(pkt.payload)}")

    return " ".join(parts)


def _format_invalid_line(
    *,
    frame_no: int,
    timestamp: str,
    direction: str,
    delta_ms: float | None,
    slip_reason: str | None,
    parse_reason: str | None,
    raw_frame: bytes,
    decoded: bytes,
    show_raw: bool,
    show_hex: bool,
    show_ascii: bool,
    show_full_hex: bool,
    show_decoded_hex: bool,
) -> str:
    parts = [
        f"[{timestamp}]",
        f"frame={frame_no}",
        f"delta_ms={'-' if delta_ms is None else f'{delta_ms:.3f}'}",
        f"dir={direction}",
        f"slip={'FAIL' if slip_reason else 'OK'}",
        f"parse=FAIL",
        f"reason={parse_reason or slip_reason or 'unknown'}",
        f"decoded_len={len(decoded)}",
    ]
    if show_full_hex and decoded:
        parts.append(f"decoded_hex={pretty_hex(decoded)}")
    elif show_hex:
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
    show_full_hex: bool = False,
    show_decoded_hex: bool = False,
    show_raw: bool = False,
    json_output: bool = False,
) -> int:
    rx = bytearray()
    frame_no = 0
    last_emit_ts: float | None = None

    with open_serial(port=port, baud=baud, timeout_s=timeout) as ser:
        try:
            while True:
                frame = _extract_frame_from_rx(rx)
                if frame is not None:
                    frame_no += 1
                    timestamp = _timestamp_now()
                    now_mono = time.monotonic()
                    delta_ms = _delta_ms(last_emit_ts, now_mono)
                    last_emit_ts = now_mono
                    slip_res = slip_decode_ex(frame)
                    decoded = slip_res.decoded
                    parse_res = parse_fuji_packet_ex(decoded) if slip_res.status == "ok" else None
                    pkt = parse_res.packet if parse_res is not None else None
                    direction = "unknown"

                    if json_output:
                        print(
                            json.dumps(
                                _packet_to_dict(
                                    timestamp=timestamp,
                                    frame_no=frame_no,
                                    direction=direction,
                                    delta_ms=delta_ms,
                                    raw_frame=frame,
                                    slip_status=slip_res.status,
                                    slip_reason=slip_res.reason,
                                    slip_warnings=slip_res.warnings,
                                    decoded=decoded,
                                    parse_status=parse_res.status if parse_res is not None else "fail",
                                    parse_reason=parse_res.reason if parse_res is not None else slip_res.reason,
                                    pkt=pkt,
                                ),
                                separators=(",", ":"),
                            ),
                            flush=True,
                        )
                        continue

                    if slip_res.status != "ok":
                        print(
                            f"[{timestamp}] frame={frame_no} delta_ms={'-' if delta_ms is None else f'{delta_ms:.3f}'} dir={direction} slip=FAIL reason={slip_res.reason or 'unknown'}",
                            flush=True,
                        )
                        if slip_res.warnings:
                            print(f"  slip_warnings={','.join(slip_res.warnings)}", flush=True)
                        if show_raw:
                            print(f"  raw={pretty_hex(frame)}", flush=True)
                        continue

                    if pkt is None:
                        print(
                            _format_invalid_line(
                                frame_no=frame_no,
                                timestamp=timestamp,
                                direction=direction,
                                delta_ms=delta_ms,
                                slip_reason=slip_res.reason,
                                parse_reason=parse_res.reason if parse_res is not None else None,
                                raw_frame=frame,
                                decoded=decoded,
                                show_raw=show_raw,
                                show_hex=show_hex,
                                show_ascii=show_ascii,
                                show_full_hex=show_full_hex,
                                show_decoded_hex=show_decoded_hex,
                            ),
                            flush=True,
                        )
                        if show_decoded_hex and decoded:
                            print(f"  decoded_hex={pretty_hex(decoded)}", flush=True)
                        continue

                    print(
                        _format_packet_line(
                            frame_no,
                            pkt,
                            timestamp=timestamp,
                            direction=direction,
                            delta_ms=delta_ms,
                            show_ascii=show_ascii,
                            show_hex=show_hex,
                            show_full_hex=show_full_hex,
                            show_decoded_hex=show_decoded_hex,
                        ),
                        flush=True,
                    )
                    if slip_res.warnings:
                        print(f"  slip_warnings={','.join(slip_res.warnings)}", flush=True)
                    if show_decoded_hex:
                        print(f"  decoded_hex={pretty_hex(decoded)}", flush=True)
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
    p.add_argument("--full-hex", action="store_true", help="Include full decoded FujiBus payload hex")
    p.add_argument("--decoded-hex", action="store_true", help="Print full decoded FujiBus packet hex")
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
        show_full_hex=args.full_hex,
        show_decoded_hex=args.decoded_hex,
        show_raw=args.raw,
        json_output=args.json,
    )


if __name__ == "__main__":
    raise SystemExit(main())
