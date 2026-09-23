"""Create a deterministic, non-black RGBA16F test frame for the SYCL DLL."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=320)
    args = parser.parse_args()
    if not 1 <= args.width <= 8192 or not 1 <= args.height <= 8192:
        parser.error("width and height must be 1..8192")
    with args.output.open("wb") as destination:
        for y in range(args.height):
            for x in range(args.width):
                destination.write(struct.pack(
                    "<4e",
                    x / max(1, args.width - 1),
                    y / max(1, args.height - 1),
                    0.15 + 0.7 * ((x // 16 + y // 16) & 1),
                    1.0,
                ))
    print(f"wrote {args.width}x{args.height} RGBA16F: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
