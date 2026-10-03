#!/usr/bin/env python3
"""Applies the Halo port's changes to nxdk's lwIP options
(nxdk/lib/net/nforceif/include/lwipopts.h): LWIP_SINGLE_NETIF 0 and
LWIP_HAVE_LOOPIF 1.

A local game's client joins its own server at 127.0.0.1 (split screen). With
a single netif lwIP routes nothing while the link is down, and it accepts a
looped-back packet only on a netif with an address, so split screen failed on
a console with no cable or no DHCP server (2026-10-01 audits). lwIP's own
loopback interface ('lo', 127.0.0.1/8, always up) carries such traffic
whatever the cable and the console's address; it needs more than one netif.
port/xbox/src/nx_net.c leaves 127.0.0.1 as it is for this (the console's own
address needs the link). The defines are not guarded in nxdk's file, so a -D
in tools/xbox_build.py's LWIP_OPTIONS cannot change them. The nxdk network
library and the game must then be rebuilt (port/xbox/README.md,
"Requirements").

Usage: xbox_lwip_patch.py NXDK_DIR
"""

import sys
from pathlib import Path

CHANGES = [
    ("#define LWIP_SINGLE_NETIF               1",
     "#define LWIP_SINGLE_NETIF               0 /* (halo-ce-universal: loopback without a link) */"),
    ("#define LWIP_HAVE_LOOPIF                0",
     "#define LWIP_HAVE_LOOPIF                1 /* (halo-ce-universal: 127.0.0.1 without a link or an address) */"),
]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    header = Path(sys.argv[1]) / "lib/net/nforceif/include/lwipopts.h"
    text = header.read_text(encoding="latin-1")
    changed = False
    for old, new in CHANGES:
        name = old.split()[1]
        if new in text:
            print(f"lwipopts.h: {name} already patched")
            continue
        if text.count(old) != 1:
            print(f"lwipopts.h: expected one {old!r}", file=sys.stderr)
            return 1
        text = text.replace(old, new)
        changed = True
        print(f"lwipopts.h: {name} patched")
    if changed:
        header.write_text(text, encoding="latin-1", newline="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
