#!/usr/bin/env python3
from __future__ import annotations

import os
import pathlib


def main() -> int:
    step = pathlib.Path(os.environ["STEP_TMP"])
    step.mkdir(parents=True, exist_ok=True)

    # 4 sectors of 256 bytes (small, deterministic)
    img = step / "raw_test.img"
    img_bytes = bytes([i & 0xFF for i in range(4 * 256)])
    img.write_bytes(img_bytes)

    # Sector-0 write payload (used by integration step)
    sec0 = step / "w0.bin"
    sec0.write_bytes(bytes([0xAA, 0x55]) + bytes([0x00] * (256 - 2)))

    print(str(img))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())