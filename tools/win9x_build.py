"""Ninja rules for the Windows 98 SE / Me build (``ninja win9x``).

The native Windows build (tools/windows_build.py) on an older system: the
same game sources, the same platform layer, compiled with clang for 32-bit
x86 Windows and linked by lld, but for the Pentium III of the original Xbox
(-march=pentium3: SSE, no SSE2), with the Xbox build's C runtime (nxdk's
PDCLib, port/win9x/pdclib, with a KERNEL32 platform layer and the MSVC
extras in port/win9x/src) instead of the UCRT, which needs Windows Vista,
and with a PE header the 9x loader takes (subsystem and system version 4.0,
pages aligned in the file, a fixed base). The executable is
``build/win9x/halo9x.exe``.

What the executable may import is decided by port/win9x/lib/*.def, from
which the import libraries are made (llvm-dlltool); OpenGL, DirectSound,
DirectInput and the shell's and IP helper's functions are loaded at run
time. tools/win9x_check.py checks the linked image (header, imports,
instruction set, runtime) and fails the build.

Like the Xbox build, this is generated wherever nxdk is found (for PDCLib
and its helpers) together with a Windows SDK's headers (for <windows.h>;
not its libraries) and llvm-dlltool. See port/win9x/README.md.

Run as a script, ``python3 tools/win9x_build.py --dev`` writes
build/win9x/dev.ninja: the C runtime and its test programs
(port/win9x/tests) alone, built and checked and run with
``ninja -f build/win9x/dev.ninja``, independently of build.ninja.
"""

import argparse
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
from io import StringIO
from pathlib import Path
from types import SimpleNamespace
from typing import Any, Dict, List, Optional, Sequence, Tuple

if __package__ in (None, ""):
    # run as a script from the repository's root (--dev)
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from tools import linux_build, windows_build, xbox_build  # noqa: E402
    from tools.ninja_syntax import Writer  # noqa: E402
else:
    from . import linux_build, windows_build, xbox_build
    from .ninja_syntax import Writer

LINUX_DIR = Path("port/linux")
WINDOWS_DIR = Path("port/windows")
XBOX_DIR = Path("port/xbox")
PORT_DIR = Path("port/win9x")
PORT_CONFIG = PORT_DIR / "port.json"
PLATFORM_DIR = PORT_DIR / "platform"
KHRONOS_DIR = Path("port/third_party/khronos")
BUILD = Path("build/win9x")
# the runtime's own build (--dev) keeps its copy of the runtime and of the
# import libraries here: ninja keeps each build's dependencies in that
# build's own log, so an object written by one build is rebuilt by the other
DEV_BUILD = BUILD / "dev"
TESTS_DIR = PORT_DIR / "tests"
DEFS_DIR = PORT_DIR / "lib"
TOML_DIR = Path("port/third_party/tomlc17")
KCP_DIR = Path("port/third_party/kcp")
MUSL_MATH_DIR = Path("port/third_party/musl-math")

# Flags shared by every unit of the build.
#  - the Microsoft target gives the game its ABI (structure layout, 16-bit
#    wchar_t, calling conventions, COMDAT inline functions), as on Windows;
#  - the Xbox's processor: single precision in SSE registers, double
#    precision on the x87 at the game's 53 bits, every operation rounded to
#    its type, no fused multiply-adds: the same results bit for bit as the
#    other ports (tools/xbox_build.py explains; cross-play runs in
#    lockstep). The Pentium III has no SSE2, which tools/win9x_check.py
#    checks for in the linked image. Vectorising stays on: clang 22 emits
#    no SSE2 for it (467 game units checked); should the check ever fire,
#    the Xbox build's -fno-vectorize -fno-slp-vectorize -mno-implicit-float
#    are the cure;
#  - no host headers: the Windows SDK's and PDCLib's are given explicitly
#    (and Open Watcom's INCLUDE, which clang would read, is kept out);
#  - tentative definitions shared between units (-fcommon), no optimisations
#    that assume the absence of MSVC-tolerated UB, EBP frames (MSVC /Oy-);
#  - no stack cookies: no runtime for them (__security_cookie);
#  - what nxdk's units are compiled with (nxdk-cc).
WIN9X_ABI_FLAGS = [
    "--target=i686-pc-windows-msvc",
    "-march=pentium3",
    "-mfpmath=sse",
    "-ffp-eval-method=source",
    "-ffp-contract=off",
    "-fno-math-errno",
    "-nostdlibinc",
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    "-fno-stack-protector",
    linux_build.OPTIMISATION,
    "-g",
    "-gcodeview",
    "-Wno-builtin-macro-redefined",
    "-D__STDC__=1",
    "-U__STDC_NO_THREADS__",
]

GAME_FLAGS = windows_build.GAME_FLAGS
PLATFORM_FLAGS = windows_build.PLATFORM_FLAGS

# port/windows/src/win32_*.c talk to Windows only (tools/windows_build.py),
# asking the SDK for the Windows 95 / NT 4.0 interface (w9x_win32.h)
WIN32_FLAGS = [
    "-std=gnu11",
    "-DHALO_WIN9X=1",
    # (gethostbyname and inet_addr are what Winsock has on 9x)
    "-D_WINSOCK_DEPRECATED_NO_WARNINGS",
    "-Wall",
]

# The C runtime: freestanding (its memcpy must not become a call to itself),
# outside link-time optimisation (code generated at link time may call
# memcpy or _alldiv, which must be there as plain objects), and warnings
# off for nxdk's sources
RUNTIME_FLAGS = [
    "-std=gnu11",
    "-ffreestanding",
    "-fno-builtin",
    "-w",
]

# The start-up code and the processor check run before the processor is
# known: their code is the first Pentium's, with no conditional moves, no
# MMX and no SSE, so that the check's message shows on any processor that
# runs Windows 98 (tools/win9x_check.py checks the linked image for it)
STARTUP_FLAGS = ["-march=pentium", "-mno-mmx", "-mno-sse", "-mfpmath=387"]

# The link: a 4.0 header (the 9x loader refuses a newer subsystem), pages
# aligned in the file (9x maps them from it instead of copying the image to
# the swap file), a fixed base below the memory window, no relocations, the
# Windows build's 8 MB stack. No large-address flag (9x ignores it; the
# window is in the private arena). The load configuration's SafeSEH table
# is for Windows XP and later only (w9x_start.c).
WIN9X_LDFLAGS = [
    "-nodefaultlib",
    "-entry:halo9x_start",
    "-osversion:4.0",
    "-fixed",
    "-base:0x400000",
    "-filealign:4096",
    "-stack:0x800000",
    "-dynamicbase:no",
    "-nxcompat:no",
    "-tsaware:no",
    "-safeseh",
    "-opt:ref",
    "-opt:noicf",
    "-debug",
]

# what link-time code generation may call after the archives are read
# (journal R3): kept as plain objects in every link
RUNTIME_LIBCALLS = [
    "_memcpy", "_memmove", "_memset", "_memcmp", "__alldiv", "__allmul", "__allrem", "__allshl", "__allshr",
    "__aulldiv", "__aullrem", "__aullshr", "__chkstk", "__fltused",
]

# The test programs of the C runtime (port/win9x/tests), each a console
# program; the ones named in REFERENCE_TESTS are built a second time against
# the UCRT (when Visual Studio's libraries are found), and their outputs
# must match.
TESTS = ["t_start", "t_threads", "t_malloc", "t_printf", "t_strtod", "t_fmod", "t_stdlib", "t_files", "t_time",
         "t_chkstk", "t_streams2", "t_localtime2"]
REFERENCE_TESTS = ["t_start", "t_printf", "t_strtod", "t_fmod", "t_time", "t_streams2", "t_localtime2"]


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def _load_config() -> Dict[str, Any]:
    return json.loads(PORT_CONFIG.read_text(encoding="utf-8"))


def win9x_configure_inputs() -> List[Path]:
    """Files whose change must re-run configure.py."""
    return [Path(__file__), PORT_CONFIG, PORT_DIR / "src", PORT_DIR / "pdclib", PLATFORM_DIR, DEFS_DIR, TESTS_DIR,
            LINUX_DIR / "src", LINUX_DIR / "game", WINDOWS_DIR / "src"]


# ---------- the tools and the headers

def find_windows_sdk(sln: Any) -> Optional[Path]:
    """The Windows SDK's include directory of one version (the one with um/
    and shared/): configure.py --win9x-sdk, the vcvarsall environment's
    WindowsSdkDir and WindowsSDKVersion, or the newest Windows 10 SDK
    installed in its usual place."""
    candidates: List[Path] = []
    explicit = getattr(sln, "win9x_sdk", None)
    if explicit:
        candidates.append(Path(explicit))
    sdk_dir, sdk_version = os.environ.get("WindowsSdkDir"), os.environ.get("WindowsSDKVersion")
    if sdk_dir and sdk_version:
        candidates.append(Path(sdk_dir) / "include" / sdk_version.strip("\\/"))
    for root in (Path("C:/Program Files (x86)/Windows Kits/10/include"), Path("C:/Program Files/Windows Kits/10/include")):
        if root.is_dir():
            candidates.extend(sorted((p for p in root.iterdir() if p.is_dir()), key=lambda p: p.name, reverse=True))
    for candidate in candidates:
        if (candidate / "um" / "windows.h").is_file() and (candidate / "shared").is_dir():
            return candidate
    return None


def find_msvc() -> Optional[Path]:
    """Visual Studio's MSVC tools (for the UCRT reference builds of the
    tests only): VCToolsInstallDir, or the newest under Program Files"""
    candidates: List[Path] = []
    if os.environ.get("VCToolsInstallDir"):
        candidates.append(Path(os.environ["VCToolsInstallDir"]))
    for root in (Path("C:/Program Files (x86)/Microsoft Visual Studio"), Path("C:/Program Files/Microsoft Visual Studio")):
        if root.is_dir():
            candidates.extend(sorted(root.glob("*/*/VC/Tools/MSVC/*"), key=lambda p: p.name, reverse=True))
    for candidate in candidates:
        if (candidate / "lib" / "x86" / "libcmt.lib").is_file():
            return candidate
    return None


def find_tool(names: Sequence[str]) -> Optional[str]:
    for name in names:
        path = shutil.which(name)
        if path:
            return name
    return None


class Toolchain:
    """What the 9x build is made with (configure.py --win9x-cc and
    --win9x-linker; llvm-dlltool and llvm-lib next to them in the PATH)"""

    def __init__(self, sln: Any) -> None:
        self.cc = getattr(sln, "win9x_cc", None) or "clang"
        self.linker = getattr(sln, "win9x_linker", None) or "lld-link"
        self.dlltool = find_tool(["llvm-dlltool"])
        self.lib = find_tool(["llvm-lib"])
        self.nxdk = xbox_build.find_nxdk(sln)
        self.sdk = find_windows_sdk(sln)

    def missing(self) -> List[str]:
        reasons = []
        if self.nxdk is None:
            reasons.append("nxdk not found (configure.py --nxdk DIR, NXDK_DIR, or ~/nxdk)")
        else:
            # (the C library is a submodule of nxdk's: an nxdk cloned without
            # its submodules has the folders, empty)
            for part in ("lib/pdclib/functions/stdio/fopen.c", "lib/pdclib/include/stdio.h",
                         "lib/pdclib/platform/xbox/functions/math", "lib/xboxrt/c_runtime/_alldiv.s"):
                if not (self.nxdk / part).exists():
                    reasons.append(f"nxdk's C library sources are not there ({self.nxdk / part}; "
                                   "git submodule update --init in nxdk)")
                    break
        if self.sdk is None:
            reasons.append("no Windows SDK headers (configure.py --win9x-sdk DIR, the directory with um/windows.h)")
        if self.dlltool is None:
            reasons.append("llvm-dlltool is not in the PATH")
        if self.lib is None:
            reasons.append("llvm-lib is not in the PATH")
        if find_tool(["llvm-objdump", "llvm-objdump-19", "llvm-objdump-20", "objdump"]) is None:
            reasons.append("llvm-objdump is not in the PATH (tools/win9x_check.py reads the image's instructions)")
        for tool, option in ((self.cc, "--win9x-cc"), (self.linker, "--win9x-linker")):
            if shutil.which(tool) is None:
                reasons.append(f"{tool} is not in the PATH (configure.py {option})")
        return reasons


# ---------- the include sets

def pdclib_include_flags(tools: Toolchain) -> List[str]:
    """PDCLib's headers: the 9x configuration and its additions, then nxdk's
    generic ones (after every overlay)"""
    return [f"-I{PORT_DIR / 'pdclib' / 'include'}", f"-isystem {_quote(tools.nxdk / 'lib/pdclib/include')}"]


def sdk_include_flags(tools: Toolchain) -> List[str]:
    return [f"-isystem {_quote(tools.sdk / 'um')}", f"-isystem {_quote(tools.sdk / 'shared')}"]


def win32_cflags(tools: Toolchain, extra: Sequence[str] = ()) -> str:
    """the Windows-facing units (port/windows/src/win32_*.c and the 9x
    platform's, port/win9x/platform): <windows.h> over PDCLib"""
    return " ".join([
        *WIN9X_ABI_FLAGS,
        *WIN32_FLAGS,
        *extra,
        f"-include {PORT_DIR / 'include' / 'w9x_win32.h'}",
        f"-I{WINDOWS_DIR / 'include' / 'posix'}",
        f"-I{LINUX_DIR / 'src'}",
        f"-I{PLATFORM_DIR}",
        f"-I{PORT_DIR / 'include'}",
        f"-I{PORT_DIR / 'include' / 'crt'}",
        *pdclib_include_flags(tools),
        *sdk_include_flags(tools),
    ])


def runtime_cflags(tools: Toolchain, extra: Sequence[str] = ()) -> str:
    """the C runtime's own units (port/win9x/src, port/win9x/pdclib and the
    shared Xbox ones): Win32 and PDCLib's internals"""
    return " ".join([
        *WIN9X_ABI_FLAGS,
        *RUNTIME_FLAGS,
        *extra,
        f"-include {PORT_DIR / 'include' / 'w9x_win32.h'}",
        f"-I{PORT_DIR / 'include'}",
        f"-I{PORT_DIR / 'include' / 'crt'}",
        *pdclib_include_flags(tools),
        f"-I{_quote(tools.nxdk / 'lib/pdclib/functions')}",
        *sdk_include_flags(tools),
    ])


def pdclib_cflags(tools: Toolchain) -> str:
    """nxdk's generic PDCLib and its math: PDCLib's headers only"""
    return " ".join([*WIN9X_ABI_FLAGS, *RUNTIME_FLAGS, *pdclib_include_flags(tools)])


def helper_asflags(tools: Toolchain) -> str:
    """nxdk's 64-bit arithmetic helpers (assembly with its prelude)"""
    return " ".join(["--target=i686-pc-windows-msvc", f"-I{_quote(tools.nxdk / 'lib/xboxrt')}"])


# ---------- the runtime

def runtime_sources(tools: Toolchain, config: Dict[str, Any]) -> List[Tuple[Path, str]]:
    """the C runtime library's units and their kind: pdclib (nxdk's generic
    PDCLib and math), glue (port/win9x/pdclib), runtime (port/win9x/src and
    the shared Xbox units), helper (nxdk's assembly)"""
    sources: List[Tuple[Path, str]] = []
    functions = tools.nxdk / "lib" / "pdclib" / "functions"
    replaced = set(config.get("pdclib_replaced", []))
    for source in sorted(functions.rglob("*.c")):
        if source.relative_to(functions).as_posix() not in replaced:
            sources.append((source, "pdclib"))
    math = tools.nxdk / "lib" / "pdclib" / "platform" / "xbox" / "functions" / "math"
    math_replaced = set(config.get("nxdk_math_replaced", []))
    for source in sorted(math.glob("*.c")):
        if source.stem not in math_replaced:
            sources.append((source, "pdclib"))
    for source in sorted((PORT_DIR / "pdclib" / "functions").rglob("*.c")):
        sources.append((source, "glue"))
    for name in config.get("runtime_sources", []):
        sources.append((Path(name), "runtime"))
    for name in config.get("nxdk_runtime_helpers", []):
        sources.append((tools.nxdk / "lib" / "xboxrt" / "c_runtime" / name, "helper"))
    return sources


def _object_path(obj_dir: Path, source: Path, tools: Toolchain) -> Path:
    """an object's place under obj_dir: nxdk's sources under nxdk/, the
    repository's at their own path (no spaces: lld-link's response files)"""
    try:
        relative = Path("nxdk") / source.resolve().relative_to(tools.nxdk.resolve())
    except ValueError:
        relative = source
    return obj_dir / Path(str(relative.with_suffix(".o")).replace(" ", "_"))


def emit_rules(n: Writer, tools: Toolchain, sln: Any) -> None:
    n.variable("win9x_cc", tools.cc)
    n.variable("win9x_linker", tools.linker)
    n.variable("win9x_dlltool", tools.dlltool or "llvm-dlltool")
    n.variable("win9x_lib", tools.lib or "llvm-lib")
    n.rule(
        name="win9x_cc",
        command=f"{linux_build.compile_launcher(sln)}$win9x_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="WIN9X CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="win9x_as",
        command="$win9x_cc $asflags -c $in -o $out",
        description="WIN9X AS $out",
    )
    n.rule(
        name="win9x_implib",
        command="$win9x_dlltool -m i386 -k -d $in -l $out",
        description="WIN9X IMPLIB $out",
    )
    n.rule(
        name="win9x_lib",
        command="$win9x_lib /out:$out @$out.rsp",
        description="WIN9X LIB $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.rule(
        name="win9x_link",
        command="$win9x_linker $ldflags -out:$out @$out.rsp $libs",
        description="WIN9X LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.rule(
        name="win9x_check",
        command=f"$python {_quote(Path('tools') / 'win9x_check.py')} $image --map $map $checkflags --stamp $out",
        description="WIN9X CHECK $image",
    )


def emit_import_libraries(n: Writer, config: Dict[str, Any], root: Path = BUILD) -> List[Path]:
    """the import libraries (under root), from port/win9x/lib/*.def"""
    libraries = []
    for name in config.get("import_libraries", []):
        library = root / "lib" / f"{name}.lib"
        n.build(outputs=library, rule="win9x_implib", inputs=DEFS_DIR / f"{name}.def")
        libraries.append(library)
    return libraries


def emit_runtime(n: Writer, tools: Toolchain, config: Dict[str, Any], root: Path = BUILD) -> Tuple[Path, List[Path]]:
    """the C runtime library (w9xcrt.lib under root) and the start-up
    objects; returns them"""
    crt_dir = root / "crt"
    members: List[Path] = []
    pdclib = pdclib_cflags(tools)
    runtime = runtime_cflags(tools)
    helper = helper_asflags(tools)
    for source, kind in runtime_sources(tools, config):
        obj = _object_path(crt_dir, source, tools)
        members.append(obj)
        if source.suffix == ".s":
            n.build(outputs=obj, rule="win9x_as", inputs=source,
                    variables={"asflags": helper if kind == "helper" else "--target=i686-pc-windows-msvc"})
        else:
            n.build(outputs=obj, rule="win9x_cc", inputs=source,
                    variables={"cflags": pdclib if kind == "pdclib" else runtime})
    library = root / "w9xcrt.lib"
    n.build(outputs=library, rule="win9x_lib", inputs=members)
    startup: List[Path] = []
    startup_cflags = runtime_cflags(tools, STARTUP_FLAGS)
    for name in config.get("startup_sources", []):
        source = Path(name)
        obj = _object_path(crt_dir, source, tools)
        n.build(outputs=obj, rule="win9x_cc", inputs=source, variables={"cflags": startup_cflags})
        startup.append(obj)
    return library, startup


def link_flags(subsystem: str, output: Path) -> List[str]:
    return [*WIN9X_LDFLAGS, f"-subsystem:{subsystem},4.0", f"-pdb:{output.with_suffix('.pdb')}",
            f"-map:{output.with_suffix('.map')}", *(f"-include:{name}" for name in RUNTIME_LIBCALLS)]


def emit_check(n: Writer, image: Path, defs: Sequence[Path], extra: Sequence[str] = ()) -> Path:
    stamp = image.with_suffix(".check")
    n.build(outputs=stamp, rule="win9x_check", inputs=image,
            implicit=[Path("tools/win9x_check.py"), Path("tools/xbox_sse2_check.py"), PORT_CONFIG, *defs],
            variables={"image": str(image), "map": str(image.with_suffix(".map")),
                       "checkflags": " ".join(extra)})
    return stamp


# ---------- the game (ninja win9x)

def inline_export_wrapper(source: Path) -> Path:
    """As tools/windows_build.py (inline_export_wrapper): a unit that defines
    an external __inline function is compiled through a wrapper that takes
    the functions' addresses, so that clang emits them. Returns the file to
    compile."""
    if source.suffix != ".c" or not str(source).replace(os.sep, "/").startswith("source/"):
        return source
    names = windows_build.EXPORTED_INLINE.findall(
        windows_build.COMMENT.sub("", source.read_text(encoding="utf-8", errors="replace")))
    if not names:
        return source
    wrapper = BUILD / "inline_exports" / source
    text = (
        "/* generated by tools/win9x_build.py: see inline_export_wrapper */\n"
        f'#include "{source.resolve().as_posix()}"\n'
        "static void *const halo_win9x_inline_exports[] __attribute__((used)) = {\n"
        + "".join(f"\t(void *){name},\n" for name in names)
        + "};\n"
    )
    wrapper.parent.mkdir(parents=True, exist_ok=True)
    if not wrapper.is_file() or wrapper.read_text(encoding="utf-8") != text:
        wrapper.write_text(text, encoding="utf-8")
    return wrapper


def generate_win9x_build(n: Writer, sln: Any) -> None:
    if not PORT_CONFIG.is_file():
        return
    tools = Toolchain(sln)
    missing = tools.missing()
    if missing:
        print(f"Windows 98 build disabled: {'; '.join(missing)}", file=sys.stderr)
        return
    if getattr(sln, "port_big_sessions", False):
        print("Windows 98 build disabled: --big-sessions builds never play with the Xbox, whose session layout "
              "the 9x build keeps", file=sys.stderr)
        return
    linux_config: Dict[str, Any] = json.loads((LINUX_DIR / "port.json").read_text(encoding="utf-8"))
    config = _load_config()

    tags_header = BUILD / "halo_msvc_tags.h"
    prefix_header = PORT_DIR / "include" / "halo_win9x_prefix.h"
    obj_dir = BUILD / "obj"
    output = BUILD / "halo9x.exe"

    n.comment("Windows 98 SE / Me build (ninja win9x)")
    emit_rules(n, tools, sln)
    n.rule(
        name="win9x_msvc_tags",
        command="$python tools/linux_msvc_semantics.py --output $out --tags source",
        description="WIN9X MSVC TAGS $out",
        restat=True,
    )
    game_headers = sorted(p for p in Path("source").rglob("*") if p.suffix in (".c", ".h"))
    n.build(outputs=tags_header, rule="win9x_msvc_tags",
            implicit=[Path("tools/linux_msvc_semantics.py"), *game_headers])
    libraries = emit_import_libraries(n, config)
    runtime_library, startup = emit_runtime(n, tools, config)

    # the memory window's place (configure.py --window-base), as the other
    # native builds take it
    window_flags = getattr(linux_build, "window_base_flags", lambda _sln: [])(sln)
    abi_extra = list(window_flags) + (["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else [])
    lto = linux_build.lto_mode(sln)
    lto_cflags = [] if lto == "off" else ["-flto=thin" if lto == "thin" else "-flto=full"]
    # the PC's profile (the 9x build never trains one: no profile runtime
    # for 9x, and an instrumented build would be far too slow there)
    profile = linux_build.pgo_profile(sln, None, [linux_build.WINDOWS_PROFILE, linux_build.LINUX_PROFILE], tools.cc)
    optimise = lto_cflags + linux_build.profile_use_flags(profile)
    pdclib_includes = pdclib_include_flags(tools)
    crt_includes = [f"-I{PORT_DIR / 'include' / 'crt'}", f"-I{XBOX_DIR / 'include' / 'crt'}"]
    objects: List[Path] = []

    def add_object(source: Path, cflags: str) -> None:
        obj = obj_dir / Path(str(source.with_suffix(".o")).replace(" ", "_"))
        objects.append(obj)
        n.build(
            outputs=obj,
            rule="win9x_cc",
            inputs=inline_export_wrapper(source),
            implicit=[*linux_build.xdk_headers(), prefix_header, tags_header, source,
                      *([profile] if profile else [])],
            variables={"cflags": cflags},
        )

    abi = " ".join(WIN9X_ABI_FLAGS + abi_extra + optimise)
    excluded = set(linux_config.get("exclude_sources", []))
    for proj in sln.projects:
        if proj.name not in linux_config["projects"]:
            continue
        options = proj.options
        defines = " ".join(f"-D{d}" for d in options.get("defines") or [])
        includes = " ".join(
            f"-I{_quote(d)}"
            for d in options.get("include_dirs") or []
            if Path(d) != Path("xbox/include")
        )
        game_cflags = " ".join([
            abi,
            " ".join(GAME_FLAGS),
            f"-include {prefix_header}",
            f"-include {tags_header}",
            defines,
            *crt_includes,
            f"-I{WINDOWS_DIR / 'include'}",
            includes,
            f"-I{linux_build.XDK_INCLUDE}",
            *pdclib_includes,
        ])
        for obj in proj.objects:
            name = str(obj.file_path).replace(os.sep, "/")
            if obj.status.name == "Missing" or name in excluded or obj.file_path.suffix.lower() != ".c":
                continue
            add_object(obj.file_path, game_cflags)
        for source in sorted(Path(linux_config["game_sources"]).glob("*.c")):
            add_object(source, game_cflags)

    platform_cflags = " ".join([
        abi,
        " ".join(PLATFORM_FLAGS),
        # (<stddef.h> is the Xbox build's StdDef.h shim where file names
        # ignore case, port/xbox/include/crt)
        "-Wno-nonportable-include-path",
        f"-include {prefix_header}",
        f"-I{WINDOWS_DIR / 'include' / 'posix'}",
        *crt_includes,
        f"-I{LINUX_DIR / 'src'}",
        f"-I{PORT_DIR / 'include'}",
        f"-I{WINDOWS_DIR / 'include'}",
        # OpenGL 1.1's header and Khronos' extension header (gl.h; glext.h
        # includes <KHR/khrplatform.h>): no SDL to bring them
        f"-I{KHRONOS_DIR}",
        f"-I{TOML_DIR}",
        f"-I{KCP_DIR}",
        f"-iquote {LINUX_DIR / 'include'}",
        "-Isource -Isource/cseries",
        f"-I{linux_build.XDK_INCLUDE}",
        *pdclib_includes,
    ])
    for name in config.get("linux_platform_sources", []):
        add_object(LINUX_DIR / "src" / name, platform_cflags)
    for name in config.get("windows_platform_sources", []):
        add_object(WINDOWS_DIR / "src" / name, platform_cflags)
    for name in config.get("platform_sources", []):
        add_object(Path(name), platform_cflags)
    win32 = win32_cflags(tools, abi_extra + optimise)
    for name in config.get("windows_sources", []):
        add_object(WINDOWS_DIR / "src" / name, win32)
    for name in config.get("win32_platform_sources", []):
        add_object(Path(name), win32)
    third_party = " ".join([abi, "-std=gnu11", "-w", *crt_includes, *pdclib_includes])
    # the settings file's parser (port/third_party/tomlc17)
    add_object(TOML_DIR / "tomlc17.c", third_party)
    # internet play's reliable streams (port/third_party/kcp; p2p.c)
    add_object(KCP_DIR / "ikcp.c", third_party)
    # the game's sin, pow and the rest, the same on every port
    for source in linux_build.musl_math_sources():
        add_object(source, " ".join([abi, "-std=gnu11", "-w", f"-I{MUSL_MATH_DIR}/include",
                                     f"-include {MUSL_MATH_DIR}/include/libm.h", *pdclib_includes]))

    # a console program with configure.py --win9x-console: the log on the
    # console the game was started from, for debugging
    console = bool(getattr(sln, "win9x_console", False))
    ldflags = link_flags("console" if console else "windows", output)
    if lto_cflags:
        ldflags.append("-opt:lldlto=2")
    n.build(
        outputs=output,
        rule="win9x_link",
        inputs=startup + objects,
        implicit=[runtime_library, *libraries],
        implicit_outputs=[output.with_suffix(".map"), output.with_suffix(".pdb")],
        variables={
            "ldflags": " ".join(ldflags),
            "libs": " ".join(str(p) for p in [runtime_library, *libraries]),
        },
    )
    defs = [DEFS_DIR / f"{name}.def" for name in config.get("import_libraries", [])]
    stamp = emit_check(n, output, defs, ["--console"] if console else [])
    n.build(outputs="win9x", rule="phony", inputs=[output, stamp])
    n.newline()


# ---------- the runtime's own build (--dev)

def reference_cflags(tools: Toolchain, msvc: Path) -> Tuple[str, str]:
    """compile and link flags of the UCRT reference builds of the tests:
    Visual Studio's static runtime (the Windows build's), the same processor
    and floating-point flags"""
    lib_root = tools.sdk.parent.parent / "Lib" / tools.sdk.name
    cflags = " ".join([
        "--target=i686-pc-windows-msvc", "-march=pentium3", "-mfpmath=sse", "-ffp-eval-method=source",
        "-ffp-contract=off", "-fno-math-errno", "-O2", "-nostdlibinc", "-w",
        "-D_USE_32BIT_TIME_T", "-D_CRT_SECURE_NO_WARNINGS", "-D_CRT_NONSTDC_NO_WARNINGS",
        # (after clang's own headers: MSVC's vadefs.h takes va_start for the
        # address of the last parameter, which clang's optimiser breaks)
        f"-idirafter {_quote(msvc / 'include')}", f"-idirafter {_quote(tools.sdk / 'ucrt')}",
        f"-idirafter {_quote(tools.sdk / 'um')}", f"-idirafter {_quote(tools.sdk / 'shared')}",
    ])
    ldflags = " ".join([
        "-fuse-ld=lld", f"-Wl,-libpath:{_quote(msvc / 'lib' / 'x86')}",
        f"-Wl,-libpath:{_quote(lib_root / 'ucrt' / 'x86')}", f"-Wl,-libpath:{_quote(lib_root / 'um' / 'x86')}",
    ])
    return cflags, ldflags


def generate_dev_build(n: Writer, sln: Any) -> int:
    """build/win9x/dev.ninja: the C runtime and its import libraries (its
    own copies, under build/win9x/dev), and the test programs built, checked
    (tools/win9x_check.py) and run"""
    tools = Toolchain(sln)
    missing = tools.missing()
    if missing:
        print(f"win9x_build: cannot build: {'; '.join(missing)}", file=sys.stderr)
        return 1
    config = _load_config()
    n.comment("The Windows 98 build's C runtime and its tests (python3 tools/win9x_build.py --dev)")
    n.variable("ninja_required_version", "1.10")
    n.variable("builddir", str(BUILD))
    n.variable("python", _quote(Path(sys.executable)))
    n.newline()
    emit_rules(n, tools, sln)
    n.rule(
        name="win9x_test_run",
        command="$python tools/win9x_build.py --run-test $in --output $out $compare",
        description="WIN9X TEST $in",
    )
    n.rule(
        name="win9x_reference_cc",
        command="$win9x_cc $cflags $in -o $out $ldflags",
        description="WIN9X UCRT REFERENCE $out",
    )
    libraries = emit_import_libraries(n, config, DEV_BUILD)
    runtime_library, startup = emit_runtime(n, tools, config, DEV_BUILD)
    defs = [DEFS_DIR / f"{name}.def" for name in config.get("import_libraries", [])]
    test_dir = BUILD / "tests"
    test_cflags = runtime_cflags(tools, ["-DW9X_TEST=1"])
    msvc = find_msvc()
    if msvc is None:
        print("win9x_build: Visual Studio's libraries not found: the tests run without their UCRT references",
              file=sys.stderr)
    built: List[Path] = []
    runs: List[Path] = []
    for name in TESTS:
        source = TESTS_DIR / f"{name}.c"
        obj = test_dir / f"{name}.o"
        exe = test_dir / f"{name}.exe"
        n.build(outputs=obj, rule="win9x_cc", inputs=source, variables={"cflags": test_cflags})
        n.build(
            outputs=exe,
            rule="win9x_link",
            inputs=startup + [obj],
            implicit=[runtime_library, *libraries],
            implicit_outputs=[exe.with_suffix(".map"), exe.with_suffix(".pdb")],
            variables={"ldflags": " ".join(link_flags("console", exe)),
                       "libs": " ".join(str(p) for p in [runtime_library, *libraries])},
        )
        built.append(emit_check(n, exe, defs, ["--console"]))
        compare = ""
        implicit = [exe.with_suffix(".check")]
        if msvc is not None and name in REFERENCE_TESTS:
            reference = test_dir / "ucrt" / f"{name}.exe"
            cflags, ldflags = reference_cflags(tools, msvc)
            n.build(outputs=reference, rule="win9x_reference_cc", inputs=source,
                    variables={"cflags": cflags, "ldflags": ldflags})
            reference_output = test_dir / "ucrt" / f"{name}.out"
            n.build(outputs=reference_output, rule="win9x_test_run", inputs=reference,
                    variables={"compare": ""})
            compare = f"--compare {reference_output}"
            implicit.append(reference_output)
        output = test_dir / f"{name}.out"
        n.build(outputs=output, rule="win9x_test_run", inputs=exe, implicit=implicit,
                variables={"compare": compare})
        runs.append(output)
    n.build(outputs="win9x_runtime", rule="phony", inputs=[runtime_library, *startup, *libraries])
    n.build(outputs="win9x_tests", rule="phony", inputs=built)
    n.build(outputs="win9x_test_runs", rule="phony", inputs=runs)
    n.default(["win9x_tests", "win9x_test_runs"])
    return 0


# ---------- the test runner (the dev build's)

def run_test(executable: Path, output: Path, compare: Optional[Path]) -> int:
    """Runs a test program; its standard output goes to output. With
    compare, the output must equal that file (the UCRT reference's), line
    for line, except lines starting with '#' (the program's own notes,
    which may differ)."""
    result = subprocess.run([str(executable.resolve())], capture_output=True, timeout=600)
    text = result.stdout.decode("latin-1").replace("\r\n", "\n")
    if result.returncode != 0:
        sys.stderr.write(text[-4000:])
        sys.stderr.write(result.stderr.decode("latin-1")[-2000:])
        print(f"{executable}: exit code {result.returncode}", file=sys.stderr)
        return 1
    if compare is not None:
        expected = compare.read_text(encoding="latin-1").replace("\r\n", "\n")
        ours = [line for line in text.split("\n") if not line.startswith("#")]
        theirs = [line for line in expected.split("\n") if not line.startswith("#")]
        if ours != theirs:
            differences = [line for line in difflib.unified_diff(theirs, ours, "ucrt", "w9x", n=0, lineterm="")]
            print(f"{executable}: {sum(1 for d in differences if d.startswith('+') and not d.startswith('+++'))} "
                  f"line(s) differ from the UCRT's ({compare}):", file=sys.stderr)
            print("\n".join(differences[:60]), file=sys.stderr)
            return 1
    with open(output, "w", encoding="latin-1", newline="\n") as f:
        f.write(text)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dev", action="store_true", help="write build/win9x/dev.ninja (the runtime and its tests)")
    parser.add_argument("--nxdk", help="nxdk (default: NXDK_DIR, or ~/nxdk)")
    parser.add_argument("--win9x-sdk", help="the Windows SDK's include directory of one version")
    parser.add_argument("--win9x-cc", help="the compiler (default: clang)")
    parser.add_argument("--win9x-linker", help="the linker (default: lld-link)")
    parser.add_argument("--run-test", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--output", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--compare", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.run_test:
        return run_test(args.run_test, args.output, args.compare)
    if not args.dev:
        parser.error("nothing to do: --dev writes build/win9x/dev.ninja (configure.py writes the game's rules)")
    sln = SimpleNamespace(nxdk=args.nxdk, win9x_sdk=args.win9x_sdk, win9x_cc=args.win9x_cc,
                          win9x_linker=args.win9x_linker, compiler_launcher=None)
    out = StringIO()
    status = generate_dev_build(Writer(out), sln)
    if status:
        return status
    BUILD.mkdir(parents=True, exist_ok=True)
    target = BUILD / "dev.ninja"
    if not target.is_file() or target.read_text(encoding="utf-8") != out.getvalue():
        target.write_text(out.getvalue(), encoding="utf-8")
    print(f"wrote {target}: ninja -f {target.as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
