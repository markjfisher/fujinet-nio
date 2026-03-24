from __future__ import annotations

import argparse
import json
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Optional

from . import diskproto as dp
from . import fileproto as fp
from . import modemproto as mp
from . import netproto as np


DEVICE_NAMES: dict[int, str] = {
    0x70: "FujiNet",
    0x45: "Clock",
    0xFB: "ModemService",
    0xFC: "DiskService",
    0xFD: "NetworkService",
    0xFE: "FileService",
}

COMMAND_NAMES: dict[int, dict[int, str]] = {
    0x70: {
        0xFF: "Reset",
        0xFE: "GetSsid",
        0xFD: "GetMounts",
        0xFC: "SetMount",
        0xFB: "GetMount",
    },
    0x45: {
        0x01: "Get",
        0x02: "Set",
        0x03: "GetFormat",
        0x04: "GetTimezone",
        0x05: "SetTimezone",
        0x06: "SetTimezoneSave",
    },
    fp.FILE_DEVICE_ID: {
        fp.CMD_STAT: "Stat",
        fp.CMD_LIST: "ListDirectory",
        fp.CMD_READ: "ReadFile",
        fp.CMD_WRITE: "WriteFile",
        fp.CMD_MKDIR: "MakeDirectory",
    },
    dp.DISK_DEVICE_ID: {
        dp.CMD_MOUNT: "Mount",
        dp.CMD_UNMOUNT: "Unmount",
        dp.CMD_READ_SECTOR: "ReadSector",
        dp.CMD_WRITE_SECTOR: "WriteSector",
        dp.CMD_INFO: "Info",
        dp.CMD_CLEAR_CHANGED: "ClearChanged",
        dp.CMD_CREATE: "Create",
    },
    np.NETWORK_DEVICE_ID: {
        np.CMD_OPEN: "Open",
        np.CMD_READ: "Read",
        np.CMD_WRITE: "Write",
        np.CMD_CLOSE: "Close",
        np.CMD_INFO: "Info",
    },
    mp.MODEM_DEVICE_ID: {
        mp.CMD_WRITE: "Write",
        mp.CMD_READ: "Read",
        mp.CMD_STATUS: "Status",
        mp.CMD_CONTROL: "Control",
    },
}


@dataclass
class CapturedFrame:
    line_no: int
    raw: dict[str, Any]
    frame_no: Optional[int]
    timestamp: str
    delta_ms: Optional[float]
    device: Optional[int]
    command: Optional[int]
    checksum_ok: Optional[bool]
    slip_status: str
    slip_reason: Optional[str]
    parse_status: str
    parse_reason: Optional[str]
    params: list[int]
    params_hex: list[str]
    payload_length: int
    payload_hex: str
    payload_ascii: str
    payload_ascii_full: str


def _parse_int_filter(s: str) -> int:
    return int(s, 0)


def _short_ts(ts: str) -> str:
    return ts.split(" ")[-1] if " " in ts else ts


def _device_name(device: Optional[int]) -> str:
    if device is None:
        return "-"
    return DEVICE_NAMES.get(device, f"0x{device:02X}")


def _command_name(device: Optional[int], command: Optional[int]) -> str:
    if command is None:
        return "-"
    if device is None:
        return f"0x{command:02X}"
    return COMMAND_NAMES.get(device, {}).get(command, f"0x{command:02X}")


def _params_compact(ev: CapturedFrame) -> str:
    return " ".join(ev.params_hex) if ev.params_hex else "-"


def _extract_string_hints(text: str) -> list[str]:
    hints: list[str] = []
    if not text:
        return hints
    for token in ["sd0:/", "tnfs://", ".ini", ".atr", ".cas", ".xex", "http://", "https://", "host:"]:
        idx = text.lower().find(token)
        if idx >= 0:
            end = idx
            while end < len(text) and 32 <= ord(text[end]) < 127:
                end += 1
            hints.append(text[idx:end][:80])
    if not hints:
        cleaned = text.strip(".")
        if cleaned and any(ch.isalnum() for ch in cleaned):
            hints.append(cleaned[:80])
    seen: list[str] = []
    for h in hints:
        if h not in seen:
            seen.append(h)
    return seen[:3]


def _payload_hints(ev: CapturedFrame) -> list[str]:
    return _extract_string_hints(ev.payload_ascii_full or ev.payload_ascii)


def _decode_semantic_hint(ev: CapturedFrame) -> Optional[str]:
    try:
        payload = bytes.fromhex(ev.payload_hex) if ev.payload_hex else b""
    except Exception:
        return None

    try:
        if ev.device == fp.FILE_DEVICE_ID and ev.command == fp.CMD_STAT:
            r = fp.parse_stat_resp(payload)
            return f"exists={int(r.exists)} dir={int(r.is_dir)} size={r.size_bytes}"
        if ev.device == fp.FILE_DEVICE_ID and ev.command == fp.CMD_LIST:
            r = fp.parse_list_resp(payload)
            names = ", ".join(e.name for e in r.entries[:3])
            extra = "..." if len(r.entries) > 3 else ""
            return f"entries={len(r.entries)} more={int(r.more)} {names}{extra}".strip()
        if ev.device == fp.FILE_DEVICE_ID and ev.command == fp.CMD_READ:
            r = fp.parse_read_resp(payload)
            return f"offset={r.offset} data_len={len(r.data)} eof={int(r.eof)}"
        if ev.device == fp.FILE_DEVICE_ID and ev.command == fp.CMD_WRITE:
            r = fp.parse_write_resp(payload)
            return f"offset={r.offset} written={r.written}"

        if ev.device == dp.DISK_DEVICE_ID and ev.command == dp.CMD_INFO:
            r = dp.parse_info_resp(payload)
            return f"slot={r.slot} inserted={int(r.inserted)} sector_size={r.sector_size} sectors={r.sector_count}"
        if ev.device == dp.DISK_DEVICE_ID and ev.command == dp.CMD_READ_SECTOR:
            r = dp.parse_read_sector_resp(payload)
            return f"slot={r.slot} lba={r.lba} data_len={len(r.data)}"
        if ev.device == dp.DISK_DEVICE_ID and ev.command == dp.CMD_WRITE_SECTOR:
            r = dp.parse_write_sector_resp(payload)
            return f"slot={r.slot} lba={r.lba} written={r.written_len}"

        if ev.device == np.NETWORK_DEVICE_ID and ev.command == np.CMD_OPEN:
            r = np.parse_open_resp(payload)
            return f"accepted={int(r.accepted)} handle={r.handle} needs_body_write={int(r.needs_body_write)}"
        if ev.device == np.NETWORK_DEVICE_ID and ev.command == np.CMD_INFO:
            r = np.parse_info_resp(payload)
            return f"handle={r.handle} http_status={r.http_status} content_length={r.content_length}"
        if ev.device == np.NETWORK_DEVICE_ID and ev.command == np.CMD_READ:
            r = np.parse_read_resp(payload)
            return f"handle={r.handle} offset={r.offset} data_len={len(r.data)} eof={int(r.eof)}"
        if ev.device == np.NETWORK_DEVICE_ID and ev.command == np.CMD_WRITE:
            r = np.parse_write_resp(payload)
            return f"handle={r.handle} offset={r.offset} written={r.written}"

        if ev.device == mp.MODEM_DEVICE_ID and ev.command == mp.CMD_STATUS:
            r = mp.parse_status_resp(payload)
            return f"connected={int(r.connected)} cmd_mode={int(r.cmd_mode)} host_rx_avail={r.host_rx_avail}"
        if ev.device == mp.MODEM_DEVICE_ID and ev.command == mp.CMD_READ:
            r = mp.parse_read_resp(payload)
            return f"offset={r.offset} data_len={len(r.data)}"
        if ev.device == mp.MODEM_DEVICE_ID and ev.command == mp.CMD_WRITE:
            r = mp.parse_write_resp(payload)
            return f"offset={r.offset} written={r.written}"
    except Exception:
        return None

    return None


def _frame_from_record(line_no: int, rec: dict[str, Any]) -> CapturedFrame:
    payload = rec.get("payload") or {}
    return CapturedFrame(
        line_no=line_no,
        raw=rec,
        frame_no=rec.get("frame"),
        timestamp=str(rec.get("timestamp", "")),
        delta_ms=rec.get("delta_ms"),
        device=rec.get("device"),
        command=rec.get("command"),
        checksum_ok=rec.get("checksum_ok"),
        slip_status=str((rec.get("slip") or {}).get("status", "fail")),
        slip_reason=(rec.get("slip") or {}).get("reason"),
        parse_status=str((rec.get("parse") or {}).get("status", "fail")),
        parse_reason=(rec.get("parse") or {}).get("reason"),
        params=list(rec.get("params") or []),
        params_hex=list(rec.get("params_hex") or []),
        payload_length=int(rec.get("payload_length", payload.get("length", 0)) or 0),
        payload_hex=str(payload.get("hex", "")),
        payload_ascii=str(payload.get("ascii", "")),
        payload_ascii_full=str(payload.get("payload_ascii_full", "")),
    )


def _iter_jsonl(path: Path) -> Iterable[tuple[int, dict[str, Any] | None, str | None]]:
    with path.open("r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, start=1):
            s = line.strip()
            if not s:
                continue
            try:
                obj = json.loads(s)
            except Exception as e:
                yield line_no, None, str(e)
                continue
            if not isinstance(obj, dict):
                yield line_no, None, "json_value_not_object"
                continue
            yield line_no, obj, None


def _matches(ev: CapturedFrame, args: argparse.Namespace) -> bool:
    if args.device is not None and ev.device != args.device:
        return False
    if args.command is not None and ev.command != args.command:
        return False
    if args.checksum_fail and ev.checksum_ok is not False:
        return False
    if args.parse_fail and ev.parse_status != "fail":
        return False
    if args.slip_fail and ev.slip_status != "fail":
        return False
    return True


def _format_summary(ev: CapturedFrame) -> str:
    head = [
        f"#{ev.frame_no if ev.frame_no is not None else '?'}",
        _short_ts(ev.timestamp),
        f"+{'-' if ev.delta_ms is None else f'{ev.delta_ms:.1f}'}ms",
    ]
    if ev.slip_status != "ok":
        head += [f"slip=FAIL", f"reason={ev.slip_reason or 'unknown'}"]
        return " ".join(head)
    if ev.parse_status != "ok":
        head += [
            f"dev={_device_name(ev.device)}",
            f"cmd={_command_name(ev.device, ev.command)}",
            "parse=FAIL",
            f"reason={ev.parse_reason or 'unknown'}",
        ]
        return " ".join(head)

    head += [
        f"dev={_device_name(ev.device)}(0x{(ev.device or 0):02X})",
        f"cmd={_command_name(ev.device, ev.command)}(0x{(ev.command or 0):02X})",
        f"chk={'OK' if ev.checksum_ok else 'BAD'}",
        f"params={_params_compact(ev)}",
        f"payload={ev.payload_length}",
    ]
    hints = _payload_hints(ev)
    if hints:
        head.append(f"hint={json.dumps(hints[0])}")
    return " ".join(head)


def _print_detail(ev: CapturedFrame) -> None:
    print(f"  slip={ev.slip_status} parse={ev.parse_status} checksum_ok={ev.checksum_ok}")
    if ev.params:
        print(f"  params_int={ev.params}")
    if ev.params_hex:
        print(f"  params_hex={ev.params_hex}")
    hints = _payload_hints(ev)
    if hints:
        print(f"  payload_hints={hints}")
    semantic = _decode_semantic_hint(ev)
    if semantic:
        print(f"  decode={semantic}")
    if ev.slip_reason:
        print(f"  slip_reason={ev.slip_reason}")
    if ev.parse_reason:
        print(f"  parse_reason={ev.parse_reason}")


def analyze_capture(args: argparse.Namespace) -> int:
    path = Path(args.capture)
    malformed = 0
    total = 0
    shown = 0
    checksum_fail = 0
    parse_fail = 0
    slip_fail = 0
    devices: Counter[int] = Counter()
    cmd_pairs: Counter[tuple[int, int]] = Counter()

    for line_no, obj, err in _iter_jsonl(path):
        if obj is None:
            malformed += 1
            if not args.quiet_malformed:
                print(f"line {line_no}: malformed_json reason={err}")
            continue

        ev = _frame_from_record(line_no, obj)
        total += 1
        if ev.device is not None:
            devices[ev.device] += 1
        if ev.device is not None and ev.command is not None:
            cmd_pairs[(ev.device, ev.command)] += 1
        if ev.checksum_ok is False:
            checksum_fail += 1
        if ev.parse_status != "ok":
            parse_fail += 1
        if ev.slip_status != "ok":
            slip_fail += 1

        if not _matches(ev, args):
            continue

        print(_format_summary(ev))
        shown += 1
        if args.detail:
            _print_detail(ev)

    if args.stats:
        print("-- stats --")
        print(f"total_frames={total}")
        print(f"shown_frames={shown}")
        print(f"malformed_lines={malformed}")
        print(f"slip_failures={slip_fail}")
        print(f"parse_failures={parse_fail}")
        print(f"checksum_failures={checksum_fail}")
        print(f"unique_devices={len(devices)}")
        print(f"unique_device_commands={len(cmd_pairs)}")
    return 0


def register_subcommands(subparsers) -> None:
    pa = subparsers.add_parser("analyze-capture", help="Analyze FujiBus monitor JSONL capture")
    pa.add_argument("capture", help="Path to JSONL capture file produced by the monitor")
    pa.add_argument("--detail", action="store_true", help="Show extra decoded detail below each matching frame")
    pa.add_argument("--device", type=_parse_int_filter, default=None, help="Filter by device id, e.g. 0xFE")
    pa.add_argument("--command", type=_parse_int_filter, default=None, help="Filter by command id, e.g. 0x02")
    pa.add_argument("--checksum-fail", action="store_true", help="Show only checksum failures")
    pa.add_argument("--parse-fail", action="store_true", help="Show only parse failures")
    pa.add_argument("--slip-fail", action="store_true", help="Show only SLIP decode failures")
    pa.add_argument("--no-stats", action="store_true", help="Do not print summary stats at end")
    pa.add_argument("--quiet-malformed", action="store_true", help="Suppress malformed JSONL line reports")
    pa.set_defaults(fn=lambda args: analyze_capture(argparse.Namespace(**{**vars(args), "stats": not args.no_stats})))


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Analyze FujiBus monitor JSONL captures")
    p.add_argument("capture", help="Path to JSONL capture file produced by the monitor")
    p.add_argument("--detail", action="store_true", help="Show extra decoded detail below each matching frame")
    p.add_argument("--device", type=_parse_int_filter, default=None, help="Filter by device id, e.g. 0xFE")
    p.add_argument("--command", type=_parse_int_filter, default=None, help="Filter by command id, e.g. 0x02")
    p.add_argument("--checksum-fail", action="store_true", help="Show only checksum failures")
    p.add_argument("--parse-fail", action="store_true", help="Show only parse failures")
    p.add_argument("--slip-fail", action="store_true", help="Show only SLIP decode failures")
    p.add_argument("--no-stats", action="store_true", help="Do not print summary stats at end")
    p.add_argument("--quiet-malformed", action="store_true", help="Suppress malformed JSONL line reports")
    return p


def main() -> int:
    args = build_arg_parser().parse_args()
    args.stats = not args.no_stats
    return analyze_capture(args)


if __name__ == "__main__":
    raise SystemExit(main())
