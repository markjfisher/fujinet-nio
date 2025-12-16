# ✅ Replace file: py/fujinet_tools/fujibus.py
# Goals:
# - Keep existing FujiPacket parsing/printing
# - Add robust SLIP frame receive (incremental reads)
# - Avoid "single ser.read(read_max)" trap
# - Allow callers to reuse an already-open Serial instance

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, List, Union

import time
import serial

SLIP_END = 0xC0
SLIP_ESCAPE = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

HEADER_SIZE = 6  # device(1), command(1), length(2), checksum(1), descr(1)

FIELD_SIZE_TABLE = [0, 1, 1, 1, 1, 2, 2, 4]
NUM_FIELDS_TABLE = [0, 1, 2, 3, 4, 1, 2, 1]


@dataclass
class FujiPacket:
    device: int
    command: int
    length: int
    checksum: int
    checksum_computed: int
    checksum_ok: bool
    descr: int
    params: List[int]
    payload: bytes


def calc_checksum(data: bytes) -> int:
    chk = 0
    for b in data:
        chk += b
    chk = ((chk >> 8) + (chk & 0xFF)) & 0xFFFF
    return chk & 0xFF


def slip_encode(payload: bytes) -> bytes:
    out = bytearray()
    out.append(SLIP_END)
    for b in payload:
        if b == SLIP_END:
            out.append(SLIP_ESCAPE)
            out.append(SLIP_ESC_END)
        elif b == SLIP_ESCAPE:
            out.append(SLIP_ESCAPE)
            out.append(SLIP_ESC_ESC)
        else:
            out.append(b)
    out.append(SLIP_END)
    return bytes(out)


def slip_decode(frame: bytes) -> bytes:
    if not frame or frame[0] != SLIP_END or frame[-1] != SLIP_END:
        return b""
    out = bytearray()
    i = 1
    while i < len(frame) - 1:
        b = frame[i]
        if b == SLIP_ESCAPE:
            i += 1
            if i >= len(frame) - 1:
                break
            esc = frame[i]
            if esc == SLIP_ESC_END:
                out.append(SLIP_END)
            elif esc == SLIP_ESC_ESC:
                out.append(SLIP_ESCAPE)
            else:
                # unknown escape, keep original
                out.append(b)
        else:
            out.append(b)
        i += 1
    return bytes(out)


def extract_first_slip_frame(data: bytes) -> Optional[bytes]:
    try:
        start = data.index(SLIP_END)
    except ValueError:
        return None
    try:
        end = data.index(SLIP_END, start + 1)
    except ValueError:
        return None
    return data[start : end + 1]


def parse_fuji_packet(decoded: bytes) -> Optional[FujiPacket]:
    if len(decoded) < HEADER_SIZE:
        return None

    device = decoded[0]
    command = decoded[1]
    length = decoded[2] | (decoded[3] << 8)
    checksum = decoded[4]
    descr = decoded[5]

    if length != len(decoded):
        return None

    tmp = bytearray(decoded)
    tmp[4] = 0
    computed = calc_checksum(tmp)
    checksum_ok = computed == checksum

    offset = HEADER_SIZE

    # Descriptor can be "varint-like": continuation bit 0x80
    descr_bytes = [descr]
    while descr_bytes[-1] & 0x80:
        if offset >= len(decoded):
            return None
        descr_bytes.append(decoded[offset])
        offset += 1

    params: List[int] = []
    for dbyte in descr_bytes:
        field_desc = dbyte & 0x07
        field_count = NUM_FIELDS_TABLE[field_desc]
        field_size = FIELD_SIZE_TABLE[field_desc]
        for _ in range(field_count):
            if offset + field_size > len(decoded):
                return None
            v = 0
            for i in range(field_size):
                v |= decoded[offset + i] << (8 * i)
            params.append(v)
            offset += field_size

    payload = decoded[offset:]
    return FujiPacket(
        device=device,
        command=command,
        length=length,
        checksum=checksum,
        checksum_computed=computed,
        checksum_ok=checksum_ok,
        descr=descr,
        params=params,
        payload=payload,
    )


def build_fuji_packet(device: int, command: int, payload: bytes) -> bytes:
    length = HEADER_SIZE + len(payload)
    pkt = bytearray(length)
    pkt[0] = device & 0xFF
    pkt[1] = command & 0xFF
    pkt[2] = length & 0xFF
    pkt[3] = (length >> 8) & 0xFF
    pkt[4] = 0
    pkt[5] = 0
    pkt[HEADER_SIZE:] = payload
    pkt[4] = calc_checksum(pkt)
    return slip_encode(pkt)


def pretty_hex(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def pretty_ascii(data: bytes) -> str:
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data)


def print_packet(label: str, raw: bytes):
    print(f"\n=== {label} ===")
    if not raw:
        print(" (no data)")
        return None
    print(f"Raw data ({len(raw)} bytes):")
    print(" " + pretty_hex(raw))

    frame = extract_first_slip_frame(raw)
    if frame is None:
        print(" No complete SLIP frame found.")
        return None

    print(f"SLIP frame ({len(frame)} bytes):")
    print(" " + pretty_hex(frame))

    decoded = slip_decode(frame)
    print(f"Decoded FujiBus packet ({len(decoded)} bytes):")
    print(" " + pretty_hex(decoded))

    pkt = parse_fuji_packet(decoded)
    if pkt is None:
        print(" Failed to parse FujiBus header/structure.")
        return None

    print("\n Header:")
    print(f" device   = 0x{pkt.device:02X} ({pkt.device})")
    print(f" command  = 0x{pkt.command:02X} ({pkt.command})")
    print(f" length   = 0x{pkt.length:04X} ({pkt.length})")
    print(
        f" checksum = 0x{pkt.checksum:02X} "
        f"(computed: 0x{pkt.checksum_computed:02X}, "
        f"{'OK' if pkt.checksum_ok else 'MISMATCH'})"
    )
    print(f" descr    = 0x{pkt.descr:02X}")

    if pkt.params:
        print(" Params:")
        for i, v in enumerate(pkt.params):
            print(f"  [{i}] = {v} (0x{v:08X})")
    else:
        print(" Params: (none)")

    payload = pkt.payload
    print(f" Payload ({len(payload)} bytes):")
    if payload:
        print("  hex   :", pretty_hex(payload))
        print("  ascii :", repr(pretty_ascii(payload)))
    else:
        print("  (empty)")
    return pkt


def _read_one_slip_frame(
    ser: serial.Serial,
    deadline: float,
    read_chunk: int = 256,
    debug: bool = False,
) -> Optional[bytes]:
    """
    Incrementally read until we have a full SLIP frame (C0 ... C0) or deadline.
    Robust against frames split across multiple reads.
    """
    buf = bytearray()

    # state: have we seen the first END yet?
    in_frame = False

    # Use small per-iteration reads; rely on deadline for overall timeout.
    while time.monotonic() < deadline:
        # Prefer non-blocking-ish behaviour:
        # - if bytes waiting, drain them
        # - else read 1 byte with the port's timeout
        n_wait = ser.in_waiting
        n = n_wait if n_wait > 0 else 1
        n = min(n, read_chunk)

        chunk = ser.read(n)
        if not chunk:
            # nothing arrived in this serial timeout slice; loop until deadline
            continue

        buf.extend(chunk)

        # Find frame boundaries in buffer.
        # We want first C0 ... next C0.
        if not in_frame:
            try:
                start = buf.index(SLIP_END)
            except ValueError:
                # haven't seen start yet, keep buffering
                continue
            # discard anything before start
            if start > 0:
                del buf[:start]
            in_frame = True

        # Now we have a start at buf[0]. Look for end.
        try:
            end = buf.index(SLIP_END, 1)
        except ValueError:
            continue

        frame = bytes(buf[: end + 1])
        if debug:
            # include any trailing bytes too (useful for diagnosis)
            pass
        return frame

    return None


def send_command(
    port: Union[str, serial.Serial],
    device: int,
    command: int,
    payload: bytes,
    baud: int = 115200,
    timeout: float = 1.0,
    read_max: int = 4096,
    debug: bool = False,
) -> Optional[FujiPacket]:
    """
    If `port` is a string, opens/closes the serial port (legacy behaviour).
    If `port` is an existing serial.Serial, reuses it (preferred for performance).
    """
    packet = build_fuji_packet(device, command, payload)

    def _do(ser: serial.Serial) -> Optional[FujiPacket]:
        if debug:
            print_packet("Outgoing request", packet)

        ser.write(packet)
        ser.flush()

        # Overall deadline. We keep reading until we have a full frame.
        deadline = time.monotonic() + timeout
        frame = _read_one_slip_frame(ser, deadline=deadline, read_chunk=min(256, read_max), debug=debug)

        if frame is None:
            if debug:
                print("No complete SLIP frame before timeout")
            return None

        if debug:
            print_packet("Incoming raw data", frame)

        decoded = slip_decode(frame)
        pkt = parse_fuji_packet(decoded)
        if debug and pkt is not None:
            print_packet("Decoded response", frame)

        return pkt

    if isinstance(port, serial.Serial):
        return _do(port)

    # Legacy: open per call (still works, but slower)
    with serial.Serial(port, baud, timeout=0.01) as ser:
        return _do(ser)
