#!/usr/bin/env python3
"""Turn a Springboard ROM image into a C array for the firmware.

rom2c.py ROM OUTPUT.c        embed ROM
rom2c.py --erased OUTPUT.c   embed an erased (0xFF) placeholder instead
"""

import argparse
import sys
import zlib

# CS0 is only the ROM (include/sbnio.h), so the whole 64 KiB window is usable.
ROM_MIN, ROM_MAX = 512, 0x10000


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        usage="%(prog)s [--erased] [ROM] OUTPUT.c",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--erased", action="store_true", help="embed an erased image")
    parser.add_argument("paths", nargs="+", metavar="PATH", help="[ROM] OUTPUT.c")
    args = parser.parse_args()
    if len(args.paths) != (1 if args.erased else 2):
        parser.error("expected ROM OUTPUT.c, or --erased OUTPUT.c")
    *source, destination = args.paths
    data = b"\xff" * ROM_MIN if args.erased else open(source[0], "rb").read()
    if not ROM_MIN <= len(data) <= ROM_MAX or len(data) % 2:
        sys.exit(
            f"{source[0] if source else 'erased image'}: {len(data)} bytes; "
            f"a ROM must be {ROM_MIN}-{ROM_MAX} bytes and even"
        )
    with open(destination, "w") as out:
        out.write("#include <stdint.h>\n")
        out.write(f"const uint32_t sbnio_rom_size = {len(data)};\n")
        out.write(f"const uint32_t sbnio_rom_crc = 0x{zlib.crc32(data):08x};\n")
        out.write("const uint8_t sbnio_rom[] = {\n")
        for i in range(0, len(data), 16):
            out.write("  " + ", ".join(f"0x{b:02x}" for b in data[i : i + 16]) + ",\n")
        out.write("};\n")


if __name__ == "__main__":
    main()
