#!/usr/bin/env python3
from __future__ import annotations
from dataclasses import dataclass
from typing import List, Tuple, Optional
import struct

FILEPROTO_VERSION = 1

# Common prefix:
# u8  version
# u8  fsNameLen
# u8[] fsName
# u16 pathLen (LE)
# u8[] path
def build_common(fs: str, path: str) -> bytes:
    if not fs:
        raise ValueError("fs must not be empty")
    if not path:
        raise ValueError("path must not be empty")

    fs_b = fs.encode("utf-8")
    path_b = path.encode("utf-8")

    if len(fs_b) > 255:
        raise ValueError("fs name too long (>255)")
    if len(path_b) > 65535:
        raise ValueError("path too long (>65535)")

    return struct.pack("<BB", FILEPROTO_VERSION, len(fs_b)) + fs_b + struct.pack("<H", len(path_b)) + path_b


# --------------------
# Stat (0x01)
# --------------------
@dataclass
class StatResult:
    exists: bool
    is_dir: bool
    size_bytes: int
    mtime_unix: int

def build_stat(fs: str, path: str) -> bytes:
    return build_common(fs, path)

def parse_stat(payload: bytes) -> StatResult:
    # u8 version
    # u8 flags (bit0=isDir, bit1=exists)
    # u16 reserved
    # u64 sizeBytes
    # u64 modifiedUnixTime
    if len(payload) < 1 + 1 + 2 + 8 + 8:
        raise ValueError("stat response too short")

    ver, flags, _reserved = struct.unpack_from("<BBH", payload, 0)
    if ver != FILEPROTO_VERSION:
        raise ValueError(f"bad version {ver}")

    size, mtime = struct.unpack_from("<QQ", payload, 4)
    return StatResult(
        exists=bool(flags & 0x02),
        is_dir=bool(flags & 0x01),
        size_bytes=size,
        mtime_unix=mtime,
    )


# --------------------
# ListDirectory (0x02)
# --------------------
@dataclass
class DirEntry:
    name: str
    is_dir: bool
    size_bytes: int
    mtime_unix: int

@dataclass
class ListDirResult:
    more: bool
    entries: List[DirEntry]

def build_listdir(fs: str, path: str, start_index: int = 0, max_entries: int = 64) -> bytes:
    if not (0 <= start_index <= 65535):
        raise ValueError("start_index out of range")
    if not (1 <= max_entries <= 65535):
        raise ValueError("max_entries out of range")
    return build_common(fs, path) + struct.pack("<HH", start_index, max_entries)

def parse_listdir(payload: bytes) -> ListDirResult:
    # u8 version
    # u8 flags (bit0=more)
    # u16 reserved
    # u16 returnedCount
    # repeated entries:
    #   u8 eflags (bit0=isDir)
    #   u8 nameLen
    #   u8[] name
    #   u64 sizeBytes
    #   u64 modifiedUnixTime
    if len(payload) < 1 + 1 + 2 + 2:
        raise ValueError("listdir response too short")

    ver, flags, _res, count = struct.unpack_from("<BBHH", payload, 0)
    if ver != FILEPROTO_VERSION:
        raise ValueError(f"bad version {ver}")

    more = bool(flags & 0x01)
    off = 6
    entries: List[DirEntry] = []

    for _ in range(count):
        if off + 2 > len(payload):
            raise ValueError("truncated entry header")
        eflags = payload[off]
        name_len = payload[off + 1]
        off += 2

        if off + name_len + 8 + 8 > len(payload):
            raise ValueError("truncated entry body")

        name_b = payload[off:off + name_len]
        off += name_len

        size, mtime = struct.unpack_from("<QQ", payload, off)
        off += 16

        entries.append(
            DirEntry(
                name=name_b.decode("utf-8", errors="replace"),
                is_dir=bool(eflags & 0x01),
                size_bytes=size,
                mtime_unix=mtime,
            )
        )

    return ListDirResult(more=more, entries=entries)


# --------------------
# ReadFile (0x03)
# --------------------
@dataclass
class ReadResult:
    offset: int
    eof: bool
    truncated: bool
    data: bytes

def build_read(fs: str, path: str, offset: int = 0, max_bytes: int = 1024) -> bytes:
    if not (0 <= offset <= 0xFFFFFFFF):
        raise ValueError("offset out of range")
    if not (1 <= max_bytes <= 65535):
        raise ValueError("max_bytes out of range")
    return build_common(fs, path) + struct.pack("<IH", offset, max_bytes)

def parse_read(payload: bytes) -> ReadResult:
    # u8 version
    # u8 flags bit0=eof bit1=truncated
    # u16 reserved
    # u32 offset (echo)
    # u16 dataLen
    # u8[] data
    if len(payload) < 1 + 1 + 2 + 4 + 2:
        raise ValueError("read response too short")

    ver, flags, _res = struct.unpack_from("<BBH", payload, 0)
    if ver != FILEPROTO_VERSION:
        raise ValueError(f"bad version {ver}")

    offset = struct.unpack_from("<I", payload, 4)[0]
    data_len = struct.unpack_from("<H", payload, 8)[0]
    data = payload[10:10 + data_len]
    if len(data) != data_len:
        raise ValueError("truncated read data")

    return ReadResult(
        offset=offset,
        eof=bool(flags & 0x01),
        truncated=bool(flags & 0x02),
        data=data,
    )


# --------------------
# WriteFile (0x04)
# --------------------
@dataclass
class WriteResult:
    offset: int
    written_len: int

def build_write(fs: str, path: str, offset: int, data: bytes) -> bytes:
    if not (0 <= offset <= 0xFFFFFFFF):
        raise ValueError("offset out of range")
    if len(data) > 65535:
        raise ValueError("data too large for one packet (>65535)")
    return build_common(fs, path) + struct.pack("<IH", offset, len(data)) + data

def parse_write(payload: bytes) -> WriteResult:
    # u8 version
    # u8 flags
    # u16 reserved
    # u32 offset
    # u16 writtenLen
    if len(payload) < 1 + 1 + 2 + 4 + 2:
        raise ValueError("write response too short")

    ver = payload[0]
    if ver != FILEPROTO_VERSION:
        raise ValueError(f"bad version {ver}")

    offset = struct.unpack_from("<I", payload, 4)[0]
    written = struct.unpack_from("<H", payload, 8)[0]
    return WriteResult(offset=offset, written_len=written)
