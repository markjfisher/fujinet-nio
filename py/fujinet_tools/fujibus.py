from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass
from typing import Dict, List, Tuple, Optional, Union
import serial
import time

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
                # unknown escape, keep original escape byte
                out.append(b)
        else:
            out.append(b)
        i += 1
    return bytes(out)


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


def print_packet(label: str, raw: bytes, cmd_txt: str = "") -> Optional[FujiPacket]:
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
    print(f" command  = 0x{pkt.command:02X} ({pkt.command}) ({cmd_txt})")
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


def _extract_frame_from_rx(rx: bytearray) -> Optional[bytes]:
    """
    If rx contains a full SLIP frame (C0 ... C0), remove it from rx and return it.
    Otherwise return None.
    """
    try:
        start = rx.index(SLIP_END)
    except ValueError:
        rx.clear()
        return None

    if start > 0:
        del rx[:start]

    try:
        end = rx.index(SLIP_END, 1)
    except ValueError:
        return None

    frame = bytes(rx[: end + 1])
    del rx[: end + 1]
    return frame


class FujiBusSession:
    """
    Session that:
      - attaches to a serial port
      - reads SLIP frames incrementally (handles partial reads)
      - parses FujiPacket responses
      - stashes out-of-order packets keyed by (device, command)
    """
    def __init__(self):
        self._stash: Dict[Tuple[int, int], list[FujiPacket]] = defaultdict(list)
        self._ser: Optional[serial.Serial] = None
        self._rx: bytearray = bytearray()
        self._debug: bool = False

    def attach(self, ser: serial.Serial, *, debug: bool = False) -> "FujiBusSession":
        self._ser = ser
        self._debug = debug
        return self

    def stash(self, pkt: FujiPacket) -> None:
        self._stash[(pkt.device, pkt.command)].append(pkt)

    def pop(self, device: int, command: int) -> Optional[FujiPacket]:
        q = self._stash.get((device, command))
        if not q:
            return None
        pkt = q.pop(0)
        if not q:
            del self._stash[(device, command)]
        return pkt

    def send_command(self, device: int, command: int, payload: bytes, *, cmd_txt: str = "") -> None:
        if self._ser is None:
            raise RuntimeError("FujiBusSession is not attached to a serial port")

        pkt = build_fuji_packet(device, command, payload)
        if self._debug:
            label = f"Outgoing request{(' ' + cmd_txt) if cmd_txt else ''}"
            print_packet(label, pkt, cmd_txt=cmd_txt)

        self._ser.write(pkt)
        self._ser.flush()

    def send_command_expect(
        self,
        device: int,
        command: int,
        payload: bytes,
        *,
        expect_device: int,
        expect_command: int,
        timeout: float,
        cmd_txt: str = "",
    ) -> Optional[FujiPacket]:
        # return immediately if already stashed
        hit = self.pop(expect_device, expect_command)
        if hit is not None:
            return hit

        self.send_command(device, command, payload, cmd_txt=cmd_txt)

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            hit = self.pop(expect_device, expect_command)
            if hit is not None:
                return hit

            pkt = self._read_one_packet(deadline)
            if pkt is None:
                continue

            if pkt.device == expect_device and pkt.command == expect_command:
                return pkt

            self.stash(pkt)

        return None

    def _read_one_packet(self, deadline: float) -> Optional[FujiPacket]:
        frame = self._read_one_slip_frame(deadline)
        if frame is None:
            return None

        if self._debug:
            print_packet("Incoming raw data", frame)

        decoded = slip_decode(frame)
        if not decoded:
            if self._debug:
                print("[fujibus] Ignoring empty SLIP frame")
            return None

        pkt = parse_fuji_packet(decoded)
        if pkt is None:
            if self._debug:
                print("[fujibus] Ignoring non-parseable SLIP frame")
            return None

        if self._debug:
            print_packet("Decoded response", frame)

        return pkt

    def _read_one_slip_frame(self, deadline: float, read_chunk: int = 256) -> Optional[bytes]:
        if self._ser is None:
            raise RuntimeError("FujiBusSession is not attached to a serial port")

        while time.monotonic() < deadline:
            frame = _extract_frame_from_rx(self._rx)
            if frame is not None:
                return frame

            n_wait = getattr(self._ser, "in_waiting", 0) or 0
            n = min(max(1, n_wait), read_chunk)
            chunk = self._ser.read(n)
            if chunk:
                self._rx.extend(chunk)
                continue

        return None


# --- Convenience wrappers (no duplicate SLIP/parse logic) ---

def send_command(
    port: Union[str, serial.Serial],
    device: int,
    command: int,
    payload: bytes,
    baud: int = 115200,
    timeout: float = 1.0,
    debug: bool = False,
    cmd_txt: str = "",
) -> Optional[FujiPacket]:
    """
    Legacy helper:
      - if `port` is str: open/close per call
      - if `port` is serial.Serial: reuse it
    This function now uses FujiBusSession internally (no duplicate framing logic).
    """
    def _do(ser: serial.Serial) -> Optional[FujiPacket]:
        bus = FujiBusSession().attach(ser, debug=debug)
        # We expect the response to match the request's device+command.
        return bus.send_command_expect(
            device, command, payload,
            expect_device=device,
            expect_command=command,
            timeout=timeout,
            cmd_txt=cmd_txt,
        )

    if isinstance(port, serial.Serial):
        return _do(port)

    # Use consistent timeout settings (avoid circular import with common.open_serial)
    with serial.Serial(port=port, baudrate=baud, timeout=timeout, write_timeout=max(1.0, timeout)) as ser:
        return _do(ser)


def send_command_expect(
    port: Union[str, serial.Serial],
    device: int,
    command: int,
    payload: bytes,
    *,
    expect_device: int,
    expect_command: int,
    baud: int = 115200,
    timeout: float = 1.0,
    debug: bool = False,
    cmd_txt: str = "",
) -> Optional[FujiPacket]:
    """
    Convenience wrapper matching the session API, but usable from callers that
    still pass a port string.
    """
    def _do(ser: serial.Serial) -> Optional[FujiPacket]:
        bus = FujiBusSession().attach(ser, debug=debug)
        return bus.send_command_expect(
            device, command, payload,
            expect_device=expect_device,
            expect_command=expect_command,
            timeout=timeout,
            cmd_txt=cmd_txt,
        )

    if isinstance(port, serial.Serial):
        return _do(port)

    # Use consistent timeout settings (avoid circular import with common.open_serial)
    with serial.Serial(port=port, baudrate=baud, timeout=timeout, write_timeout=max(1.0, timeout)) as ser:
        return _do(ser)
