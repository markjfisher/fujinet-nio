from __future__ import annotations

from dataclasses import dataclass
from typing import List

from .byte_proto import (
    read_u8,
    read_u16le,
    read_u32le,
    read_u64le,
    u16le,
    u32le,
)

APPSTORE_VERSION = 1
APPSTORE_DEVICE_ID = 0xF1

CMD_STAT = 0x01
CMD_READ = 0x02
CMD_WRITE = 0x03
CMD_DELETE = 0x04
CMD_LIST = 0x05


def _lp_u16_bytes(value: bytes) -> bytes:
    if len(value) > 0xFFFF:
        raise ValueError("bytes too long for lp_u16")
    return u16le(len(value)) + value


def build_prefix(namespace: str, key: str = "") -> bytes:
    ns_b = namespace.encode("utf-8")
    key_b = key.encode("utf-8")
    if not (1 <= len(ns_b) <= 255):
        raise ValueError("namespace must be 1..255 bytes")
    if len(key_b) > 255:
        raise ValueError("key must be 0..255 bytes")
    return bytes([APPSTORE_VERSION]) + _lp_u16_bytes(ns_b) + _lp_u16_bytes(key_b)


def _check_key(key: str) -> None:
    if not (1 <= len(key.encode("utf-8")) <= 255):
        raise ValueError("key must be 1..255 bytes")


def build_stat_req(namespace: str, key: str) -> bytes:
    _check_key(key)
    return build_prefix(namespace, key)


def build_read_req(namespace: str, key: str, offset: int, max_bytes: int) -> bytes:
    _check_key(key)
    if not (0 <= offset <= 0xFFFFFFFF):
        raise ValueError("offset must fit u32")
    if not (1 <= max_bytes <= 0xFFFF):
        raise ValueError("max_bytes must fit u16 and be >0")
    return build_prefix(namespace, key) + u32le(offset) + u16le(max_bytes)


def build_write_req(namespace: str, key: str, offset: int, data: bytes) -> bytes:
    _check_key(key)
    if not (0 <= offset <= 0xFFFFFFFF):
        raise ValueError("offset must fit u32")
    if len(data) > 0xFFFF:
        raise ValueError("data chunk too large for u16 length; split it")
    return build_prefix(namespace, key) + u32le(offset) + u16le(len(data)) + data


def build_delete_req(namespace: str, key: str) -> bytes:
    _check_key(key)
    return build_prefix(namespace, key)


def build_list_req(namespace: str, start: int, max_payload_bytes: int) -> bytes:
    if not (0 <= start <= 0xFFFF):
        raise ValueError("start must fit u16")
    if not (1 <= max_payload_bytes <= 0xFFFF):
        raise ValueError("max_payload_bytes must fit u16 and be >0")
    return build_prefix(namespace) + u16le(start) + u16le(max_payload_bytes)


@dataclass
class StatResp:
    exists: bool
    size_bytes: int
    mtime_unix: int


@dataclass
class ReadResp:
    exists: bool
    eof: bool
    offset: int
    data: bytes


@dataclass
class WriteResp:
    offset: int
    written: int


@dataclass
class DeleteResp:
    deleted: bool


@dataclass
class ListResp:
    more: bool
    start_index: int
    key_count: int
    keys_len: int
    keys: List[str]


def _check_version(payload: bytes, off: int = 0) -> int:
    version, off = read_u8(payload, off)
    if version != APPSTORE_VERSION:
        raise ValueError(
            f"bad version {version}, expected {APPSTORE_VERSION}"
        )
    return off


def parse_stat_resp(payload: bytes) -> StatResp:
    off = _check_version(payload)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    size, off = read_u64le(payload, off)
    mtime, off = read_u64le(payload, off)
    return StatResp(bool(flags & 0x01), size, mtime)


def parse_read_resp(payload: bytes) -> ReadResp:
    off = _check_version(payload)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    offset, off = read_u32le(payload, off)
    data_len, off = read_u16le(payload, off)
    if off + data_len > len(payload):
        raise ValueError("appstore read data out of bounds")
    return ReadResp(
        exists=bool(flags & 0x02),
        eof=bool(flags & 0x01),
        offset=offset,
        data=payload[off : off + data_len],
    )


def parse_write_resp(payload: bytes) -> WriteResp:
    off = _check_version(payload)
    _flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    offset, off = read_u32le(payload, off)
    written, off = read_u16le(payload, off)
    return WriteResp(offset, written)


def parse_delete_resp(payload: bytes) -> DeleteResp:
    off = _check_version(payload)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    return DeleteResp(bool(flags & 0x01))


def parse_list_resp(payload: bytes) -> ListResp:
    off = _check_version(payload)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    start_index, off = read_u16le(payload, off)
    key_count, off = read_u16le(payload, off)
    keys_len, off = read_u16le(payload, off)
    keys_start = off
    keys: List[str] = []
    for _ in range(key_count):
        key_len, off = read_u16le(payload, off)
        if off + key_len > len(payload):
            raise ValueError("appstore key out of bounds")
        keys.append(payload[off : off + key_len].decode("utf-8", errors="replace"))
        off += key_len
    if off - keys_start != keys_len:
        raise ValueError(
            f"keysLen mismatch: parsed {off - keys_start}, header says {keys_len}"
        )
    return ListResp(bool(flags & 0x01), start_index, key_count, keys_len, keys)
