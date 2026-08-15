#!/usr/bin/env python3
"""Verify the COFF machine field of a PE image without external packages."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys


MACHINES = {
    "x86": 0x014C,
    "x86_64": 0x8664,
}


def pe_machine(path: Path) -> int:
    with path.open("rb") as image:
        dos_header = image.read(64)
        if len(dos_header) != 64 or dos_header[:2] != b"MZ":
            raise ValueError("missing DOS header")
        pe_offset = struct.unpack_from("<I", dos_header, 0x3C)[0]
        image.seek(pe_offset)
        pe_header = image.read(6)
        if len(pe_header) != 6 or pe_header[:4] != b"PE\0\0":
            raise ValueError("missing PE signature")
        return struct.unpack_from("<H", pe_header, 4)[0]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--machine", choices=tuple(MACHINES), required=True)
    parser.add_argument("image", type=Path)
    args = parser.parse_args(argv)
    try:
        actual = pe_machine(args.image)
    except (OSError, ValueError) as error:
        print(f"{args.image}: invalid PE image: {error}", file=sys.stderr)
        return 1
    expected = MACHINES[args.machine]
    if actual != expected:
        print(
            f"{args.image}: PE machine 0x{actual:04x} is not "
            f"{args.machine} (0x{expected:04x})",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
