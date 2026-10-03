#!/usr/bin/env python3
"""Fails if a linked image would not run on Windows 98 SE / Me or on a
Pentium III (the 9x build's, tools/win9x_build.py).

The 9x loader refuses an image it does not understand or that imports a
function its system does not export, and a function that is exported but
only stubbed (most W functions, the NT 4.0 additions) fails at run time;
the Pentium III stops at the first SSE2 instruction. None of this shows on
the Windows 10 or 11 machine that builds the game, so the linked image is
checked here, after every link:

 1. header: i386; GUI subsystem (console with --console, for tests);
    subsystem and system version at most 4.10 (98's own); pages aligned in
    the file (98 maps them from it); no relocations (fixed base); the
    stack reserve the game expects;
 2. TLS: no callbacks (the 9x loader never calls them);
 3. imports: only from the DLLs of port/win9x/port.json's import_libraries,
    only functions their port/win9x/lib/*.def list, by name, none delay-
    loaded; DLLs the build loads at run time (OpenGL, DirectSound,
    DirectInput, the shell's, the IP helper's) never imported; and a deny-
    list that holds even if a .def listed one by mistake (W functions but
    the few 9x implements, the NT 4.0 stubs, newer DLLs);
 4. instruction set: nothing a Pentium III lacks (SSE2, SSE3 and later,
    AVX, 3DNow!, BMI and the like), anywhere; nothing the first Pentium
    lacks (SSE, MMX, conditional moves and the Pentium Pro's other
    additions) in the start-up code and the processor check
    (port/win9x/src/w9x_start.c, w9x_cpucheck.c; the map names their
    functions) but the check's own probe, so that its message shows on any
    processor Windows 98 runs on; no ldmxcsr whose value is not shown to
    clear DAZ (a Pentium III without it faults);
 5. runtime: none of the UCRT's or vcruntime's names (stack cookies, SEH and
    C++ frame handlers, __acrt_*, __stdio_common_*) in the map, or, for an
    image without a map, none of the UCRT's own strings in it.

usage: win9x_check.py IMAGE [--map MAP] [--console] [--stack N] [--stamp STAMP]
"""

import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xbox_sse2_check  # noqa: E402  (its instruction tables and objdump)

ROOT = Path(__file__).resolve().parent.parent
PORT_CONFIG = ROOT / "port" / "win9x" / "port.json"
DEFS_DIR = ROOT / "port" / "win9x" / "lib"

# the newest subsystem and system version the Windows 98 loader takes
MAXIMUM_VERSION = (4, 10)

# W functions 9x implements (the rest of the W exports of KERNEL32, USER32
# and GDI32 fail with ERROR_CALL_NOT_IMPLEMENTED)
REAL_W_ON_9X = {
    "MultiByteToWideChar", "WideCharToMultiByte", "lstrlenW", "GetCommandLineW",
    "EnumResourceLanguagesW", "EnumResourceNamesW", "EnumResourceTypesW", "FindResourceW",
    "FindResourceExW", "MessageBoxW", "MessageBoxExW", "TextOutW", "ExtTextOutW",
    "GetCharWidthW", "GetTextExtentPointW", "GetTextExtentPoint32W", "GetTextExtentExPointW",
}
# NT 4.0 functions 9x exports as stubs only
STUBS_ON_9X = {
    "TryEnterCriticalSection", "SignalObjectAndWait", "SwitchToThread", "FindFirstFileExA", "FindFirstFileExW",
    "ReadDirectoryChangesW", "CancelIo", "CreateFiber", "ConvertThreadToFiber", "SwitchToFiber",
    "IsProcessorFeaturePresent",
}
# DLLs no 9x system has (or only with a redistributable that never shipped
# for 9x)
NEVER_ON_9X = re.compile(
    r"^(api-ms-win-.*|ext-ms-.*|ucrtbase.*|vcruntime\d+.*|msvcp\d+.*|bcrypt|ncrypt|dwmapi|uxtheme|"
    r"sdl\d*|dbghelp|powrprof|userenv|wtsapi32|dxgi|d3d1[012].*|d3d9|xinput.*|mfplat|avrt|propsys|normaliz|"
    r"bcryptprimitives|ntdll)\.dll$", re.I)

# instructions a Pentium III lacks that name no XMM register (SSE2's are
# xbox_sse2_check's SSE2_PLAIN)
LATER_PLAIN = {
    # SSE3 and later
    "fisttp", "fisttps", "fisttpl", "fisttpll", "monitor", "mwait", "lddqu",
    # bit manipulation, newer integer instructions
    "popcnt", "lzcnt", "andn", "bextr", "blsi", "blsmsk", "blsr", "bzhi", "pdep", "pext", "rorx", "sarx",
    "shlx", "shrx", "movbe", "crc32", "crc32b", "crc32w", "crc32l", "rdrand", "rdseed", "adcx", "adox", "xgetbv",
    "xsave", "xrstor", "xsaveopt", "clflushopt", "clwb", "rdtscp", "cmpxchg16b", "prefetchw", "prefetchwt1",
    # SSSE3 on MMX registers (with XMM ones they are caught as SSE2 or later)
    "pshufb", "phaddw", "phaddd", "phaddsw", "phsubw", "phsubd", "phsubsw", "pmaddubsw", "pmulhrsw", "psignb",
    "psignw", "psignd", "pabsb", "pabsw", "pabsd", "palignr",
    # 3DNow! (AMD's; its prefetch, 0F 0D, is not an instruction of the
    # Pentium III's)
    "prefetch", "femms", "pavgusb", "pf2id", "pf2iw", "pfacc", "pfadd", "pfcmpeq", "pfcmpge", "pfcmpgt", "pfmax", "pfmin",
    "pfmul", "pfnacc", "pfpnacc", "pfrcp", "pfrcpit1", "pfrcpit2", "pfrsqit1", "pfrsqrt", "pfsub", "pfsubr",
    "pi2fd", "pi2fw", "pmulhrw", "pswapd",
}
# (tzcnt is not here: its encoding is bsf's with a rep prefix, which a
# processor without BMI ignores, and LLVM writes it for a count of trailing
# zeros of a value that is never zero (dlmalloc's bin index), where the two
# agree; lzcnt's is bsr's, whose result differs, so it stays)

# SSE instructions that name no XMM register (the MXCSR, prefetches, the
# fence, and SSE's integer additions on MMX registers): none may run before
# the processor check has answered
SSE_PLAIN = {
    "ldmxcsr", "stmxcsr", "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "sfence", "movntq", "maskmovq",
    "pavgb", "pavgw", "pextrw", "pinsrw", "pmaxsw", "pmaxub", "pminsw", "pminub", "pmovmskb", "pmulhuw", "psadbw",
    "pshufw", "fxsave", "fxrstor",
}
# what the Pentium Pro and Pentium II added to the first Pentium, MMX
# aside (which names its registers, or is emms): conditional moves, the
# x87's compares into EFLAGS, the fast system call, the performance
# counters' read, and the long no-operation (0F 1F) compilers pad code with
PENTIUM_PRO = re.compile(r"^(cmov[a-z]+|fcmov[a-z]+|fu?comip?|fu?compi|sysenter|sysexit|rdpmc|ud1|ud0)$")
LONG_NOP = re.compile(r"^nop[wl]$")
# the start-up code's units, which run before the processor check answers
STARTUP_OBJECTS = {"w9x_start.o", "w9x_cpucheck.o"}
# the processor check's probe (port/win9x/src/w9x_cpucheck.c)
PROBE = ("xorps", "%xmm0, %xmm0")
PROBE_FUNCTION = "_sse_faults"

# names of the UCRT's and vcruntime's that must not be linked
RUNTIME_NAMES = re.compile(
    r"^_*(security_cookie|security_check_cookie|security_init_cookie|GSHandlerCheck|except_handler[234]|"
    r"except_handler4_common|CxxFrameHandler\d*|CxxThrowException|acrt_\w+|stdio_common_\w+|"
    r"conio_common_\w+|vcrt_\w+|std_terminate|C_specific_handler|local_stdio_\w+_options|"
    r"invalid_parameter\w*|scrt_\w+|isa_available\w*|report_gsfailure|guard_check_icall\w*|"
    r"dyn_tls_init_callback|dyn_tls_dtor_callback)(@\d+)?$")
# what the static UCRT carries when no map tells its names
UCRT_STRINGS = [
    ("api-ms-win-core".encode("utf-16-le"), "the UCRT's API-set lookups (api-ms-win-core-*)"),
    (b"CorExitProcess", "the UCRT's exit (CorExitProcess, mscoree.dll)"),
    (b"Microsoft Visual C++ Runtime Library", "the UCRT's error box"),
]


class Image:
    """a PE32 image's header, sections, directories, imports"""

    def __init__(self, path: Path) -> None:
        self.data = path.read_bytes()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE image")
        (self.machine, sections, _, _, _, optional_size,
         self.characteristics) = struct.unpack_from("<HHIIIHH", d, pe + 4)
        optional = pe + 24
        self.magic = struct.unpack_from("<H", d, optional)[0]
        if self.magic != 0x10B:
            raise ValueError("not PE32 (32-bit)")
        (self.image_base, self.section_alignment, self.file_alignment, self.os_major, self.os_minor,
         _, _, self.subsystem_major, self.subsystem_minor) = struct.unpack_from("<IIIHHHHHH", d, optional + 28)
        self.subsystem, self.dll_characteristics = struct.unpack_from("<HH", d, optional + 68)
        self.stack_reserve, self.stack_commit = struct.unpack_from("<II", d, optional + 72)
        directories = struct.unpack_from("<I", d, optional + 92)[0]
        self.directories = [struct.unpack_from("<II", d, optional + 96 + 8 * i) for i in range(min(directories, 16))]
        self.sections = []
        for i in range(sections):
            offset = optional + optional_size + 40 * i
            name = d[offset:offset + 8].rstrip(b"\0").decode("latin-1")
            virtual_size, address, raw_size, raw_pointer = struct.unpack_from("<IIII", d, offset + 8)
            self.sections.append((name, address, max(virtual_size, raw_size), raw_pointer, raw_size))

    def directory(self, index: int) -> Tuple[int, int]:
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def offset(self, rva: int) -> int:
        for _, address, size, pointer, _ in self.sections:
            if address <= rva < address + size:
                return pointer + rva - address
        raise ValueError(f"RVA {rva:#x} outside the sections")

    def string(self, rva: int) -> str:
        start = self.offset(rva)
        return self.data[start:self.data.index(b"\0", start)].decode("latin-1")

    def _thunks(self, rva: int):
        while True:
            value = struct.unpack_from("<I", self.data, self.offset(rva))[0]
            if not value:
                return
            yield ("#%d" % (value & 0xFFFF)) if value & 0x80000000 else self.string(value + 2)
            rva += 4

    def imports(self) -> List[Tuple[str, str]]:
        result = []
        rva, size = self.directory(1)
        while rva and size:
            lookup, _, _, name, address_table = struct.unpack_from("<IIIII", self.data, self.offset(rva))
            if not name:
                break
            dll = self.string(name)
            result += [(dll, function) for function in self._thunks(lookup or address_table)]
            rva += 20
        return result

    def delay_imports(self) -> List[Tuple[str, str]]:
        result = []
        rva, size = self.directory(13)
        while rva and size:
            attributes, name, _, _, names = struct.unpack_from("<IIIII", self.data, self.offset(rva))
            if not name:
                break
            base = 0 if attributes & 1 else self.image_base
            dll = self.string(name - base)
            result += [(dll, function) for function in self._thunks(names - base)]
            rva += 32
        return result

    def tls_callbacks(self) -> Optional[int]:
        """None without a TLS directory, else the number of callbacks"""
        rva, _ = self.directory(9)
        if not rva:
            return None
        callbacks = struct.unpack_from("<I", self.data, self.offset(rva) + 12)[0]
        if not callbacks:
            return 0
        count, start = 0, self.offset(callbacks - self.image_base)
        while struct.unpack_from("<I", self.data, start + 4 * count)[0]:
            count += 1
        return count

    def load_config_security_cookie(self) -> int:
        rva, size = self.directory(10)
        if not rva or size < 0x40:
            return 0
        structure = self.offset(rva)
        length = struct.unpack_from("<I", self.data, structure)[0]
        return struct.unpack_from("<I", self.data, structure + 0x3C)[0] if length >= 0x40 else 0


def read_definitions(directory: Path, libraries: List[str]) -> Dict[str, Set[str]]:
    """the import lists: DLL name (lower case) to its functions, from each
    .def's LIBRARY line and EXPORTS (stdcall's @N removed)"""
    lists: Dict[str, Set[str]] = {}
    for library in libraries:
        path = directory / f"{library}.def"
        dll, names, exports = f"{library}.dll", set(), False
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.split(";", 1)[0].strip()
            if not line:
                continue
            if line.upper().startswith("LIBRARY"):
                dll = line.split(None, 1)[1].strip().strip('"')
                continue
            if line.upper() == "EXPORTS":
                exports = True
                continue
            if exports:
                names.add(line.split()[0].split("@")[0])
        lists[dll.lower()] = names
    return lists


def read_map(path: Path) -> List[Tuple[int, str, str]]:
    """the map's code symbols: (address, name, object), sorted"""
    symbols = []
    pattern = re.compile(r"^\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]+)\s+(?:\S+:)?(\S+)\s*$", re.I)
    for line in path.read_text(encoding="latin-1").splitlines():
        match = pattern.match(line)
        if match:
            symbols.append((int(match.group(2), 16), match.group(1), match.group(3)))
    return sorted(set(symbols))


def map_names(path: Path) -> Set[str]:
    names = set()
    pattern = re.compile(r"^\s*[0-9a-f]{4}:[0-9a-f]+\s+(\S+)\s+[0-9a-f]+", re.I)
    for line in path.read_text(encoding="latin-1").splitlines():
        match = pattern.match(line)
        if match:
            names.add(match.group(1))
    return names


def disassemble(image: Path) -> List[Tuple[int, str, str]]:
    """(address, mnemonic, operands) of every instruction of the image"""
    listing = subprocess.run([xbox_sse2_check.objdump(), "-d", "--no-show-raw-insn", str(image)],
                             check=True, capture_output=True, text=True).stdout
    instructions = []
    for line in listing.splitlines():
        match = xbox_sse2_check.LINE.match(line)
        if match:
            address, mnemonic, operands = match.groups()
            # (without objdump's comments: "# imm = 0x1F80", the target's name)
            instructions.append((int(address, 16), mnemonic, operands.split("#", 1)[0].split(" <", 1)[0].strip()))
    return instructions


def is_sse(mnemonic: str, operands: str) -> bool:
    return ("%xmm" in operands or mnemonic in SSE_PLAIN or mnemonic in xbox_sse2_check.SSE1
            or bool(xbox_sse2_check.SSE1_COMPARE.match(mnemonic)))


def beyond_pentium(mnemonic: str, operands: str) -> Optional[str]:
    """what the first Pentium lacks of this instruction (SSE aside: is_sse),
    or None"""
    if PENTIUM_PRO.match(mnemonic):
        return "a Pentium Pro instruction"
    if LONG_NOP.match(mnemonic) and operands:
        return "the Pentium Pro's long no-operation"
    if re.search(r"%mm\d", operands) or mnemonic == "emms":
        return "MMX"
    return None


def beyond_pentium3(mnemonic: str, operands: str) -> Optional[str]:
    """what a Pentium III lacks of this instruction, or None"""
    base = mnemonic.split(".")[0]
    # (AT&T's size suffix: popcntl is popcnt)
    plain = {base, base[:-1]} if base[-1:] in ("b", "w", "l", "q") else {base}
    if "%ymm" in operands or "%zmm" in operands or re.search(r"%k\d", operands):
        return "AVX"
    if base.startswith("v") and "%xmm" in operands:
        return "AVX (VEX)"
    if plain & xbox_sse2_check.SSE2_PLAIN:
        return "SSE2"
    if plain & LATER_PLAIN:
        return "a later extension"
    if "%xmm" in operands and base not in xbox_sse2_check.SSE1 and not xbox_sse2_check.SSE1_COMPARE.match(base) \
            and base not in SSE_PLAIN:
        return "SSE2 or later"
    return None


def split_operands(operands: str) -> List[str]:
    """AT&T operands, split at the commas outside parentheses"""
    parts, depth, current = [], 0, ""
    for character in operands:
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        if character == "," and depth == 0:
            parts.append(current.strip())
            current = ""
        else:
            current += character
    if current.strip():
        parts.append(current.strip())
    return parts


# instructions that write EAX or EDX without naming them
IMPLICIT_EAX_EDX = re.compile(r"^(c[wl]td|cltq|cbtw|cwtl|cpuid|rdtsc|rdpmc|rdmsr|i?mul[bwl]?|i?div[bwl]?|lahf|xgetbv)$")
# instructions with one operand that do not write it
READS_ITS_OPERAND = re.compile(r"^(push[lw]?|call[l]?|jmp[l]?|j[a-z]+|ldmxcsr|fldcw|fld\w*|fi?com\w*|fi?mul\w*|"
                               r"fi?add\w*|fi?sub\w*|fi?div\w*|nop[wl]?|prefetch\w*|test\w*|cmp\w*|bt[lw]?|lea[lw]?)$")


def daz_clear(instructions: List[Tuple[int, str, str]], index: int, location: str,
              budget: int = 32) -> Optional[bool]:
    """Whether the value in location (a register or a memory operand, as
    objdump spells it) is shown to have DAZ (bit 6) clear when instruction
    index runs: followed back to the instruction that wrote it, which must
    be a mov of an immediate without the bit, an and with such an immediate,
    an and of two values one of which is shown clear, a stmxcsr (MXCSR never
    holds DAZ: every load is checked), or a mov from a location shown clear.
    Anything else that writes it (or, add, a call, a pop...), a jump or a
    return on the way back, or running out of budget: not shown (None).
    False: an immediate with the bit set reaches it."""
    for position in range(index - 1, max(-1, index - 1 - budget), -1):
        _, mnemonic, operands = instructions[position]
        if mnemonic in ("ret", "retl", "jmp", "jmpl", "calll", "call", "int3", "ud2"):
            return None
        parts = split_operands(operands)
        if IMPLICIT_EAX_EDX.match(mnemonic) and re.match(r"^%(e?[ad]x|[ad][lh])$", location):
            return None
        if not parts:
            continue
        if len(parts) == 1:
            if parts[0] == location and not READS_ITS_OPERAND.match(mnemonic):
                return True if mnemonic == "stmxcsr" else None
            continue
        destination, source = parts[-1], parts[0]
        if mnemonic.startswith(("xchg", "xadd", "cmpxchg")) and location in parts:
            return None
        if destination != location:
            continue
        if mnemonic.startswith(("cmp", "test", "bt")) and not mnemonic.startswith(("bts", "btr", "btc")):
            continue
        immediate = re.match(r"^\$(-?0x[0-9a-f]+|-?\d+)$", source)
        if mnemonic.startswith("mov") and not mnemonic.startswith(("movs", "movz")):
            if immediate:
                return not (int(immediate.group(1), 0) & 0x40)
            return daz_clear(instructions, position, source, budget - (index - position))
        if mnemonic.startswith("and"):
            if immediate:
                if not (int(immediate.group(1), 0) & 0x40):
                    return True
                continue
            remaining = budget - (index - position)
            if (daz_clear(instructions, position, source, remaining) or
                    daz_clear(instructions, position, location, remaining)):
                return True
            return None
        return None
    return None


def check_mxcsr_loads(instructions: List[Tuple[int, str, str]], errors: List[str]) -> int:
    """Each ldmxcsr must load a value shown to have DAZ (bit 6) clear
    (daz_clear: the value followed back through the instructions that wrote
    it). Returns the number of loads seen."""
    loads = 0
    for index, (address, mnemonic, operands) in enumerate(instructions):
        if mnemonic != "ldmxcsr":
            continue
        loads += 1
        shown = daz_clear(instructions, index, operands.strip())
        if shown is None:
            errors.append(f"ldmxcsr at {address:#x} loads a value not shown to clear DAZ (bit 6), which a "
                          "Pentium III without DAZ refuses with a fault")
        elif not shown:
            errors.append(f"ldmxcsr at {address:#x} sets DAZ (bit 6): a Pentium III without it faults")
    return loads


def main() -> int:
    parser = argparse.ArgumentParser(description="checks an image of the Windows 98 SE / Me build")
    parser.add_argument("image", type=Path)
    parser.add_argument("--map", type=Path, help="lld-link's map of the image (default: IMAGE with .map)")
    parser.add_argument("--console", action="store_true", help="a console program (the runtime's tests)")
    parser.add_argument("--stack", type=lambda text: int(text, 0), default=0x800000,
                        help="the stack reserve the image must have (default 8 MB, the game's)")
    parser.add_argument("--port-json", type=Path, default=PORT_CONFIG)
    parser.add_argument("--defs", type=Path, default=DEFS_DIR, help="the import lists' folder")
    parser.add_argument("--stamp", type=Path, help="written when the image passes")
    args = parser.parse_args()

    errors: List[str] = []
    notes: List[str] = []
    try:
        image = Image(args.image)
    except (OSError, ValueError, struct.error) as error:
        print(f"{args.image}: {error}")
        return 1
    config = json.loads(args.port_json.read_text(encoding="utf-8"))

    # ---------- 1. header
    if image.machine != 0x14C:
        errors.append(f"machine {image.machine:#x} is not i386")
    allowed_subsystems = {2: "GUI", 3: "console"} if args.console else {2: "GUI"}
    if image.subsystem not in allowed_subsystems:
        errors.append(f"subsystem {image.subsystem} (the game is a GUI program, subsystem 2)")
    if (image.subsystem_major, image.subsystem_minor) > MAXIMUM_VERSION:
        errors.append(f"subsystem version {image.subsystem_major}.{image.subsystem_minor}: the 98 loader takes "
                      f"{MAXIMUM_VERSION[0]}.{MAXIMUM_VERSION[1]} at most (lld-link -subsystem:windows,4.0)")
    if (image.os_major, image.os_minor) > MAXIMUM_VERSION:
        errors.append(f"operating system version {image.os_major}.{image.os_minor}: 4.0 expected "
                      "(lld-link -osversion:4.0)")
    if image.file_alignment != 4096 or image.section_alignment != 4096:
        errors.append(f"file alignment {image.file_alignment}, section alignment {image.section_alignment}: 4096 "
                      "expected (98 maps pages from the file only then; lld-link -filealign:4096)")
    if not (image.characteristics & 0x0001) or image.directory(5)[0]:
        errors.append("relocatable: the build links at a fixed base (lld-link -fixed)")
    if image.stack_reserve != args.stack:
        errors.append(f"stack reserve {image.stack_reserve:#x}, {args.stack:#x} expected (lld-link -stack)")

    # ---------- 2. TLS
    callbacks = image.tls_callbacks()
    if callbacks:
        errors.append(f"{callbacks} TLS callback(s): the 9x loader does not call them")

    # ---------- 3. imports
    libraries = config.get("import_libraries", [])
    allowed = read_definitions(args.defs, libraries)
    runtime_only = {name.lower() for name in config.get("runtime_dlls", [])}
    for dll, function in image.delay_imports():
        errors.append(f"{dll}!{function} is delay-loaded: the build loads its run-time DLLs with LoadLibraryA")
    imported = image.imports()
    for dll in sorted({dll for dll, _ in imported}, key=str.lower):
        low = dll.lower()
        functions = sorted(function for d, function in imported if d == dll)
        if NEVER_ON_9X.match(low):
            errors.append(f"{dll} ({len(functions)} imports): no Windows 9x system has it")
        elif low in runtime_only:
            errors.append(f"{dll} ({len(functions)} imports): the build loads it at run time, never by import")
        elif low not in allowed:
            errors.append(f"{dll} ({len(functions)} imports): not one of port/win9x/port.json's import libraries")
        for function in functions:
            if function.startswith("#"):
                errors.append(f"{dll}!{function}: imported by ordinal (9x DLLs' ordinals differ from NT's)")
                continue
            if function in STUBS_ON_9X:
                errors.append(f"{dll}!{function}: 9x exports it as a stub only")
            elif function.endswith("W") and function not in REAL_W_ON_9X and low in (
                    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "winmm.dll", "comdlg32.dll",
                    "shell32.dll"):
                errors.append(f"{dll}!{function}: a W function, which 9x only stubs")
            elif low in allowed and function not in allowed[low]:
                errors.append(f"{dll}!{function}: not in port/win9x/lib/{Path(low).stem}.def (the functions of "
                              "98 SE's that the build may import)")
    notes.append(f"{len(imported)} imports from {len({d.lower() for d, _ in imported})} DLLs")

    # ---------- 4. instruction set
    instructions = disassemble(args.image)
    later = [(address, mnemonic, operands, what) for address, mnemonic, operands in instructions
             for what in [beyond_pentium3(mnemonic, operands)] if what]
    for address, mnemonic, operands, what in later[:40]:
        errors.append(f"{address:#x}: {mnemonic} {operands}: {what}, which a Pentium III lacks")
    if len(later) > 40:
        errors.append(f"... {len(later) - 40} more instructions a Pentium III lacks")
    loads = check_mxcsr_loads(instructions, errors)
    notes.append(f"{len(instructions)} instructions, {loads} ldmxcsr")
    map_path = args.map or args.image.with_suffix(".map")
    if map_path.is_file():
        symbols = read_map(map_path)
        ranges = []
        for index, (address, name, obj) in enumerate(symbols):
            if obj in STARTUP_OBJECTS:
                end = symbols[index + 1][0] if index + 1 < len(symbols) else address + 0x1000
                ranges.append((address, end, name))
        if not any(name == "_halo9x_start" for _, _, name in ranges):
            errors.append(f"{map_path}: no _halo9x_start of w9x_start.o (the start-up code is not checked)")
        probes = 0
        for address, mnemonic, operands in instructions:
            for start, end, name in ranges:
                if not start <= address < end:
                    continue
                if is_sse(mnemonic, operands):
                    if name == PROBE_FUNCTION and (mnemonic, operands) == PROBE:
                        probes += 1
                    else:
                        errors.append(f"{address:#x}: {mnemonic} {operands} in {name}, which runs before the "
                                      "processor check has answered: no SSE there")
                else:
                    what = beyond_pentium(mnemonic, operands)
                    if what:
                        errors.append(f"{address:#x}: {mnemonic} {operands} in {name}, which runs before the "
                                      f"processor check has answered: {what}, which the first Pentium lacks "
                                      "(tools/win9x_build.py STARTUP_FLAGS)")
        if probes != 1:
            errors.append(f"{probes} processor check probes ({PROBE[0]} {PROBE[1]} in {PROBE_FUNCTION}), 1 expected")
        notes.append(f"start-up code: {len(ranges)} functions, the first Pentium's instructions only before the "
                     "check")
        # 5. runtime: by the map's names
        linked = sorted(name for name in map_names(map_path) if RUNTIME_NAMES.match(name.lstrip("_")) or
                        RUNTIME_NAMES.match(name))
        for name in linked[:20]:
            errors.append(f"{name}: the UCRT's or vcruntime's (the 9x build has its own runtime)")
    else:
        # (5. runtime: by the UCRT's own strings)
        notes.append(f"no map ({map_path}): the start-up code and the runtime's names are not checked")
        for signature, what in UCRT_STRINGS:
            if signature in image.data:
                errors.append(f"the static UCRT is linked: {what}")
        if image.load_config_security_cookie():
            errors.append("stack cookies (/GS): the load configuration names __security_cookie")

    if errors:
        print(f"{args.image}: {len(errors)} problem(s) for Windows 98 SE / Me on a Pentium III:")
        for error in errors:
            print(f"  {error}")
        return 1
    if args.stamp:
        args.stamp.write_text("\n".join(notes) + "\n", encoding="utf-8")
    print(f"{args.image}: fit for Windows 98 SE / Me on a Pentium III ({'; '.join(notes)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
