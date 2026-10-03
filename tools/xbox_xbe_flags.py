#!/usr/bin/env python3
"""Clears the "limit to 64 MB" flag of an XBE's header.

nxdk's cxbe always sets it (XINIT_LIMIT_DEVKIT_MEMORY, bit 2 of the
initialization flags at offset 0x124), which makes the kernel keep the top
64 MB of a 128 MB console from the title. The Xbox build's 720p mode needs
that memory (port/xbox/src/nx_video.c); on a 64 MB console the flag changes
nothing, and the game still places its memory in the low 64 MB unless 720p
is on (nx_physical_alloc).

Usage: xbox_xbe_flags.py default.xbe
"""

import struct
import sys

INIT_FLAGS_OFFSET = 0x124
LIMIT_64MB = 0x4


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    path = sys.argv[1]
    with open(path, "r+b") as xbe:
        header = xbe.read(INIT_FLAGS_OFFSET + 4)
        if header[:4] != b"XBEH" or len(header) < INIT_FLAGS_OFFSET + 4:
            print(f"{path}: not an XBE", file=sys.stderr)
            return 1
        (flags,) = struct.unpack_from("<I", header, INIT_FLAGS_OFFSET)
        if flags & LIMIT_64MB:
            xbe.seek(INIT_FLAGS_OFFSET)
            xbe.write(struct.pack("<I", flags & ~LIMIT_64MB))
    return 0


if __name__ == "__main__":
    sys.exit(main())
