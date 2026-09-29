#!/usr/bin/env python3
"""Fails if the console image uses an instruction the Pentium III lacks.

The console build does its single-precision arithmetic in SSE registers
(tools/xbox_build.py); the Pentium III has SSE but not SSE2, and an SSE2
instruction stops it (and the emulator, which models the same processor)
with an invalid-opcode fault. clang 19 is known to emit one for SSE1
targets in one case (llvm/llvm-project#134607); this disassembles the
linked image and lists any.

usage: xbox_sse2_check.py IMAGE [STAMP]
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

# SSE (and MMX) instructions on XMM registers the Pentium III has
SSE1 = {
    "movss", "movaps", "movups", "movlps", "movhps", "movlhps", "movhlps",
    "addss", "subss", "mulss", "divss", "sqrtss", "maxss", "minss", "rcpss", "rsqrtss",
    "addps", "subps", "mulps", "divps", "sqrtps", "maxps", "minps", "rcpps", "rsqrtps",
    "comiss", "ucomiss", "cvtsi2ss", "cvtss2si", "cvttss2si", "cvtsi2ssl", "cvtss2sil", "cvttss2sil",
    "cvtpi2ps", "cvtps2pi", "cvttps2pi",
    "andps", "andnps", "orps", "xorps", "shufps", "unpcklps", "unpckhps", "movmskps", "movntps",
}
# the compare forms: cmpeqss, cmpltps and so on
SSE1_COMPARE = re.compile(r"^cmp(eq|lt|le|unord|neq|nlt|nle|ord)?(ss|ps)$")
# SSE2 instructions that name no XMM register
SSE2_PLAIN = {"movnti", "clflush", "lfence", "mfence", "paddq", "psubq", "pmuludq"}

LINE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )*\s*([a-z][a-z0-9.]*)\s*(.*)$")


def objdump() -> str:
    for name in ("llvm-objdump", "llvm-objdump-19", "llvm-objdump-20", "objdump"):
        path = shutil.which(name)
        if path:
            return path
    sys.exit("xbox_sse2_check: no objdump in the PATH")


def main() -> None:
    image = Path(sys.argv[1])
    listing = subprocess.run([objdump(), "-d", "--no-show-raw-insn", str(image)],
                             check=True, capture_output=True, text=True).stdout
    found = []
    for line in listing.splitlines():
        match = LINE.match(line)
        if not match:
            continue
        address, mnemonic, operands = match.groups()
        if mnemonic in SSE2_PLAIN or (
                "%xmm" in operands and mnemonic not in SSE1 and not SSE1_COMPARE.match(mnemonic)):
            found.append(f"  {address}: {mnemonic} {operands}")
    if found:
        print(f"{image}: {len(found)} instruction(s) the Pentium III does not have (SSE2):")
        print("\n".join(found[:40]))
        sys.exit(1)
    if len(sys.argv) > 2:
        Path(sys.argv[2]).write_text("no SSE2\n")


if __name__ == "__main__":
    main()
