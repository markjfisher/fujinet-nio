#!/usr/bin/env python3
import argparse
import serial

SLIP_END     = 0xC0
SLIP_ESCAPE  = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

HEADER_SIZE = 6  # device(1), command(1), length(2), checksum(1), descr(1)

# Descriptor tables (same as C++ FujiBusPacket)
FIELD_SIZE_TABLE = [0, 1, 1, 1, 1, 2, 2, 4]
NUM_FIELDS_TABLE = [0, 1, 2, 3, 4, 1, 2, 1]


def calc_checksum(data: bytes) -> int:
    """
    Same checksum as in FujiBusPacket::calcChecksum:
      - 16-bit accumulator
      - fold carry each step
      - result is low 8 bits
    """
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


def extract_first_slip_frame(data: bytes) -> bytes | None:
    """Return first SLIP frame (including END bytes) or None."""
    try:
        start = data.index(SLIP_END)
    except ValueError:
        return None
    try:
        end = data.index(SLIP_END, start + 1)
    except ValueError:
        return None
    return data[start:end + 1]


def slip_decode(frame: bytes) -> bytes:
    """Decode a single SLIP frame (including leading/trailing END)."""
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
            # else: malformed escape, ignore
        else:
            out.append(b)
        i += 1
    return bytes(out)


def parse_fuji_packet(decoded: bytes) -> dict | None:
    """
    Parse a FujiBus packet (decoded SLIP payload).
    Returns dict with header, params, payload, checksum_ok, etc.
    """
    if len(decoded) < HEADER_SIZE:
        return None

    device   = decoded[0]
    command  = decoded[1]
    length   = decoded[2] | (decoded[3] << 8)
    checksum = decoded[4]
    descr    = decoded[5]

    if length != len(decoded):
        # Length mismatch: probably corrupt or mis-framed
        return None

    # Verify checksum: recompute with checksum byte zeroed
    tmp = bytearray(decoded)
    tmp[4] = 0
    computed = calc_checksum(tmp)
    checksum_ok = (computed == checksum)

    # Parse descriptors + params
    offset = HEADER_SIZE
    descr_bytes = [descr]

    # Additional descriptors when bit 7 is set
    while descr_bytes[-1] & 0x80:
        if offset >= len(decoded):
            return None
        next_descr = decoded[offset]
        offset += 1
        descr_bytes.append(next_descr)

    params: list[int] = []

    for dbyte in descr_bytes:
        field_desc = dbyte & 0x07  # low bits => field pattern index
        field_count = NUM_FIELDS_TABLE[field_desc]
        if field_count == 0:
            continue
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

    return {
        "device": device,
        "command": command,
        "length": length,
        "checksum": checksum,
        "checksum_computed": computed,
        "checksum_ok": checksum_ok,
        "descr": descr,
        "params": params,
        "payload": payload,
    }


def pretty_hex(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def pretty_ascii(data: bytes) -> str:
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data)


def print_packet(label: str, raw: bytes):
    print(f"\n=== {label} ===")
    if not raw:
        print("  (no data)")
        return

    print(f"Raw SLIP frame ({len(raw)} bytes):")
    print("  " + pretty_hex(raw))

    frame = extract_first_slip_frame(raw)
    if frame is None:
        print("  No complete SLIP frame found.")
        return

    print(f"First SLIP frame ({len(frame)} bytes):")
    print("  " + pretty_hex(frame))

    decoded = slip_decode(frame)
    print(f"Decoded FujiBus payload ({len(decoded)} bytes):")
    print("  " + pretty_hex(decoded))

    pkt = parse_fuji_packet(decoded)
    if pkt is None:
        print("  Failed to parse FujiBus header/structure.")
        return

    print("\n  Header:")
    print(f"    device   = 0x{pkt['device']:02X} ({pkt['device']})")
    print(f"    command  = 0x{pkt['command']:02X} ({pkt['command']})")
    print(f"    length   = 0x{pkt['length']:04X} ({pkt['length']})")
    print(f"    checksum = 0x{pkt['checksum']:02X} "
          f"(computed: 0x{pkt['checksum_computed']:02X}, "
          f"{'OK' if pkt['checksum_ok'] else 'MISMATCH'})")
    print(f"    descr    = 0x{pkt['descr']:02X}")

    if pkt["params"]:
        print("  Params:")
        for i, v in enumerate(pkt["params"]):
            print(f"    [{i}] = {v} (0x{v:08X})")
    else:
        print("  Params: (none)")

    payload = pkt["payload"]
    print(f"  Payload ({len(payload)} bytes):")
    if payload:
        print("    hex   :", pretty_hex(payload))
        print("    ascii :", repr(pretty_ascii(payload)))
    else:
        print("    (empty)")


def build_fuji_packet(device: int, command: int, payload: bytes) -> bytes:
    """
    Build a minimal FujiBus packet:
      header:
        uint8  device
        uint8  command
        uint16 length (little endian, header+payload)
        uint8  checksum (over full packet with checksum=0)
        uint8  descr (0 => no params)
      followed by payload bytes.
    """
    length = HEADER_SIZE + len(payload)

    pkt = bytearray(HEADER_SIZE + len(payload))
    pkt[0] = device & 0xFF
    pkt[1] = command & 0xFF
    pkt[2] = length & 0xFF        # length low
    pkt[3] = (length >> 8) & 0xFF # length high
    pkt[4] = 0                    # checksum placeholder
    pkt[5] = 0                    # descr = 0 (no params)

    pkt[HEADER_SIZE:] = payload

    checksum = calc_checksum(pkt)
    pkt[4] = checksum

    return slip_encode(pkt)

def send_fuji_command(
    port: str,
    device: int,
    command: int,
    payload: bytes,
    baud: int = 115200,
    timeout: float = 1.0,
    debug: bool = False,
) -> dict | None:
    """
    High-level helper to:
      - build a FujiBus+SLIP packet
      - send it over serial
      - read a response
      - decode and parse it

    Returns:
      dict as from parse_fuji_packet(), or None if no/invalid response.
    """

    packet = build_fuji_packet(device, command, payload)

    if debug:
        print_packet("Outgoing request", packet)

    with serial.Serial(port, baud, timeout=timeout) as ser:
        ser.write(packet)
        ser.flush()

        # Read some bytes back; you can tune this as needed
        resp = ser.read(512)

    if debug:
        print_packet("Incoming response", resp)

    if not resp:
        return None

    frame = extract_first_slip_frame(resp)
    if frame is None:
        return None

    decoded = slip_decode(frame)
    pkt = parse_fuji_packet(decoded)
    return pkt


def main():
    parser = argparse.ArgumentParser(description="Send a simple FujiBus+SLIP packet")
    parser.add_argument("--port", "-p", required=True,
                        help="Serial port (e.g. /dev/ttyACM1 or /dev/pts/8)")
    parser.add_argument("--baud", "-b", type=int, default=115200,
                        help="Baud rate (ignored for USB CDC, but needed by pyserial)")
    parser.add_argument("--device", "-d", type=int, default=1,
                        help="Fuji device ID (on-wire); 1 for your DummyDevice test")
    parser.add_argument("--command", "-c", type=int, default=0x01,
                        help="Fuji command byte")
    parser.add_argument("--payload", "-P", default="",
                        help="String payload to send (will be sent as raw bytes)")
    parser.add_argument("--read", "-r", action="store_true",
                        help="Read back and decode response after sending")
    parser.add_argument("--timeout", "-t", type=float, default=1.0,
                        help="Read timeout in seconds when --read is used")

    args = parser.parse_args()

    payload_bytes = args.payload.encode("ascii", errors="replace")
    packet = build_fuji_packet(args.device, args.command, payload_bytes)

    print_packet("Outgoing request", packet)

    with serial.Serial(args.port, args.baud, timeout=args.timeout) as ser:
        ser.write(packet)
        ser.flush()

        if args.read:
            resp = ser.read(256)  # read up to 256 bytes
            print_packet("Incoming response", resp)
        else:
            print("\n(no response read; --read not set)")


if __name__ == "__main__":
    main()
