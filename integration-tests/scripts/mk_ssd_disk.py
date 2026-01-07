#!/usr/bin/env python3
from __future__ import annotations

import os
import pathlib


def main() -> int:
    step = pathlib.Path(os.environ["STEP_TMP"])
    step.mkdir(parents=True, exist_ok=True)

    img = step / "test.ssd"

    # 40-track SSD: 400 sectors * 256 bytes = 102400 bytes
    data = bytearray(400 * 256)

    # Put a recognizable pattern at the start of sector 0
    data[0:16] = bytes.fromhex("112233445566778899aabbccddeeff00")
    img.write_bytes(data)

    # Sector-10 write payload (used by integration step)
    w = step / "w10.bin"
    w.write_bytes(bytes([0x5A]) * 256)

    print(str(img))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


