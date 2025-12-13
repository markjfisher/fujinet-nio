from __future__ import annotations

from typing import Tuple


def u16le(x: int) -> bytes:
    return bytes((x & 0xFF, (x >> 8) & 0xFF))


def u32le(x: int) -> bytes:
    return bytes((
        x & 0xFF,
        (x >> 8) & 0xFF,
        (x >> 16) & 0xFF,
        (x >> 24) & 0xFF,
    ))


def u64le(x: int) -> bytes:
    return bytes((x >> (8 * i)) & 0xFF for i in range(8))


def read_u8(b: bytes, off: int) -> Tuple[int, int]:
    if off + 1 > len(b):
        raise ValueError("read_u8 out of bounds")
    return b[off], off + 1


def read_u16le(b: bytes, off: int) -> Tuple[int, int]:
    if off + 2 > len(b):
        raise ValueError("read_u16le out of bounds")
    return b[off] | (b[off + 1] << 8), off + 2


def read_u32le(b: bytes, off: int) -> Tuple[int, int]:
    if off + 4 > len(b):
        raise ValueError("read_u32le out of bounds")
    return (
        b[off]
        | (b[off + 1] << 8)
        | (b[off + 2] << 16)
        | (b[off + 3] << 24)
    ), off + 4


def read_u64le(b: bytes, off: int) -> Tuple[int, int]:
    if off + 8 > len(b):
        raise ValueError("read_u64le out of bounds")
    v = 0
    for i in range(8):
        v |= b[off + i] << (8 * i)
    return v, off + 8


def read_lp_u16_bytes(b: bytes, off: int) -> Tuple[bytes, int]:
    n, off = read_u16le(b, off)
    if off + n > len(b):
        raise ValueError("read_lp_u16_bytes out of bounds")
    return b[off:off + n], off + n


def read_lp_u16_str(b: bytes, off: int) -> Tuple[str, int]:
    raw, off = read_lp_u16_bytes(b, off)
    return raw.decode("utf-8", errors="replace"), off


