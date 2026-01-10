from __future__ import annotations

from dataclasses import dataclass
from typing import Tuple

from .byte_proto import (
    u16le,
    u32le,
    read_u8,
    read_u16le,
    read_u32le,
)

MODEMPROTO_VERSION = 1

# Wire device id for ModemDevice protocol service
MODEM_DEVICE_ID = 0xFB

# ModemCommand (v1)
CMD_WRITE = 0x01
CMD_READ = 0x02
CMD_STATUS = 0x03
CMD_CONTROL = 0x04


def _check_version(payload: bytes, off: int = 0) -> int:
    ver, off = read_u8(payload, off)
    if ver != MODEMPROTO_VERSION:
        raise ValueError(f"bad version {ver}, expected {MODEMPROTO_VERSION}")
    return off


def build_write_req(*, offset: int, data: bytes) -> bytes:
    """
    v1:
      u8  version
      u32 offset
      u16 len
      u8[len] data
    """
    if not (0 <= offset <= 0xFFFFFFFF):
        raise ValueError("offset must fit u32")
    if len(data) > 0xFFFF:
        raise ValueError("data too long (max 65535)")
    return bytes([MODEMPROTO_VERSION]) + u32le(offset) + u16le(len(data)) + data


def build_read_req(*, offset: int, max_bytes: int) -> bytes:
    """
    v1:
      u8  version
      u32 offset
      u16 maxBytes
    """
    if not (0 <= offset <= 0xFFFFFFFF):
        raise ValueError("offset must fit u32")
    if not (0 <= max_bytes <= 0xFFFF):
        raise ValueError("max_bytes must fit u16")
    return bytes([MODEMPROTO_VERSION]) + u32le(offset) + u16le(max_bytes)


def build_status_req() -> bytes:
    return bytes([MODEMPROTO_VERSION])


def build_control_req(op: int, data: bytes = b"") -> bytes:
    """
    v1:
      u8 version
      u8 op
      op-specific bytes...
    """
    if not (0 <= op <= 0xFF):
        raise ValueError("op must fit u8")
    return bytes([MODEMPROTO_VERSION, op & 0xFF]) + data


@dataclass
class WriteResp:
    offset: int
    written: int


def parse_write_resp(payload: bytes) -> WriteResp:
    """
    v1:
      u8  version
      u8  flags
      u16 reserved
      u32 offset
      u16 written
    """
    off = _check_version(payload, 0)
    _flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    offset, off = read_u32le(payload, off)
    written, off = read_u16le(payload, off)
    if off != len(payload):
        raise ValueError("trailing bytes in write resp")
    return WriteResp(offset=offset, written=written)


@dataclass
class ReadResp:
    offset: int
    data: bytes


def parse_read_resp(payload: bytes) -> ReadResp:
    """
    v1:
      u8  version
      u8  flags
      u16 reserved
      u32 offset
      u16 len
      u8[len] data
    """
    off = _check_version(payload, 0)
    _flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    offset, off = read_u32le(payload, off)
    n, off = read_u16le(payload, off)
    if off + n > len(payload):
        raise ValueError("read resp out of bounds")
    data = payload[off:off + n]
    off += n
    if off != len(payload):
        raise ValueError("trailing bytes in read resp")
    return ReadResp(offset=offset, data=data)


@dataclass
class StatusResp:
    flags: int
    listen_port: int
    host_rx_avail: int
    host_write_cursor: int
    net_read_cursor: int
    net_write_cursor: int

    @property
    def cmd_mode(self) -> bool:
        return bool(self.flags & 0x01)

    @property
    def connected(self) -> bool:
        return bool(self.flags & 0x02)


def parse_status_resp(payload: bytes) -> StatusResp:
    """
    v1:
      u8  version
      u8  flags
      u16 reserved
      u16 listenPort
      u32 hostRxAvail
      u32 hostTxCursor
      u32 netRxCursor
      u32 netTxCursor
    """
    off = _check_version(payload, 0)
    flags, off = read_u8(payload, off)
    _reserved, off = read_u16le(payload, off)
    listen_port, off = read_u16le(payload, off)
    host_rx_avail, off = read_u32le(payload, off)
    host_write_cursor, off = read_u32le(payload, off)
    net_read_cursor, off = read_u32le(payload, off)
    net_write_cursor, off = read_u32le(payload, off)
    if off != len(payload):
        raise ValueError("trailing bytes in status resp")
    return StatusResp(
        flags=flags,
        listen_port=listen_port,
        host_rx_avail=host_rx_avail,
        host_write_cursor=host_write_cursor,
        net_read_cursor=net_read_cursor,
        net_write_cursor=net_write_cursor,
    )


def normalize_hostport(target: str) -> str:
    """
    Accept:
      - "tcp://host:port"
      - "host:port"
      - "host" (defaults to :23)
    Returns "host:port".
    """
    s = target.strip()
    if s.startswith("tcp://"):
        s = s[len("tcp://"):]
    if not s:
        raise ValueError("empty target")
    if ":" not in s:
        return s + ":23"
    return s


