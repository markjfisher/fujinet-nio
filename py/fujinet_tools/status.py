from __future__ import annotations

# Must match fujinet::io::StatusCode (uint8)
STATUS_NAMES: dict[int, str] = {
    0: "Ok",
    1: "DeviceNotFound",
    2: "InvalidRequest",
    3: "DeviceBusy",
    4: "NotReady",
    5: "IOError",
    6: "Timeout",
    7: "InternalError",
    8: "Unsupported",
}

def status_name(code: int) -> str:
    return STATUS_NAMES.get(int(code), f"Unknown({code})")

def format_status(code: int) -> str:
    code_i = int(code)
    return f"{status_name(code_i)}({code_i})"
