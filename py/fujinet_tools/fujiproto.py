from __future__ import annotations

from dataclasses import dataclass
from typing import List

from .byte_proto import read_u8, read_u16le, u16le

FUJI_DEVICE_ID = 0x70

CMD_GET_MOUNTS = 0xFD
CMD_SET_MOUNT = 0xFC
CMD_GET_MOUNT = 0xFB

GET_MOUNTS_VERSION = 1
GET_MOUNTS_FLAG_FORMATTED = 0x01
GET_MOUNTS_RESP_FLAG_MORE = 0x01
GET_MOUNTS_RESP_FLAG_FORMATTED = 0x02


@dataclass
class MountEntry:
    slot: int
    enabled: bool
    uri: str
    mode: str


@dataclass
class GetMountsResp:
    legacy: bool
    more: bool
    formatted: bool
    first_slot: int
    start_index: int
    entry_count: int
    entries_len: int
    entries: List[MountEntry]
    text: str


def build_get_mounts_req(
    *,
    flags: int = 0,
    first_slot: int = 0,
    last_slot: int = 0,
    start_index: int = 0,
    max_payload_bytes: int = 0,
) -> bytes:
    if flags == 0 and first_slot == 0 and last_slot == 0:
        return b""
    if not (0 <= flags <= 0xFF):
        raise ValueError("flags must fit u8")
    if not (0 <= first_slot <= 0xFFFF):
        raise ValueError("first_slot must fit u16")
    if not (0 <= last_slot <= 0xFFFF):
        raise ValueError("last_slot must fit u16")
    if not (0 <= start_index <= 0xFFFF):
        raise ValueError("start_index must fit u16")
    if not (1 <= max_payload_bytes <= 0xFFFF):
        raise ValueError("max_payload_bytes must fit u16 and be >0")
    return (
        bytes([flags & 0xFF])
        + u16le(first_slot)
        + u16le(last_slot)
        + u16le(start_index)
        + u16le(max_payload_bytes)
    )


def _parse_legacy_get_mounts_resp(payload: bytes) -> GetMountsResp:
    if not payload:
        raise ValueError("empty GetMounts response")

    legacy_count = payload[0]
    off = 1
    entries: List[MountEntry] = []
    for _ in range(legacy_count):
        slot_index, off = read_u8(payload, off)
        flags, off = read_u8(payload, off)
        uri_len, off = read_u8(payload, off)
        if off + uri_len > len(payload):
            raise ValueError("legacy uri out of bounds")
        uri = payload[off : off + uri_len].decode("utf-8", errors="replace")
        off += uri_len
        mode_len, off = read_u8(payload, off)
        if off + mode_len > len(payload):
            raise ValueError("legacy mode out of bounds")
        mode = payload[off : off + mode_len].decode("utf-8", errors="replace")
        off += mode_len
        entries.append(MountEntry(slot=slot_index, enabled=bool(flags & 0x01), uri=uri, mode=mode))

    if off != len(payload):
        raise ValueError("legacy GetMounts payload has trailing bytes")

    return GetMountsResp(
        legacy=True,
        more=False,
        formatted=False,
        first_slot=entries[0].slot if entries else 0,
        start_index=0,
        entry_count=len(entries),
        entries_len=0,
        entries=entries,
        text="",
    )


def _parse_extended_get_mounts_resp(payload: bytes) -> GetMountsResp:
    if not payload:
        raise ValueError("empty GetMounts response")

    off = 0
    version, off = read_u8(payload, off)
    if version != GET_MOUNTS_VERSION:
        raise ValueError(f"bad GetMounts version {version}, expected {GET_MOUNTS_VERSION}")
    flags, off = read_u8(payload, off)
    first_slot, off = read_u16le(payload, off)
    start_index, off = read_u16le(payload, off)
    entry_count, off = read_u16le(payload, off)
    entries_len, off = read_u16le(payload, off)

    more = bool(flags & GET_MOUNTS_RESP_FLAG_MORE)
    formatted = bool(flags & GET_MOUNTS_RESP_FLAG_FORMATTED)
    if formatted:
        if off + entries_len > len(payload):
            raise ValueError("formatted entries out of bounds")
        text = payload[off : off + entries_len].decode("utf-8", errors="replace")
        if off + entries_len != len(payload):
            raise ValueError("formatted GetMounts payload has trailing bytes")
        return GetMountsResp(
            legacy=False,
            more=more,
            formatted=True,
            first_slot=first_slot,
            start_index=start_index,
            entry_count=entry_count,
            entries_len=entries_len,
            entries=[],
            text=text,
        )

    entries: List[MountEntry] = []
    entries_end = off + entries_len
    if entries_end > len(payload):
        raise ValueError("binary entries out of bounds")
    for _ in range(entry_count):
        slot, off = read_u16le(payload, off)
        entry_flags, off = read_u8(payload, off)
        uri_len, off = read_u8(payload, off)
        if off + uri_len > len(payload):
            raise ValueError("uri out of bounds")
        uri = payload[off : off + uri_len].decode("utf-8", errors="replace")
        off += uri_len
        mode_len, off = read_u8(payload, off)
        if off + mode_len > len(payload):
            raise ValueError("mode out of bounds")
        mode = payload[off : off + mode_len].decode("utf-8", errors="replace")
        off += mode_len
        entries.append(MountEntry(slot=slot, enabled=bool(entry_flags & 0x01), uri=uri, mode=mode))

    if off != entries_end or entries_end != len(payload):
        raise ValueError("GetMounts payload has trailing bytes")

    return GetMountsResp(
        legacy=False,
        more=more,
        formatted=False,
        first_slot=first_slot,
        start_index=start_index,
        entry_count=entry_count,
        entries_len=entries_len,
        entries=entries,
        text="",
    )


def parse_get_mounts_resp(payload: bytes, *, expect_legacy: bool | None = None) -> GetMountsResp:
    if expect_legacy is True:
        return _parse_legacy_get_mounts_resp(payload)
    if expect_legacy is False:
        return _parse_extended_get_mounts_resp(payload)

    try:
        return _parse_extended_get_mounts_resp(payload)
    except ValueError:
        return _parse_legacy_get_mounts_resp(payload)
