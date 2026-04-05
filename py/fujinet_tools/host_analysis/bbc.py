from __future__ import annotations

from dataclasses import dataclass
from string import printable

from .. import diskproto as dp
from typing import TYPE_CHECKING

from ..bbc_dfs import parse_dfs_catalogue_090
from .base import HostAnnotation

if TYPE_CHECKING:
    from ..analyze_capture import CapturedFrame


@dataclass
class _ReadSectorHit:
    frame_line_no: int
    frame_no: int | None
    slot: int
    lba: int
    data: bytes


def _is_catalog_textish(data: bytes) -> bool:
    title_bytes = data[0:8]
    name_area = data[8 : 8 + 8 * 8]
    sample = title_bytes + name_area
    printable_count = sum(
        chr(b) in printable and b not in b"\x0b\x0c\r\n\t" for b in sample
    )
    return printable_count >= max(8, len(sample) // 3)


def _parse_read_sector_hit(ev: CapturedFrame) -> _ReadSectorHit | None:
    if (
        ev.parse_status != "ok"
        or ev.device != dp.DISK_DEVICE_ID
        or ev.command != dp.CMD_READ_SECTOR
    ):
        return None
    try:
        payload = bytes.fromhex(ev.payload_hex) if ev.payload_hex else b""
        resp = dp.parse_read_sector_resp(payload)
    except Exception:
        return None
    if len(resp.data) < 256:
        return None
    return _ReadSectorHit(
        frame_line_no=ev.line_no,
        frame_no=ev.frame_no,
        slot=resp.slot,
        lba=resp.lba,
        data=resp.data,
    )


def _frame_distance(a: _ReadSectorHit, b: _ReadSectorHit) -> int:
    if a.frame_no is not None and b.frame_no is not None:
        return b.frame_no - a.frame_no
    return b.frame_line_no - a.frame_line_no


def _catalog_summary(entries) -> str:
    names = [entry.full_name for entry in entries[:8]]
    summary = ", ".join(names)
    if len(entries) > 8:
        summary += ", ..."
    return summary


def _validation_status(desc, entries) -> tuple[str, str, list[str]]:
    warnings: list[str] = []
    if desc.file_count != len(entries):
        warnings.append(
            f"parsed_files={len(entries)} descriptor_files={desc.file_count}"
        )
    if desc.boot_option not in {0, 1, 2, 3}:
        warnings.append(f"boot_option_out_of_range={desc.boot_option}")
    if desc.disc_sectors <= 0:
        warnings.append(f"disc_sectors={desc.disc_sectors}")
    if any(entry.start_sector <= 1 for entry in entries):
        warnings.append("entry_starts_in_catalogue_area")
    if any(entry.length <= 0 for entry in entries):
        warnings.append("entry_with_non_positive_length")
    if warnings:
        return (
            "warning",
            f"catalogue decoded with {len(warnings)} warning(s)",
            warnings,
        )
    return ("ok", "catalogue decoded cleanly", warnings)


def _entry_detail_lines(entries) -> list[str]:
    lines: list[str] = []
    for entry in entries[:6]:
        lock = " locked" if entry.locked else ""
        lines.append(
            "host_entry="
            f"{entry.full_name} load={entry.load_addr:05X} exec={entry.exec_addr:05X} "
            f"len={entry.length:05X} start={entry.start_sector:04X}{lock}"
        )
    if len(entries) > 6:
        lines.append(f"host_entry_more={len(entries) - 6}")
    return lines


def analyze_frames(frames: list[CapturedFrame]) -> dict[int, HostAnnotation]:
    annotations: dict[int, HostAnnotation] = {}
    pending_sector0: dict[int, _ReadSectorHit] = {}

    for ev in frames:
        hit = _parse_read_sector_hit(ev)
        if hit is None:
            continue

        if hit.lba == 0:
            extra_lines: list[str] = []
            confidence = "possible"
            if _is_catalog_textish(hit.data[:256]):
                extra_lines.append(
                    "host_note=sector0 looks catalogue-like before sector1 confirmation"
                )
                confidence = "candidate"
            annotations[ev.line_no] = HostAnnotation(
                frame_line_no=ev.line_no,
                host_hint="bbc_dfs_catalog_candidate",
                confidence=confidence,
                suppress_summary_hint=True,
                extra_lines=extra_lines,
            )
            pending_sector0[hit.slot] = hit
            continue

        if hit.lba != 1:
            continue

        first = pending_sector0.get(hit.slot)
        if first is None:
            continue

        distance = _frame_distance(first, hit)
        if distance < 1 or distance > 6:
            continue

        try:
            desc, entries = parse_dfs_catalogue_090(
                sector0=first.data[:256], sector1=hit.data[:256]
            )
        except Exception:
            continue

        decode = (
            f"title={desc.title!r} files={len(entries)} boot_option={desc.boot_option} "
            f"disc_sectors={desc.disc_sectors} cycle={desc.cycle_bcd}"
        )
        validation_status, validation_detail, warnings = _validation_status(
            desc, entries
        )
        extra_lines: list[str] = []
        if warnings:
            extra_lines.append(f"host_warnings={warnings}")
        catalog_summary = _catalog_summary(entries)
        if catalog_summary:
            extra_lines.append(f"host_catalog={catalog_summary}")
        extra_lines.extend(_entry_detail_lines(entries))
        annotations[ev.line_no] = HostAnnotation(
            frame_line_no=ev.line_no,
            host_hint="bbc_dfs_catalog_like",
            confidence="medium",
            host_decode=decode,
            validation_status=validation_status,
            validation_detail=validation_detail,
            suppress_payload_hints=True,
            suppress_summary_payload_hint=True,
            summary_suffix="host=bbc_dfs_catalog_like",
            extra_lines=extra_lines,
        )
        pending_sector0.pop(hit.slot, None)

    return annotations
