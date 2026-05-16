from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Optional

from . import diskproto as dp
from . import fileproto as fp
from . import modemproto as mp
from . import netproto as np
from .analyze_capture import COMMAND_NAMES
from .byte_proto import read_lp_u16_str, read_u32le, read_u16le, read_u8
from .fujibus import (
    build_fuji_request_wire,
    build_fuji_response_wire,
    parse_fuji_packet,
    slip_decode,
)

_RECEIVE_RE = re.compile(
    r"receive:\s+id=\d+\s+dev=(0x[0-9A-Fa-f]+)\s+cmd=(0x[0-9A-Fa-f]+)\s+params=(\d+)\s+payload=(\d+)"
)
_SEND_RE = re.compile(
    r"send:\s+dev=(0x[0-9A-Fa-f]+)\s+status=(\d+)\s+cmd=(0x[0-9A-Fa-f]+)\s+payload=(\d+)"
)
_PAYLOAD_HDR_RE = re.compile(r"payload\s+\((\d+)\s+bytes\)")
_PARAM_LINE_RE = re.compile(r"\[(\d+)\]\s*=\s*0x([0-9A-Fa-f]+)")
_HEX_LINE_RE = re.compile(
    r"(?<![0-9a-fA-F])(?P<off>[0-9a-fA-F]{4}):\s+(?P<hex>(?:[0-9a-fA-F]{2}\s*)+)"
)


@dataclass
class PendingReceive:
    line_no: int
    device: int
    command: int
    param_count: int
    params: list[int]
    payload: bytes


@dataclass
class ExtractedMock:
    seq: int
    line_no: int
    device: int
    command: int
    request_payload: bytes
    response_payload: bytes
    request_wire: bytes
    response_wire: bytes
    response_status: int
    filename_base: str
    wire_warning: str | None = None


def _parse_int_hex(s: str) -> int:
    return int(s, 0)


def _op_slug(device: int, command: int) -> str:
    name = COMMAND_NAMES.get(device, {}).get(command)
    if not name:
        return f"dev{device:02x}_cmd{command:02x}"
    out: list[str] = []
    for ch in name:
        if ch.isupper() and out:
            out.append("_")
        out.append(ch.lower())
    return "".join(out)


def _safe_slug(text: str, *, max_len: int = 40) -> str:
    slug = re.sub(r"[^a-zA-Z0-9._-]+", "_", text).strip("._-")
    if not slug:
        return "x"
    return slug[:max_len]


def _uri_basename(uri: str) -> str:
    path = uri.split("?", 1)[0]
    if "/" in path:
        path = path.rsplit("/", 1)[-1]
    return path or "root"


def _infer_fujibus_param_sizes(
    device: int, command: int, values: list[int]
) -> list[int] | None:
    """Best-effort param widths when debug logs omit them."""
    if not values:
        return []
    if device == fp.FILE_DEVICE_ID and command == fp.CMD_LIST and len(values) == 2:
        return [2, 2]
    if device == np.NETWORK_DEVICE_ID and command == np.CMD_READ and len(values) == 2:
        return [1, 4]
    if device == np.NETWORK_DEVICE_ID and command == np.CMD_WRITE and len(values) == 2:
        return [1, 4]
    if len(values) == 1:
        if values[0] <= 0xFF:
            return [1]
        if values[0] <= 0xFFFF:
            return [2]
        return [4]
    if all(v <= 0xFF for v in values):
        return [1] * len(values)
    if all(v <= 0xFFFF for v in values):
        return [2] * len(values)
    if all(v <= 0xFFFFFFFF for v in values):
        return [4] * len(values)
    return None


def _request_filename_suffix(device: int, command: int, payload: bytes) -> str:
    if not payload:
        return ""

    try:
        if device == dp.DISK_DEVICE_ID:
            if command == dp.CMD_READ_SECTOR and len(payload) >= 8:
                off = 0
                _, off = read_u8(payload, off)
                _, off = read_u8(payload, off)
                lba, _ = read_u32le(payload, off)
                return str(lba)
            if command == dp.CMD_WRITE_SECTOR and len(payload) >= 8:
                off = 0
                _, off = read_u8(payload, off)
                _, off = read_u8(payload, off)
                lba, _ = read_u32le(payload, off)
                return str(lba)
            if command in (
                dp.CMD_INFO,
                dp.CMD_MOUNT,
                dp.CMD_UNMOUNT,
                dp.CMD_CLEAR_CHANGED,
            ) and len(payload) >= 2:
                return f"slot{payload[1]}"

        if device == fp.FILE_DEVICE_ID:
            if command == fp.CMD_RESOLVE_PATH:
                base_uri, arg = fp.parse_resolve_path_req(payload)
                base = _safe_slug(_uri_basename(base_uri) or base_uri[:40])
                arg_slug = _safe_slug(arg) if arg else "canonical"
                return f"{base}_{arg_slug}"
            off = 0
            _, off = read_u8(payload, off)
            uri, off = read_lp_u16_str(payload, off)
            base = _safe_slug(_uri_basename(uri))
            if command == fp.CMD_READ and len(payload) >= off + 6:
                offset, _ = read_u32le(payload, off)
                return f"{base}_{offset}"
            if command == fp.CMD_WRITE and len(payload) >= off + 6:
                offset, _ = read_u32le(payload, off)
                return f"{base}_{offset}"
            if command == fp.CMD_LIST and len(payload) >= off + 4:
                start, off = read_u16le(payload, off)
                max_entries, _ = read_u16le(payload, off)
                return f"{base}_start{start}_max{max_entries}"
            if command in (fp.CMD_STAT, fp.CMD_MAKE_DIRECTORY):
                return base

        if device == np.NETWORK_DEVICE_ID:
            if command == np.CMD_READ and len(payload) >= 11:
                off = 0
                _, off = read_u8(payload, off)
                handle, off = read_u8(payload, off)
                offset, _ = read_u32le(payload, off)
                return f"h{handle}_o{offset}"
            if command == np.CMD_WRITE and len(payload) >= 11:
                off = 0
                _, off = read_u8(payload, off)
                handle, off = read_u8(payload, off)
                offset, _ = read_u32le(payload, off)
                return f"h{handle}_o{offset}"
            if command == np.CMD_OPEN and len(payload) >= 3:
                off = 0
                _, off = read_u8(payload, off)
                _, off = read_u8(payload, off)
                _, off = read_u8(payload, off)
                url, _ = read_lp_u16_str(payload, off)
                return _safe_slug(_uri_basename(url) or url[:40])
            if command == np.CMD_INFO and len(payload) >= 2:
                return f"h{payload[1]}"

        if device == mp.MODEM_DEVICE_ID:
            if command == mp.CMD_READ and len(payload) >= 5:
                off = 0
                _, off = read_u8(payload, off)
                offset, _ = read_u32le(payload, off)
                return str(offset)
    except Exception:
        return ""

    return ""


def _filename_base(seq: int, device: int, command: int, request_payload: bytes) -> str:
    op = _op_slug(device, command)
    suffix = _request_filename_suffix(device, command, request_payload)
    if suffix:
        return f"{seq:03d}_{op}_{suffix}"
    return f"{seq:03d}_{op}"


def _parse_hex_line(line: str) -> list[int]:
    m = _HEX_LINE_RE.search(line)
    if not m:
        return []
    hex_part = m.group("hex").strip()
    if not hex_part:
        return []
    return [int(b, 16) for b in hex_part.split()]


def _collect_payload_lines(
    lines: list[str], start_idx: int, expected_len: int
) -> tuple[bytes, int]:
    data: list[int] = []
    i = start_idx
    while i < len(lines) and len(data) < expected_len:
        chunk = _parse_hex_line(lines[i])
        if chunk:
            need = expected_len - len(data)
            data.extend(chunk[:need])
        i += 1
    return bytes(data[:expected_len]), i


def _find_payload_header(lines: list[str], start_idx: int) -> tuple[int, int] | None:
    for i in range(start_idx, min(start_idx + 6, len(lines))):
        m = _PAYLOAD_HDR_RE.search(lines[i])
        if m:
            return int(m.group(1)), i + 1
    return None


def _collect_param_values(
    lines: list[str], start_idx: int, expected_count: int
) -> tuple[list[int], int]:
    values: list[int | None] = [None] * expected_count
    i = start_idx
    while i < len(lines) and any(v is None for v in values):
        m = _PARAM_LINE_RE.search(lines[i])
        if m:
            idx = int(m.group(1))
            if 0 <= idx < expected_count:
                values[idx] = int(m.group(2), 16)
        elif _PAYLOAD_HDR_RE.search(lines[i]) or _RECEIVE_RE.search(lines[i]) or _SEND_RE.search(lines[i]):
            break
        i += 1
    if any(v is None for v in values):
        return [], start_idx
    return [int(v) for v in values], i


def _build_request_wire(
    device: int, command: int, param_count: int, params: list[int], payload: bytes
) -> tuple[bytes, str | None]:
    if param_count == 0:
        return build_fuji_request_wire(device, command, payload), None
    if len(params) != param_count:
        return build_fuji_request_wire(device, command, payload), (
            f"request has params={param_count} but log lacks param values; "
            "wire frame omits FujiBus params"
        )
    sizes = _infer_fujibus_param_sizes(device, command, params)
    if sizes is None:
        return build_fuji_request_wire(device, command, payload), (
            "could not infer FujiBus param widths; wire frame omits params"
        )
    wire_params = [(v, s) for v, s in zip(params, sizes)]
    return build_fuji_request_wire(device, command, payload, params=wire_params), None


def iter_log_exchanges(
    path: Path,
) -> Iterator[tuple[PendingReceive | None, bytes, int, int]]:
    """
    Yield (pending_receive_or_none, response_payload, response_status, send_line_no).
    """
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    pending: PendingReceive | None = None
    i = 0
    while i < len(lines):
        line = lines[i]
        recv_m = _RECEIVE_RE.search(line)
        if recv_m:
            device = _parse_int_hex(recv_m.group(1))
            command = _parse_int_hex(recv_m.group(2))
            param_count = int(recv_m.group(3))
            expected_len = int(recv_m.group(4))
            params: list[int] = []
            payload = b""
            scan = i + 1
            if param_count > 0:
                params, scan = _collect_param_values(lines, scan, param_count)
            if expected_len > 0:
                hdr = _find_payload_header(lines, scan)
                if hdr is not None:
                    hdr_len, data_start = hdr
                    if hdr_len != expected_len:
                        expected_len = hdr_len
                    payload, scan = _collect_payload_lines(lines, data_start, expected_len)
            pending = PendingReceive(
                line_no=i + 1,
                device=device,
                command=command,
                param_count=param_count,
                params=params,
                payload=payload,
            )
            i = max(i + 1, scan)
            continue

        send_m = _SEND_RE.search(line)
        if send_m:
            send_line_no = i + 1
            status = int(send_m.group(2))
            expected_len = int(send_m.group(4))
            response = b""
            next_i = i + 1
            if expected_len > 0:
                hdr = _find_payload_header(lines, i + 1)
                if hdr is not None:
                    hdr_len, data_start = hdr
                    if hdr_len != expected_len:
                        expected_len = hdr_len
                    response, next_i = _collect_payload_lines(
                        lines, data_start, expected_len
                    )
            yield pending, response, status, send_line_no
            pending = None
            i = next_i
            continue

        i += 1


def extract_mocks_from_log(path: Path) -> list[ExtractedMock]:
    mocks: list[ExtractedMock] = []
    seq = 0
    for pending, response, status, line_no in iter_log_exchanges(path):
        if pending is None:
            continue
        seq += 1
        req_wire, req_warn = _build_request_wire(
            pending.device,
            pending.command,
            pending.param_count,
            pending.params,
            pending.payload,
        )
        resp_wire = build_fuji_response_wire(
            pending.device, pending.command, status, response
        )
        base = _filename_base(seq, pending.device, pending.command, pending.payload)
        mocks.append(
            ExtractedMock(
                seq=seq,
                line_no=line_no,
                device=pending.device,
                command=pending.command,
                request_payload=pending.payload,
                response_payload=response,
                request_wire=req_wire,
                response_wire=resp_wire,
                response_status=status,
                filename_base=base,
                wire_warning=req_warn,
            )
        )
    return mocks


def _matches_filter(
    device: int, command: int, args: argparse.Namespace
) -> bool:
    if args.device is not None and device != args.device:
        return False
    if args.command is not None and command != args.command:
        return False
    return True


def extract_log_mocks(args: argparse.Namespace) -> int:
    log_path = Path(args.log)
    out_dir = Path(args.output)
    if not log_path.is_file():
        print(f"error: log file not found: {log_path}")
        return 1

    mocks = extract_mocks_from_log(log_path)
    if args.device is not None or args.command is not None:
        mocks = [
            m
            for m in mocks
            if _matches_filter(m.device, m.command, args)
        ]

    if not mocks:
        print(f"no fujibus send responses found in {log_path}")
        return 1

    if not args.dry_run:
        out_dir.mkdir(parents=True, exist_ok=True)

    written = 0
    for mock in mocks:
        if args.inner_payload_only:
            req_name = f"{mock.filename_base}_req.bin"
            resp_name = f"{mock.filename_base}_resp.bin"
            req_data = mock.request_payload
            resp_data = mock.response_payload
        else:
            req_name = f"{mock.filename_base}_req.bin"
            resp_name = f"{mock.filename_base}_resp.bin"
            req_data = mock.request_wire
            resp_data = mock.response_wire

        print(
            f"{mock.seq:03d} line={mock.line_no} "
            f"dev=0x{mock.device:02X} cmd=0x{mock.command:02X} "
            f"req_inner={len(mock.request_payload)}B req_wire={len(mock.request_wire)}B "
            f"resp_inner={len(mock.response_payload)}B resp_wire={len(mock.response_wire)}B "
            f"status={mock.response_status} -> {req_name} {resp_name}"
        )
        if mock.wire_warning:
            print(f"      warn: {mock.wire_warning}")
        if args.dry_run:
            continue
        (out_dir / req_name).write_bytes(req_data)
        (out_dir / resp_name).write_bytes(resp_data)
        written += 2

    unit = "file(s)" if args.inner_payload_only else "wire frame file(s)"
    print(f"extracted {written} {unit} to {out_dir}")
    return 0


def register_subcommands(subparsers) -> None:
    pe = subparsers.add_parser(
        "extract-log-mocks",
        help="Extract fujibus request/response wire frames from a text log",
    )
    pe.add_argument("log", help="Path to fujinet text log (fujibus receive/send lines)")
    pe.add_argument(
        "-o",
        "--output",
        required=True,
        help="Directory to write extracted .bin mock files",
    )
    pe.add_argument(
        "--device",
        type=lambda s: int(s, 0),
        default=None,
        help="Only extract responses for this device id (e.g. 0xFC)",
    )
    pe.add_argument(
        "--command",
        type=lambda s: int(s, 0),
        default=None,
        help="Only extract responses for this command id (e.g. 0x03)",
    )
    pe.add_argument(
        "--inner-payload-only",
        action="store_true",
        help="Write device-protocol payloads only (no SLIP/FujiBus framing)",
    )
    pe.add_argument(
        "--dry-run",
        action="store_true",
        help="Print planned output files without writing them",
    )
    pe.set_defaults(fn=extract_log_mocks)


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Extract fujibus wire frames from text logs into binary mocks"
    )
    p.add_argument("log", help="Path to fujinet text log")
    p.add_argument(
        "-o",
        "--output",
        required=True,
        help="Directory to write extracted .bin mock files",
    )
    p.add_argument("--device", type=lambda s: int(s, 0), default=None)
    p.add_argument("--command", type=lambda s: int(s, 0), default=None)
    p.add_argument("--inner-payload-only", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    return p


def main() -> int:
    return extract_log_mocks(build_arg_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
