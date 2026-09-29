#!/usr/bin/env python3
"""Names addresses of the Xbox build with its linker map (build/xbox/halo.map,
which lld writes in the MSVC style; the XBE is loaded where the PE was
linked, so the map's addresses are the console's):

    python tools/xbox_symbolize.py 2429c1 00220000 ...
"""

import bisect
import re
import sys
from pathlib import Path

MAP = Path("build/xbox/halo.map")
LINE = re.compile(r"^\s*(\d{4}):([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{16})\s+(\S.*)$")


def main() -> int:
    symbols = []
    for line in MAP.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LINE.match(line)
        if match and match.group(1) == "0001":
            symbols.append((int(match.group(4), 16), match.group(3), match.group(5).strip()))
    symbols.sort()
    addresses = [symbol[0] for symbol in symbols]
    for text in sys.argv[1:]:
        value = int(text, 16)
        index = bisect.bisect_right(addresses, value) - 1
        if index >= 0 and value - symbols[index][0] < 0x4000:
            print(f"{value:08x} = {symbols[index][1]} + 0x{value - symbols[index][0]:x} ({symbols[index][2]})")
        else:
            print(f"{value:08x} = ?")
    return 0


if __name__ == "__main__":
    sys.exit(main())
