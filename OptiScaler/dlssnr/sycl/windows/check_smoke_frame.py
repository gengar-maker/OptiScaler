"""Check an RGBA16F SYCL output for size, finite values and non-black RGB."""

from __future__ import annotations

import argparse
import math
import struct
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=320)
    args = parser.parse_args()
    expected = args.width * args.height * 8
    data = args.image.read_bytes()
    if len(data) != expected:
        raise ValueError(f"expected {expected} bytes, got {len(data)}")
    rgb_sum = 0.0
    rgb_min = math.inf
    rgb_max = -math.inf
    for pixel in struct.iter_unpack("<4e", data):
        if not all(math.isfinite(value) for value in pixel):
            raise ValueError("output contains NaN or infinity")
        for value in pixel[:3]:
            rgb_sum += value
            rgb_min = min(rgb_min, value)
            rgb_max = max(rgb_max, value)
    rgb_mean = rgb_sum / (args.width * args.height * 3)
    print(f"RGB min={rgb_min:.6f} mean={rgb_mean:.6f} max={rgb_max:.6f}")
    if rgb_mean <= 0.01 or rgb_max <= rgb_min:
        raise ValueError("output is black or constant")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
