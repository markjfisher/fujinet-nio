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
CMD_MKDIR = 0x05


def _lp_u16(s: str) -> bytes:
    """Length-prefixed u16 string (for URIs)."""
    b = s.encode("utf-8")
    if len(b) > 0xFFFF:
        raise ValueError("string too long for lp_u16")
    return u16le(len(b)) + b


def build_uri_request(uri: str) -> bytes:
    """
    Build the common request prefix for FileDevice v2+.

    New format (single URI):
    - u8 version
    - u16 uriLen (LE)
    - u8[] uri (full URI like "tnfs://host:port/path")
    """
    uri_b = uri.encode("utf-8")
    if not (1 <= len(uri_b) <= 65535):
        raise ValueError("uri must be 1..65535 bytes")

    return bytes([FILEPROTO_VERSION]) + _lp_u16(uri)


# -------- Requests --------


def build_stat_req(uri: str) -> bytes:
    """
    Build a stat request.

    Args:
        uri: Full URI (e.g., "tnfs://192.168.1.100:16384/file.txt", "sd0:/path/file")
    """
    return build_uri_request(uri)


def build_list_req(uri: str, start: int, max_entries: int) -> bytes:
    """
    Build a list directory request.

    Args:
        uri: Full URI for directory
        start: Starting index
        max_entries: Maximum entries to return
    """
    if not (0 <= start <= 0xFFFF):
        raise ValueError("start must fit u16")
    if not (1 <= max_entries <= 0xFFFF):
        raise ValueError("max_entries must fit u16 and be >0")
    return build_uri_request(uri) + u16le(start) + u16le(max_entries)


def build_read_req(uri: str, offset: int, max_bytes: int) -> bytes:
    """
    Build a read file request.

    Args:
        uri: Full URI for file
        offset: Byte offset to read from
        max_bytes: Maximum bytes to read
    """
    if not (0 <= offset <= 0xFFFFFFFF):
        raise ValueError("offset must fit u32")
    if not (1 <= max_bytes <= 0xFFFF):
        raise ValueError("max_bytes must fit u16 and be >0")
    return build_uri_request(uri) + u32le(offset) + u16le(max_bytes)


def build_write_req(uri: str, offset: int, data: bytes) -> bytes:
    """
    Build a write file request.

    Args:
        uri: Full URI for file
        offset: Byte offset to write at
        data: Data to write
    """
    if not (0 <= offset <= 0xFFFFFFFF):
        raise ValueError("offset must fit u32")
    if len(data) > 0xFFFF:
        raise ValueError("data chunk too large for u16 length; split it")
    return build_uri_request(uri) + u32le(offset) + u16le(len(data)) + data


def build_mkdir_req(*, uri: str, parents: bool = True, exist_ok: bool = True) -> bytes:
    """
    Build a make directory request.

    Args:
        uri: Full URI for directory
        parents: Create parent directories
        exist_ok: Don't error if directory exists
    """
    flags = 0
    if parents:
        flags |= 0x01
    if exist_ok:
        flags |= 0x02
    return build_uri_request(uri) + bytes([flags & 0xFF])


def parse_mkdir_resp(payload: bytes) -> None:
    off = 0
    off = _check_version(payload, off)
    # flags + reserved
    _flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)


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
        name = payload[off : off + name_len].decode("utf-8", errors="replace")
        off += name_len

        size, off = read_u64le(payload, off)
        mtime, off = read_u64le(payload, off)

        entries.append(
            ListEntry(
                is_dir=bool(eflags & 0x01),
                name=name,
                size_bytes=size,
                mtime_unix=mtime,
            )
        )

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
    data = payload[off : off + data_len]

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
    return (
        datetime.datetime.fromtimestamp(ts, tz=datetime.timezone.utc)
        .isoformat()
        .replace("+00:00", "Z")
    )
