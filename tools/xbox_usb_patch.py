#!/usr/bin/env python3
"""Applies the Halo port's change to nxdk's USB host library
(nxdk/lib/usb/libusbohci/src_core/mem_alloc.c): endpoint and transfer
descriptors claimed atomically.

alloc_ohci_ED and alloc_ohci_TD test a slot of the descriptor pool and then
mark it used, with nothing between. The controllers' completion DPC submits
transfers (a gamepad's next read), and so does the game thread (rumble, a
read re-armed after an error in port/xbox/src/nx_input.c): a DPC between the
test and the store handed the same descriptor to two transfers, which
corrupts the controller's schedule (input stops, or the console hangs). The
claim is now one exchange, which a DPC cannot split, and the use count is
kept with atomic adds (2026-10-01 review). The library and the game must then
be rebuilt: delete lib/usb/libusbohci/src_core/mem_alloc.obj, lib/nxdk_usb.lib
and lib/libnxdk.lib (nxdk's makefile misses source changes under MSYS2) and
build as port/xbox/README.md, "Requirements", says.

Usage: xbox_usb_patch.py NXDK_DIR
"""

import sys
from pathlib import Path

CLAIM = ("        if (_unit_used[i] == 0)\n"
         "        {\n"
         "            _unit_used[i] = 1;\n"
         "            _mem_pool_used++;\n")
CLAIM_NEW = ("        /* (halo-ce-universal) claimed in one exchange: a DPC between a test\n"
             "        and a store gave two transfers the same descriptor */\n"
             "        if (_unit_used[i] == 0 && __atomic_exchange_n(&_unit_used[i], 1, __ATOMIC_SEQ_CST) == 0)\n"
             "        {\n"
             "            __atomic_add_fetch(&_mem_pool_used, 1, __ATOMIC_SEQ_CST);\n")
RELEASE = ("            _unit_used[i] = 0;\n"
           "            _mem_pool_used--;\n")
RELEASE_NEW = ("            __atomic_store_n(&_unit_used[i], 0, __ATOMIC_SEQ_CST);\n"
               "            __atomic_sub_fetch(&_mem_pool_used, 1, __ATOMIC_SEQ_CST);\n")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    source = Path(sys.argv[1]) / "lib/usb/libusbohci/src_core/mem_alloc.c"
    text = source.read_text(encoding="latin-1")
    if "(halo-ce-universal) claimed in one exchange" in text:
        print("mem_alloc.c: already patched")
        return 0
    nl = "\r\n" if "\r\n" in text else "\n"
    for old, new, wanted in ((CLAIM, CLAIM_NEW, 2), (RELEASE, RELEASE_NEW, None)):
        old = old.replace("\n", nl)
        new = new.replace("\n", nl)
        found = text.count(old)
        if (wanted is not None and found != wanted) or not found:
            print(f"mem_alloc.c: expected {wanted or 'some'} of {old!r}, found {found}", file=sys.stderr)
            return 1
        text = text.replace(old, new)
    source.write_text(text, encoding="latin-1", newline="")
    print("mem_alloc.c: patched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
