#!/usr/bin/env python3
from __future__ import annotations

import os
import pathlib


def main() -> int:
    step = pathlib.Path(os.environ["STEP_TMP"])
    step.mkdir(parents=True, exist_ok=True)
    sector_size = 512
    sector_count = 1760
    image = bytearray(sector_size * sector_count)
    for lba in range(sector_count):
        image[lba * sector_size:lba * sector_size + 2] = bytes((lba & 0xff, lba >> 8))
    (step / "amiga_test.ADF").write_bytes(image)
    (step / "write.bin").write_bytes(bytes([0xA5]) * sector_size)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
