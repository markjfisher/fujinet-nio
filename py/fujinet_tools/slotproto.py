from __future__ import annotations

from dataclasses import dataclass

from .byte_proto import read_u8, read_u16le, u16le

SLOT_CATALOG_VERSION = 1
SLOT_CATALOG_DEVICE_ID = 0xF2

CMD_GET = 0x01
CMD_PUT = 0x02
CMD_DELETE = 0x03
CMD_RANGE = 0x04

REQUEST_TAIL_URI = 0x01
REQUEST_FORMATTED = 0x02
RESPONSE_MORE = 0x01
RESPONSE_FORMATTED = 0x02
ENTRY_VALID = 0x01
ENTRY_READ_ONLY = 0x02
ENTRY_URI_TRUNCATED = 0x04


@dataclass
class Entry:
    index: int
    flags: int
    uri: str


def build_get_req(index: int) -> bytes:
    return bytes([SLOT_CATALOG_VERSION, index & 0xFF])


def build_put_req(index: int, target: str, flags: int = 0) -> bytes:
    target_b = target.encode("utf-8")
    if not target_b or len(target_b) > 0xFFFF:
        raise ValueError("target must be 1..65535 bytes")
    return (
        bytes([SLOT_CATALOG_VERSION, index & 0xFF, flags & 0xFF])
        + u16le(len(target_b))
        + target_b
    )


def build_delete_req(index: int) -> bytes:
    return bytes([SLOT_CATALOG_VERSION, index & 0xFF])


def build_range_req(
    lower: int,
    upper: int,
    cursor: int,
    flags: int,
    max_uri_bytes: int,
    max_payload_bytes: int,
) -> bytes:
    return bytes(
        [
            SLOT_CATALOG_VERSION,
            lower & 0xFF,
            upper & 0xFF,
            cursor & 0xFF,
            flags & 0xFF,
            max_uri_bytes & 0xFF,
        ]
    ) + u16le(max_payload_bytes)


def parse_entry(payload: bytes) -> Entry:
    off = 0
    version, off = read_u8(payload, off)
    if version != SLOT_CATALOG_VERSION:
        raise ValueError(f"bad slot catalog version {version}")
    flags, off = read_u8(payload, off)
    index, off = read_u8(payload, off)
    uri_len, off = read_u16le(payload, off)
    if off + uri_len != len(payload):
        raise ValueError("slot URI length mismatch")
    return Entry(
        index=index,
        flags=flags,
        uri=payload[off : off + uri_len].decode("utf-8", errors="replace"),
    )
