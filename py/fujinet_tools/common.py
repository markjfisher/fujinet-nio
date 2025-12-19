# py/fujinet_tools/common.py
"""
Common utility functions shared across fujinet_tools modules.
"""
from __future__ import annotations

from typing import Optional

from .fujibus import FujiPacket

try:
    import serial  # type: ignore
except Exception:
    serial = None  # pyright: ignore


def open_serial(port: str, baud: int, timeout_s: float):
    """
    Open a serial port with consistent settings.
    
    Small per-read timeout; we enforce overall timeout ourselves.
    """
    if serial is None:
        raise RuntimeError("pyserial not available, cannot open serial port")
    return serial.Serial(port=port, baudrate=baud, timeout=timeout_s, write_timeout=max(1.0, timeout_s))


def status_ok(pkt: Optional[FujiPacket]) -> bool:
    """
    Check if a FujiPacket indicates success (status code 0).
    
    FujiBus convention: status is param[0] on responses.
    """
    return bool(pkt and pkt.params and pkt.params[0] == 0)

