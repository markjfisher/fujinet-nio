from __future__ import annotations

from dataclasses import dataclass
from typing import List, Tuple, Optional

from .byte_proto import (
    u16le,
    u32le,
    u64le,
    read_u8,
    read_u16le,
    read_u32le,
    read_u64le,
    read_lp_u16_bytes,
    read_lp_u16_str,
)

NETPROTO_VERSION = 1

# Wire device id for NetworkDevice protocol service
NETWORK_DEVICE_ID = 0xFD

# NetworkCommand (v1)
CMD_OPEN = 0x01
CMD_READ = 0x02
CMD_WRITE = 0x03
CMD_CLOSE = 0x04
CMD_INFO = 0x05


def _check_version(payload: bytes, off: int = 0) -> int:
    ver, off = read_u8(payload, off)
    if ver != NETPROTO_VERSION:
        raise ValueError(f"bad version {ver}, expected {NETPROTO_VERSION}")
    return off


def build_open_req(
    *,
    method: int,
    flags: int,
    url: str,
    headers: Optional[List[Tuple[str, str]]] = None,
    body_len_hint: int = 0,
    response_headers: Optional[List[str]] = None,
) -> bytes:
    """
    v1 (updated):
      u8  version
      u8  method
      u8  flags
      lp_u16 url
      u16 headerCount; repeated (lp_u16 key, lp_u16 value)
      u32 bodyLenHint
      u16 respHeaderCount; repeated lp_u16 name   <-- NEW (required)
    """
    if headers is None:
        headers = []
    if response_headers is None:
        response_headers = []

    url_b = url.encode("utf-8")
    if len(url_b) > 0xFFFF:
        raise ValueError("url too long")
    if not (1 <= method <= 5):
        raise ValueError("method must be 1..5")
    if not (0 <= flags <= 0xFF):
        raise ValueError("flags must fit u8")
    if not (0 <= body_len_hint <= 0xFFFFFFFF):
        raise ValueError("body_len_hint must fit u32")
    if len(response_headers) > 0xFFFF:
        raise ValueError("too many response headers")

    out = bytearray()
    out.append(NETPROTO_VERSION)
    out.append(method & 0xFF)
    out.append(flags & 0xFF)

    out += u16le(len(url_b))
    out += url_b

    out += u16le(len(headers))
    for k, v in headers:
        kb = k.encode("utf-8")
        vb = v.encode("utf-8")
        out += u16le(min(len(kb), 0xFFFF)) + kb[:0xFFFF]
        out += u16le(min(len(vb), 0xFFFF)) + vb[:0xFFFF]

    out += u32le(body_len_hint)

    # NEW: response header allowlist (store nothing if empty)
    out += u16le(len(response_headers))
    for name in response_headers:
        nb = name.encode("utf-8")
        out += u16le(min(len(nb), 0xFFFF)) + nb[:0xFFFF]

    return bytes(out)


def build_close_req(handle: int) -> bytes:
    if not (0 <= handle <= 0xFFFF):
        raise ValueError("handle must fit u16")
    return bytes([NETPROTO_VERSION]) + u16le(handle)


def build_info_req(handle: int) -> bytes:
    """
    v1 (updated):
      u8 version
      u16 handle
    """
    if not (0 <= handle <= 0xFFFF):
        raise ValueError("handle must fit u16")
    return bytes([NETPROTO_VERSION]) + u16le(handle)


def build_read_req(handle: int, offset: int, max_bytes: int) -> bytes:
    if not (0 <= handle <= 0xFFFF):
        raise ValueError("handle must fit u16")
    if not (0 <= offset <= 0xFFFFFFFF):
        raise ValueError("offset must fit u32")
    if not (0 <= max_bytes <= 0xFFFF):
        raise ValueError("max_bytes must fit u16")
    return bytes([NETPROTO_VERSION]) + u16le(handle) + u32le(offset) + u16le(max_bytes)


@dataclass
class OpenResp:
    accepted: bool
    needs_body_write: bool
    handle: int


def parse_open_resp(payload: bytes) -> OpenResp:
    off = _check_version(payload, 0)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    handle, off = read_u16le(payload, off)
    if off != len(payload):
        raise ValueError("trailing bytes in open response")
    return OpenResp(
        accepted=bool(flags & 0x01),
        needs_body_write=bool(flags & 0x02),
        handle=handle,
    )


@dataclass
class InfoResp:
    headers_included: bool
    has_content_length: bool
    has_http_status: bool
    handle: int
    http_status: Optional[int]
    content_length: Optional[int]
    header_bytes: bytes


def parse_info_resp(payload: bytes) -> InfoResp:
    off = _check_version(payload, 0)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    handle, off = read_u16le(payload, off)
    http_status, off = read_u16le(payload, off)
    content_length, off = read_u64le(payload, off)
    hdr_len, off = read_u16le(payload, off)
    if off + hdr_len > len(payload):
        raise ValueError("header bytes out of bounds")
    hdr = payload[off : off + hdr_len]
    off += hdr_len
    if off != len(payload):
        raise ValueError("trailing bytes in info response")

    has_len = bool(flags & 0x02)
    has_status = bool(flags & 0x04)

    return InfoResp(
        headers_included=bool(flags & 0x01),
        has_content_length=has_len,
        has_http_status=has_status,
        handle=handle,
        http_status=http_status if has_status else None,
        content_length=content_length if has_len else None,
        header_bytes=hdr,
    )


@dataclass
class WriteResp:
    handle: int
    offset: int
    written: int


def build_write_req(handle: int, offset: int, data: bytes) -> bytes:
    if not (0 <= handle <= 0xFFFF):
        raise ValueError("handle must fit u16")
    if not (0 <= offset <= 0xFFFFFFFF):
        raise ValueError("offset must fit u32")
    if len(data) > 0xFFFF:
        raise ValueError("data too large for u16 length; split it")
    return bytes([NETPROTO_VERSION]) + u16le(handle) + u32le(offset) + u16le(len(data)) + data


def parse_write_resp(payload: bytes) -> WriteResp:
    off = _check_version(payload, 0)
    _flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    handle, off = read_u16le(payload, off)
    offset, off = read_u32le(payload, off)
    written, off = read_u16le(payload, off)
    if off != len(payload):
        raise ValueError("trailing bytes in write response")
    return WriteResp(handle=handle, offset=offset, written=written)


@dataclass
class ReadResp:
    eof: bool
    truncated: bool
    handle: int
    offset: int
    data: bytes


def parse_read_resp(payload: bytes) -> ReadResp:
    off = _check_version(payload, 0)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    handle, off = read_u16le(payload, off)
    offset, off = read_u32le(payload, off)
    data_len, off = read_u16le(payload, off)
    if off + data_len > len(payload):
        raise ValueError("read data out of bounds")
    data = payload[off : off + data_len]
    off += data_len
    if off != len(payload):
        raise ValueError("trailing bytes in read response")
    return ReadResp(
        eof=bool(flags & 0x01),
        truncated=bool(flags & 0x02),
        handle=handle,
        offset=offset,
        data=data,
    )
