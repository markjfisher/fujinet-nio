# py/fujinet_tools/fileproto.py
from __future__ import annotations

from dataclasses import dataclass
from typing import List, Tuple, Optional
import datetime

from .byte_proto import (
    u16le,
    u32le,
    u64le,
    read_u8,
    read_u16le,
    read_u32le,
    read_u64le,
)

FILEPROTO_VERSION = 1

# Wire device id for FileDevice
FILE_DEVICE_ID = 0xFE

# FileCommand (matches C++)
CMD_STAT = 0x01
CMD_LIST = 0x02
CMD_READ = 0x03
CMD_WRITE = 0x04


def build_common_prefix(fs: str, path: str) -> bytes:
    fs_b = fs.encode("utf-8")
    path_b = path.encode("utf-8")

    if not (1 <= len(fs_b) <= 255):
        raise ValueError("fs name must be 1..255 bytes")
    if not (1 <= len(path_b) <= 65535):
        raise ValueError("path must be 1..65535 bytes")

    return bytes([FILEPROTO_VERSION, len(fs_b)]) + fs_b + u16le(len(path_b)) + path_b


# -------- Requests --------

def build_stat_req(fs: str, path: str) -> bytes:
    return build_common_prefix(fs, path)


def build_list_req(fs: str, path: str, start: int, max_entries: int) -> bytes:
    if not (0 <= start <= 0xFFFF):
        raise ValueError("start must fit u16")
    if not (1 <= max_entries <= 0xFFFF):
        raise ValueError("max_entries must fit u16 and be >0")
    return build_common_prefix(fs, path) + u16le(start) + u16le(max_entries)


def build_read_req(fs: str, path: str, offset: int, max_bytes: int) -> bytes:
    if not (0 <= offset <= 0xFFFFFFFF):
        raise ValueError("offset must fit u32")
    if not (1 <= max_bytes <= 0xFFFF):
        raise ValueError("max_bytes must fit u16 and be >0")
    return build_common_prefix(fs, path) + u32le(offset) + u16le(max_bytes)


def build_write_req(fs: str, path: str, offset: int, data: bytes) -> bytes:
    if not (0 <= offset <= 0xFFFFFFFF):
        raise ValueError("offset must fit u32")
    if len(data) > 0xFFFF:
        raise ValueError("data chunk too large for u16 length; split it")
    return build_common_prefix(fs, path) + u32le(offset) + u16le(len(data)) + data


# -------- Responses --------

@dataclass
class StatResp:
    exists: bool
    is_dir: bool
    size_bytes: int
    mtime_unix: int


@dataclass
class ListEntry:
    is_dir: bool
    name: str
    size_bytes: int
    mtime_unix: int


@dataclass
class ListResp:
    more: bool
    entries: List[ListEntry]


@dataclass
class ReadResp:
    eof: bool
    truncated: bool
    offset: int
    data: bytes


@dataclass
class WriteResp:
    offset: int
    written: int


def _check_version(b: bytes, off: int) -> int:
    ver, off = read_u8(b, off)
    if ver != FILEPROTO_VERSION:
        raise ValueError(f"bad version {ver}, expected {FILEPROTO_VERSION}")
    return off


def parse_stat_resp(payload: bytes) -> StatResp:
    off = 0
    off = _check_version(payload, off)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    size, off = read_u64le(payload, off)
    mtime, off = read_u64le(payload, off)

    is_dir = bool(flags & 0x01)
    exists = bool(flags & 0x02)
    return StatResp(exists=exists, is_dir=is_dir, size_bytes=size, mtime_unix=mtime)


def parse_list_resp(payload: bytes) -> ListResp:
    off = 0
    off = _check_version(payload, off)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    count, off = read_u16le(payload, off)

    more = bool(flags & 0x01)
    entries: List[ListEntry] = []

    for _ in range(count):
        eflags, off = read_u8(payload, off)
        name_len, off = read_u8(payload, off)
        if off + name_len > len(payload):
            raise ValueError("name out of bounds")
        name = payload[off:off + name_len].decode("utf-8", errors="replace")
        off += name_len

        size, off = read_u64le(payload, off)
        mtime, off = read_u64le(payload, off)

        entries.append(ListEntry(
            is_dir=bool(eflags & 0x01),
            name=name,
            size_bytes=size,
            mtime_unix=mtime,
        ))

    return ListResp(more=more, entries=entries)


def parse_read_resp(payload: bytes) -> ReadResp:
    off = 0
    off = _check_version(payload, off)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    offset, off = read_u32le(payload, off)
    data_len, off = read_u16le(payload, off)

    if off + data_len > len(payload):
        raise ValueError("read data out of bounds")
    data = payload[off:off + data_len]

    eof = bool(flags & 0x01)
    truncated = bool(flags & 0x02)
    return ReadResp(eof=eof, truncated=truncated, offset=offset, data=data)


def parse_write_resp(payload: bytes) -> WriteResp:
    off = 0
    off = _check_version(payload, off)
    _flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    offset, off = read_u32le(payload, off)
    written, off = read_u16le(payload, off)
    return WriteResp(offset=offset, written=written)


def fmt_utc(ts: int) -> str:
    if ts == 0:
        return "-"
    # Fix the deprecation you hit: use timezone-aware objects
    return datetime.datetime.fromtimestamp(ts, tz=datetime.timezone.utc).isoformat().replace("+00:00", "Z")
