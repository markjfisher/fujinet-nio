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
    return serial.Serial(
        port=port, baudrate=baud, timeout=timeout_s, write_timeout=max(1.0, timeout_s)
    )


def status_ok(pkt: Optional[FujiPacket]) -> bool:
    """
    Check if a FujiPacket indicates success (status code 0).

    FujiBus convention: status is param[0] on responses.
    """
    return bool(pkt and pkt.params and pkt.params[0] == 0)


# legacy code, not needed any more, but kept around in case we revert or need it again.
def build_uri(fs: str, path: str) -> str:
    """
    Build a full URI from fs and path components.

    Handles:
    - Full URIs in fs (e.g., "tnfs://host:port/")
    - Named filesystems (e.g., "sd0", "host")

    Examples:
        build_uri("tnfs://192.168.1.100:16384/", "/dir") -> "tnfs://192.168.1.100:16384/dir"
        build_uri("sd0", "/foo/bar") -> "sd0:/foo/bar"
        build_uri("host", "/foo/bar") -> "/foo/bar"
    """
    # Check if fs already contains a full URI
    if "://" in fs:
        # fs is already a complete URI
        if fs.endswith("/"):
            return fs + path.lstrip("/")
        else:
            if path.startswith("/"):
                return fs + path
            else:
                return fs + "/" + path
    elif fs == "host":
        # For host filesystem, path is already absolute
        return path
    else:
        # Construct URI from fs and path
        if path.startswith("/"):
            return f"{fs}:{path}"
        else:
            return f"{fs}:/{path}"
