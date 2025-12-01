#!/usr/bin/env python3
import argparse
import serial

SLIP_END     = 0xC0
SLIP_ESCAPE  = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

HEADER_SIZE = 6  # FujiBusHeader: device, command, length(2), checksum, descr


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

    # Compute checksum over entire packet with checksum field = 0
    checksum = calc_checksum(pkt)
    pkt[4] = checksum

    return slip_encode(pkt)


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
    parser.add_argument("--payload", "-P", default="hello",
                        help="String payload to send (will be sent as raw bytes)")
    parser.add_argument("--read", "-r", action="store_true",
                        help="Read back and dump response bytes after sending")
    parser.add_argument("--timeout", "-t", type=float, default=1.0,
                        help="Read timeout in seconds when --read is used")

    args = parser.parse_args()

    payload_bytes = args.payload.encode("ascii", errors="replace")
    packet = build_fuji_packet(args.device, args.command, payload_bytes)

    print(f"Sending {len(packet)} bytes on {args.port}:")
    print(" ".join(f"{b:02X}" for b in packet))

    with serial.Serial(args.port, args.baud, timeout=args.timeout) as ser:
        ser.write(packet)
        ser.flush()

        if args.read:
            resp = ser.read(256)  # read up to 256 bytes
            if resp:
                print(f"Received {len(resp)} bytes:")
                print(" ".join(f"{b:02X}" for b in resp))
            else:
                print("No response received (timeout).")


if __name__ == "__main__":
    main()
