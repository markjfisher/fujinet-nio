# py/fujinet_tools/diskproto.py
from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Tuple

from .byte_proto import (
    u16le,
    u32le,
    read_u8,
    read_u16le,
    read_u32le,
)

DISKPROTO_VERSION = 1

# Wire device id for DiskDevice protocol service
DISK_DEVICE_ID = 0xFC

# DiskCommand (v1; matches C++)
CMD_MOUNT = 0x01
CMD_UNMOUNT = 0x02
CMD_READ_SECTOR = 0x03
CMD_WRITE_SECTOR = 0x04
CMD_INFO = 0x05
CMD_CLEAR_CHANGED = 0x06


# ImageType (matches C++ disk::ImageType)
TYPE_AUTO = 0
TYPE_ATR = 1
TYPE_SSD = 2
TYPE_DSD = 3
TYPE_RAW = 4


def _lp_u16(s: str) -> bytes:
    b = s.encode("utf-8")
    if len(b) > 0xFFFF:
        raise ValueError("string too long for lp_u16")
    return u16le(len(b)) + b


def _check_version(payload: bytes, off: int = 0) -> int:
    ver, off = read_u8(payload, off)
    if ver != DISKPROTO_VERSION:
        raise ValueError(f"bad version {ver}, expected {DISKPROTO_VERSION}")
    return off


# -------- Requests --------

def build_mount_req(
    *,
    slot: int,
    fs: str,
    path: str,
    readonly: bool = False,
    type_override: int = TYPE_AUTO,
    sector_size_hint: int = 256,
) -> bytes:
    if not (1 <= slot <= 255):
        raise ValueError("slot must be 1..255")
    if not (0 <= type_override <= 255):
        raise ValueError("type_override must fit u8")
    if not (0 <= sector_size_hint <= 0xFFFF):
        raise ValueError("sector_size_hint must fit u16")

    flags = 0x01 if readonly else 0x00

    out = bytearray()
    out.append(DISKPROTO_VERSION)
    out.append(slot & 0xFF)
    out.append(flags & 0xFF)
    out.append(type_override & 0xFF)
    out += u16le(sector_size_hint)
    out += _lp_u16(fs)
    out += _lp_u16(path)
    return bytes(out)


def build_unmount_req(*, slot: int) -> bytes:
    if not (1 <= slot <= 255):
        raise ValueError("slot must be 1..255")
    return bytes([DISKPROTO_VERSION, slot & 0xFF])


def build_info_req(*, slot: int) -> bytes:
    if not (1 <= slot <= 255):
        raise ValueError("slot must be 1..255")
    return bytes([DISKPROTO_VERSION, slot & 0xFF])


def build_clear_changed_req(*, slot: int) -> bytes:
    if not (1 <= slot <= 255):
        raise ValueError("slot must be 1..255")
    return bytes([DISKPROTO_VERSION, slot & 0xFF])


def build_read_sector_req(*, slot: int, lba: int, max_bytes: int) -> bytes:
    if not (1 <= slot <= 255):
        raise ValueError("slot must be 1..255")
    if not (0 <= lba <= 0xFFFFFFFF):
        raise ValueError("lba must fit u32")
    if not (1 <= max_bytes <= 0xFFFF):
        raise ValueError("max_bytes must fit u16 and be >0")
    return bytes([DISKPROTO_VERSION, slot & 0xFF]) + u32le(lba) + u16le(max_bytes)


def build_write_sector_req(*, slot: int, lba: int, data: bytes) -> bytes:
    if not (1 <= slot <= 255):
        raise ValueError("slot must be 1..255")
    if not (0 <= lba <= 0xFFFFFFFF):
        raise ValueError("lba must fit u32")
    if len(data) > 0xFFFF:
        raise ValueError("data too large for u16 length")
    return bytes([DISKPROTO_VERSION, slot & 0xFF]) + u32le(lba) + u16le(len(data)) + data


# -------- Responses --------

@dataclass
class MountResp:
    mounted: bool
    readonly: bool
    slot: int
    img_type: int
    sector_size: int
    sector_count: int


@dataclass
class InfoResp:
    inserted: bool
    readonly: bool
    dirty: bool
    changed: bool
    slot: int
    img_type: int
    sector_size: int
    sector_count: int
    last_error: int


@dataclass
class ReadSectorResp:
    truncated: bool
    slot: int
    lba: int
    data: bytes


@dataclass
class WriteSectorResp:
    slot: int
    lba: int
    written_len: int


def parse_mount_resp(payload: bytes) -> MountResp:
    off = 0
    off = _check_version(payload, off)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    slot, off = read_u8(payload, off)
    img_type, off = read_u8(payload, off)
    sector_size, off = read_u16le(payload, off)
    sector_count, off = read_u32le(payload, off)

    return MountResp(
        mounted=bool(flags & 0x01),
        readonly=bool(flags & 0x02),
        slot=slot,
        img_type=img_type,
        sector_size=sector_size,
        sector_count=sector_count,
    )


def parse_info_resp(payload: bytes) -> InfoResp:
    off = 0
    off = _check_version(payload, off)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    slot, off = read_u8(payload, off)
    img_type, off = read_u8(payload, off)
    sector_size, off = read_u16le(payload, off)
    sector_count, off = read_u32le(payload, off)
    last_err, off = read_u8(payload, off)

    return InfoResp(
        inserted=bool(flags & 0x01),
        readonly=bool(flags & 0x02),
        dirty=bool(flags & 0x04),
        changed=bool(flags & 0x08),
        slot=slot,
        img_type=img_type,
        sector_size=sector_size,
        sector_count=sector_count,
        last_error=last_err,
    )


def parse_read_sector_resp(payload: bytes) -> ReadSectorResp:
    off = 0
    off = _check_version(payload, off)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    slot, off = read_u8(payload, off)
    lba, off = read_u32le(payload, off)
    data_len, off = read_u16le(payload, off)
    if off + data_len > len(payload):
        raise ValueError("read sector data out of bounds")
    data = payload[off : off + data_len]
    return ReadSectorResp(
        truncated=bool(flags & 0x01),
        slot=slot,
        lba=lba,
        data=data,
    )


def parse_write_sector_resp(payload: bytes) -> WriteSectorResp:
    off = 0
    off = _check_version(payload, off)
    _flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    slot, off = read_u8(payload, off)
    lba, off = read_u32le(payload, off)
    written_len, off = read_u16le(payload, off)
    return WriteSectorResp(slot=slot, lba=lba, written_len=written_len)


