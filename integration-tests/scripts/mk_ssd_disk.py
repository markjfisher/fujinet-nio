#!/usr/bin/env python3
from __future__ import annotations

import os
import pathlib


SECTOR_SIZE = 256
SECTOR_COUNT_40_TRACK = 400
SSD_HEADER_MIN_BYTES = 0x108
SSD_SECTOR_COUNT_OFF_HI = 0x106
SSD_SECTOR_COUNT_OFF_LO = 0x107


def make_minimal_ssd_image() -> bytearray:
    # Minimal DFS catalogue header spanning sectors 0 and 1.
    image = bytearray(SSD_HEADER_MIN_BYTES)
    image[0:5] = b"BLANK"
    image[SSD_SECTOR_COUNT_OFF_HI] = (SECTOR_COUNT_40_TRACK >> 8) & 0x03
    image[SSD_SECTOR_COUNT_OFF_LO] = SECTOR_COUNT_40_TRACK & 0xFF
    return image


def main() -> int:
    step = pathlib.Path(os.environ["STEP_TMP"])
    step.mkdir(parents=True, exist_ok=True)

    img = step / "test.ssd"

    img.write_bytes(make_minimal_ssd_image())

    # Sector-10 write payload (used by integration step)
    w = step / "w10.bin"
    w.write_bytes(bytes([0x5A]) * SECTOR_SIZE)

    print(str(img))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
