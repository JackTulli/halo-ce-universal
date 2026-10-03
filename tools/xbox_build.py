"""Ninja rules for the Xbox build (``ninja xbox``).

The game for the original Xbox, built with nxdk (https://github.com/XboxDev/nxdk,
the open-source Xbox SDK): clang for 32-bit x86 Windows (i386-pc-win32,
the ABI the game was written against) with nxdk's C library (PDCLib), its
Win32 subset and its kernel import library, linked by lld into a PE that
nxdk's cxbe turns into an XBE, ``build/xbox/default.xbe``.

Like the other native ports, this is independent of the byte-matching
graph: it compiles the same game sources, the multiplayer units of the
Linux build (port/linux/game), the Xbox-facing platform units of the Linux
build that are host-independent (port/xbox/port.json names them) and the
Xbox platform layer in port/xbox/src. See port/xbox/README.md.
"""

import json
import os
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

from .linux_build import XDK_INCLUDE, compile_launcher, musl_math_sources, xdk_headers
from .ninja_syntax import Writer

LINUX_DIR = Path("port/linux")
PORT_DIR = Path("port/xbox")
PORT_CONFIG = PORT_DIR / "port.json"
BUILD = Path("build/xbox")
TOML_DIR = Path("port/third_party/tomlc17")
KCP_DIR = Path("port/third_party/kcp")
MUSL_MATH_DIR = Path("port/third_party/musl-math")

# nxdk's libraries the game links, under its lib/ (nxdk's own make builds
# them, with its tools: see port/xbox/README.md)
NXDK_LIBRARIES = [
    "libpdclib.lib", "libwinapi.lib", "libxboxrt.lib", "libnxdk_hal.lib",
    "libpbkit.lib",
    "nxdk_usb.lib", "libnxdk.lib",
    "libnxdk_automount_d.lib", "libnxdk_net.lib", "xboxkrnl/libxboxkrnl.lib",
]

# Flags shared by every unit of the build.
#  - the Microsoft target gives the game its ABI: structure layout, 16-bit
#    wchar_t, calling conventions, COMDAT inline functions;
#  - the console's processor is a Pentium III: single precision in SSE
#    registers, as the other ports' (x86-64) arithmetic is, so every
#    operation is rounded to its type and the results match theirs bit for
#    bit; double precision on the x87 (the Pentium III has no SSE2), which
#    the game runs at 53 bits (real_math_reset_precision), IEEE double's.
#    It is the faster of the two in the emulator as well: its SSE is plain
#    single-precision arithmetic (on the host's own), its x87 an 80-bit
#    software one. No vectorising: clang 19's constant fix-up can load a
#    pair of floats with SSE2's movsd (llvm/llvm-project#134607), and
#    tools/xbox_build.py checks the linked image for any SSE2 instruction;
#  - freestanding, as nxdk compiles: no host headers or libraries;
#  - tentative definitions shared between units (-fcommon), no
#    optimisations that assume the absence of MSVC-tolerated UB, EBP frames
#    (MSVC /Oy-: get_return_eip and the stack walker follow the frame chain);
#  - what nxdk's own units are compiled with (nxdk-cc).
XBOX_ABI_FLAGS = [
    "--target=i386-pc-win32",
    "-march=pentium3",
    "-mfpmath=sse",
    "-fno-vectorize",
    "-fno-slp-vectorize",
    # no SSE registers for plain copies (an 8-byte constant copied through
    # one is what the constant fix-up loads with movsd); the arithmetic's
    # own SSE is unaffected
    "-mno-implicit-float",
    # every operation rounded to its type, as on the other ports (the x87
    # keeps 80 bits otherwise, and musl's argument reduction relies on this)
    "-ffp-eval-method=source",
    "-ffreestanding",
    "-nostdlib",
    "-fno-builtin",
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    "-ffp-contract=off",
    # sqrt and fabs as the x87's own instructions (the console's <math.h>
    # maps them to the builtins): nothing reads errno after them, and with
    # the game's 53-bit precision the result is the C library's to the bit
    "-fno-math-errno",
    # each function in a section of its own, so the link drops the ones
    # nothing calls (-opt:ref; some 200 KB of the game's code: libtiff's,
    # zlib's deflate, tool and editor code). Not the data: the decompiled
    # game's globals keep their order
    "-ffunction-sections",
    # for size: the console's 64 MB hold the game, its data caches and the
    # image together, and a smaller image is a faster one on its cache too
    "-Os",
    "-Wno-builtin-macro-redefined",
    "-D__STDC__=1",
    "-U__STDC_NO_THREADS__",
    "-DNXDK",
]

# The units the console's frame spends its time in, built with -O2 instead
# of -Os (2026-10-01 audit: about 280 KB of code, some 100 KB more at -O2,
# for the math, collision and animation of every tick and the scene's
# render). nx_string.c stays -Os: its memcpy must not become a call to
# itself.
XBOX_HOT_UNITS = {
    "source/math/real_math.c",
    "source/math/matrix_math.c",
    "source/math/geometry.c",
    "source/physics/collision_bsp.c",
    "source/physics/bsp3d.c",
    "source/physics/collisions.c",
    "source/physics/collision_features.c",
    "source/models/model_animations.c",
    "source/memory/data.c",
    "source/memory/lruv_cache.c",
    "source/render/render_objects.c",
    "source/structures/structure_visibility.c",
    "source/rasterizer/xbox/rasterizer_xbox_environment.c",
    "source/rasterizer/xbox/rasterizer_xbox_models.c",
    "source/rasterizer/xbox/rasterizer_xbox_transparent_geometry.c",
    "source/rasterizer/xbox/rasterizer_xbox_draw_primitives.c",
    "source/objects/object_lights.c",
    "source/effects/particle_systems.c",
    "source/effects/particles.c",
    "source/effects/decals.c",
    # (2026-10-01 performance audit: the units that change usefully at -O2
    # for the few kilobytes they grow, about 20 KB in all)
    "source/structures/structure_render.c",
    "source/cache/cache_files.c",
    "source/rasterizer/xbox/rasterizer_xbox.c",
    "source/rasterizer/rasterizer.c",
    "source/render/render_sprite.c",
    "source/render/render_cameras.c",
    "source/models/models.c",
    "source/objects/objects.c",
    "source/items/weapons.c",
    "port/linux/game/render_interpolation.c",
    "port/xbox/src/d3d8_nv2a.c",
    "port/xbox/src/nx_gpu.c",
    "port/linux/src/dsound_sdl.c",
    "port/linux/src/dsound_mix_kernels.c",
}

# Units without a frame pointer: the sound mixer's inner loops
# (port/linux/src/dsound_mix_kernels.c), integer leaf functions that call
# nothing, so no stack walk passes through them; it frees a seventh
# register for their loops
XBOX_NO_FRAME_POINTER_UNITS = {
    "port/linux/src/dsound_mix_kernels.c",
}

# Game units nothing on the console uses, left out of its build: libtiff's
# and zlib's parts the game never calls (the tools' TIFF import, gzip files,
# compression), 140 KB of fax tables among them. A unit something did use
# would fail the link (the TIFF reading, strip and compress units are named
# by code the link then drops: they stay, and -opt:ref drops their code).
XBOX_EXCLUDED_UNITS = {
    "source/bitmaps/libtiff/tif_fax3.c",
    "source/bitmaps/libtiff/tif_thunder.c",
    "source/bitmaps/libtiff/tif_next.c",
    "source/bitmaps/libtiff/tif_print.c",
    "source/bitmaps/libtiff/tif_ccittrle.c",
    "source/memory/zlib/gzio.c",
}

GAME_FLAGS = [
    "-std=gnu89",
    "-w",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=incompatible-function-pointer-types",
    "-Wno-error=int-conversion",
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=return-type",
]

PLATFORM_FLAGS = [
    "-std=gnu11",
    "-DHALO_LINUX_PLATFORM_LAYER",
    "-Wall",
    "-Wno-unused-function",
    "-Wno-unknown-pragmas",
    "-Wno-microsoft-anon-tag",
    "-Wno-pragma-pack",
    "-Wno-ignored-attributes",
    "-Wno-duplicate-decl-specifier",
    "-Wno-missing-braces",
    "-Wno-unused-variable",
    "-Wno-ignored-pragmas",
    "-Wno-microsoft-enum-forward-reference",
    "-Wno-language-extension-token",
]

# port/xbox/src/nx_*.c and the generated bridge talk to nxdk only: they see
# nxdk's headers (kernel, Win32 subset, hardware library) instead of the XDK
# declarations, which define the same names differently
NX_FLAGS = [
    "-std=gnu11",
    "-Wall",
    "-Wno-unused-function",
]

# lwIP's pools, sized for the game's sockets (nxdk's own configuration has
# four): nxdk's network library must be built with the same values
# (port/xbox/README.md)
LWIP_OPTIONS = [
    # the socket table (lwIP's FD_SETSIZE): the game's sockets and internet
    # play's stand-ins, a UDP one per peer and port and a TCP one per
    # connection (port/linux/src/p2p.c, the console's limits)
    "-DMEMP_NUM_NETCONN=160",
    "-DMEMP_NUM_UDP_PCB=128",
    "-DMEMP_NUM_TCP_PCB=64",
    "-DMEMP_NUM_TCP_PCB_LISTEN=24",
    "-DMEMP_NUM_NETBUF=64",
    "-DMEMP_NUM_SELECT_CB=16",
    "-DMEMP_NUM_TCPIP_MSG_API=64",
    "-DDEFAULT_UDP_RECVMBOX_SIZE=32",
    "-DDEFAULT_TCP_RECVMBOX_SIZE=32",
    "-DTCPIP_MBOX_SIZE=64",
    "-DLWIP_LOOPBACK_MAX_PBUFS=64",
    "-DSO_REUSE=1", "-DLWIP_NETIF_LOOPBACK=1",
    # a link-local address (169.254.x.x) when no DHCP server answers, as
    # the retail network stack had: two consoles on a crossover cable or a
    # bare switch (nxdk compiles AutoIP in; DHCP still wins where it answers)
    "-DLWIP_DHCP_AUTOIP_COOP=1",
    "-DLWIP_DHCP_AUTOIP_COOP_TRIES=3",
    # a LAN game's consoles each in the address table, and packets kept
    # while an address is resolved (10 entries and no queue by default)
    "-DARP_TABLE_SIZE=32",
    "-DARP_QUEUEING=1",
]


def find_nxdk(sln: Any) -> Optional[Path]:
    """nxdk's directory: configure.py --nxdk, NXDK_DIR, or ~/nxdk"""
    for candidate in (getattr(sln, "nxdk", None), os.environ.get("NXDK_DIR"), Path.home() / "nxdk"):
        if candidate and (Path(candidate) / "lib" / "xboxkrnl" / "xboxkrnl.h").is_file():
            return Path(candidate)
    return None


def xbox_configure_inputs() -> List[Path]:
    """Files whose change must re-run configure.py."""
    return [Path(__file__), PORT_CONFIG, PORT_DIR / "src", PORT_DIR / "include", LINUX_DIR / "src", LINUX_DIR / "game"]


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def nxdk_libc_flags(nxdk: Path) -> List[str]:
    """nxdk's C library headers, for the game and the XDK-facing units"""
    return [
        f"-I{_quote(nxdk / 'lib/pdclib/platform/xbox/include')}",
        f"-I{_quote(nxdk / 'lib/xboxrt/libc_extensions')}",
        f"-isystem {_quote(nxdk / 'lib/pdclib/include')}",
    ]


def nxdk_full_flags(nxdk: Path) -> List[str]:
    """all of nxdk's headers, as nxdk-cc gives them, for the nxdk-facing units"""
    return [
        f"-I{_quote(nxdk / 'lib')}",
        f"-I{_quote(nxdk / 'lib/xboxrt/libc_extensions')}",
        f"-isystem {_quote(nxdk / 'lib/pdclib/include')}",
        f"-I{_quote(nxdk / 'lib/pdclib/platform/xbox/include')}",
        f"-I{_quote(nxdk / 'lib/winapi')}",
        f"-I{_quote(nxdk / 'lib/xboxrt/vcruntime')}",
        # the network stack (lwIP), for the log over the network
        f"-I{_quote(nxdk / 'lib/net/lwip/src/include')}",
        f"-I{_quote(nxdk / 'lib/net/nforceif/include')}",
        # the USB host stack, for the controllers
        f"-I{_quote(nxdk / 'lib/usb/libusbohci/inc')}",
        f"-I{_quote(nxdk / 'lib/usb/libusbohci_xbox')}",
        '-DUSBH_USE_EXTERNAL_CONFIG=\\"usbh_config_xbox.h\\"',
    ]


# a C __inline function with external linkage, defined in a .c file
EXPORTED_INLINE = re.compile(
    r"^(?!\s*static\b)[^;{}()\n]*\b(?:__inline|_inline|__forceinline)\b[^;{}()]*?"
    r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{",
    re.M,
)
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def inline_export_wrapper(source: Path) -> Path:
    """As tools/windows_build.py: a unit that defines an external __inline
    function is compiled through a wrapper that takes the functions'
    addresses, so that clang emits them (as COMDATs) for the units that call
    them through a prototype. Returns the file to compile."""
    if source.suffix != ".c" or not str(source).replace(os.sep, "/").startswith("source/"):
        return source
    names = EXPORTED_INLINE.findall(COMMENT.sub("", source.read_text(encoding="utf-8", errors="replace")))
    if not names:
        return source
    wrapper = BUILD / "inline_exports" / source
    text = (
        "/* generated by tools/xbox_build.py: see inline_export_wrapper */\n"
        f'#include "{source.resolve().as_posix()}"\n'
        "static void *const halo_xbox_inline_exports[] __attribute__((used)) = {\n"
        + "".join(f"\t(void *){name},\n" for name in names)
        + "};\n"
    )
    wrapper.parent.mkdir(parents=True, exist_ok=True)
    if not wrapper.is_file() or wrapper.read_text(encoding="utf-8") != text:
        wrapper.write_text(text, encoding="utf-8")
    return wrapper


def generate_xbox_build(n: Writer, sln: Any) -> None:
    if not PORT_CONFIG.is_file():
        return
    nxdk = find_nxdk(sln)
    if nxdk is None:
        print("Xbox build disabled: nxdk not found (configure.py --nxdk DIR, NXDK_DIR, or ~/nxdk)", file=sys.stderr)
        return
    cxbe = nxdk / "tools" / "cxbe" / "cxbe"
    if not cxbe.is_file() and cxbe.with_suffix(".exe").is_file():
        cxbe = cxbe.with_suffix(".exe")  # nxdk built on Windows (MSYS2)
    missing = [str(nxdk / "lib" / lib) for lib in NXDK_LIBRARIES if not (nxdk / "lib" / lib).is_file()]
    if not cxbe.is_file():
        missing.append(str(cxbe))
    if missing:
        print("Xbox build disabled: nxdk's libraries and tools are not built (make in one of its samples "
              f"builds them); missing {', '.join(missing)}", file=sys.stderr)
        return
    linux_config: Dict[str, Any] = json.loads((LINUX_DIR / "port.json").read_text(encoding="utf-8"))
    config: Dict[str, Any] = json.loads(PORT_CONFIG.read_text(encoding="utf-8"))

    cc = getattr(sln, "xbox_cc", None) or "clang"
    linker = getattr(sln, "xbox_linker", None) or "lld-link"
    tags_header = BUILD / "halo_msvc_tags.h"
    bridge_header = BUILD / "nx_win32.h"
    bridge_source = BUILD / "nx_win32.c"
    thunks_source = BUILD / "win32_thunks.c"
    obj_dir = BUILD / "obj"
    executable = BUILD / "halo.exe"
    xbe = BUILD / "default.xbe"
    prefix_header = PORT_DIR / "include" / "halo_xbox_prefix.h"
    crt_include = PORT_DIR / "include" / "crt"

    n.comment("Xbox build (ninja xbox)")
    n.variable("xbox_cc", cc)
    n.variable("xbox_linker", linker)
    # MSVC gives struct tags first named in a prototype file scope; clang
    # does not (the Linux and Windows builds' generator writes these
    # declarations)
    n.rule(
        name="xbox_msvc_tags",
        command="$python tools/linux_msvc_semantics.py --output $out --tags source",
        description="XBOX MSVC TAGS $out",
        restat=True,
    )
    game_headers = sorted(p for p in Path("source").rglob("*") if p.suffix in (".c", ".h"))
    n.build(outputs=tags_header, rule="xbox_msvc_tags",
            implicit=[Path("tools/linux_msvc_semantics.py"), *game_headers])
    # the calling-convention bridge to nxdk's Win32 subset
    n.rule(
        name="xbox_win32_bridge",
        command="$python tools/xbox_win32_bridge.py --header port/include/xdk/xdk_pdb.h "
                "--names port/xbox/win32_names.txt --output-dir build/xbox",
        description="XBOX WIN32 BRIDGE",
        restat=True,
    )
    n.build(outputs=[bridge_header, bridge_source, thunks_source], rule="xbox_win32_bridge",
            implicit=[Path("tools/xbox_win32_bridge.py"), PORT_DIR / "win32_names.txt", XDK_INCLUDE / "xdk_pdb.h"])
    n.rule(
        name="xbox_cc",
        command=f"{compile_launcher(sln)}$xbox_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="XBOX CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="xbox_link",
        command="$xbox_linker $ldflags -out:$out @$out.rsp $libs",
        description="XBOX LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.rule(
        name="xbox_cxbe",
        # (ninja runs commands without a shell on Windows: cmd does the
        # redirection). The XBE keeps cxbe's 64 MB limit: a 128 MB console
        # without it had the game's fixed memory taken at start-up; only the
        # 720p disc's copy loses it (tools/xbox_disc.py)
        command=f"{'cmd /c ' if os.name == 'nt' else ''}{_quote(cxbe)} -OUT:$out -TITLE:Halo -MODE:retail $in > $out.log",
        description="XBOX CXBE $out",
    )
    # the linked image may not use SSE2 (see XBOX_ABI_FLAGS)
    n.rule(
        name="xbox_sse2_check",
        command=f"{_quote(Path(sys.executable))} {_quote(Path('tools') / 'xbox_sse2_check.py')} $in $out",
        description="XBOX SSE2 CHECK $in",
    )

    abi = " ".join(XBOX_ABI_FLAGS + (["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else []))
    libc = " ".join(nxdk_libc_flags(nxdk))
    excluded = set(linux_config.get("exclude_sources", [])) | XBOX_EXCLUDED_UNITS
    objects: List[Path] = []

    def add_object(source: Path, cflags: str, implicit: Optional[List[Path]] = None) -> None:
        # (no spaces: lld-link's response file takes none unquoted)
        obj = obj_dir / Path(str(source.with_suffix(".o")).replace(" ", "_"))
        objects.append(obj)
        if source.as_posix() in XBOX_HOT_UNITS:
            cflags += " -O2"  # (after -Os: the last one counts)
        if source.as_posix() in XBOX_NO_FRAME_POINTER_UNITS:
            cflags += " -fomit-frame-pointer"  # (after -fno-omit-frame-pointer: the last one counts)
        compiled = inline_export_wrapper(source)
        n.build(
            outputs=obj,
            rule="xbox_cc",
            inputs=compiled,
            implicit=[*xdk_headers(), prefix_header, tags_header, bridge_header, source, *(implicit or [])],
            variables={"cflags": cflags},
        )

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
            f"-I{crt_include}",
            f"-I{PORT_DIR / 'include'}",
            includes,
            f"-I{XDK_INCLUDE}",
            libc,
        ])
        for obj in proj.objects:
            name = str(obj.file_path).replace(os.sep, "/")
            if obj.status.name == "Missing" or name in excluded:
                continue
            if obj.file_path.suffix.lower() != ".c":
                continue
            cflags = game_cflags
            if name.endswith("shell_xbox.c"):
                # the entry point is nx_crash.c's main, under an exception handler
                cflags += " -Dmain=halo_shell_main"
            add_object(obj.file_path, cflags)
        # the native ports' game-side units: the netcode, the interpolation
        for source in sorted(Path(linux_config["game_sources"]).glob("*.c")):
            add_object(source, game_cflags)

    platform_cflags = " ".join([
        abi,
        " ".join(PLATFORM_FLAGS),
        f"-include {prefix_header}",
        f"-I{PORT_DIR / 'src'}",
        f"-I{LINUX_DIR / 'src'}",
        f"-I{BUILD}",
        f"-I{crt_include}",
        f"-I{PORT_DIR / 'include'}",
        f"-I{KCP_DIR}",
        f"-iquote {LINUX_DIR / 'include'}",
        f"-I{TOML_DIR}",
        "-Isource -Isource/cseries",
        f"-I{XDK_INCLUDE}",
        libc,
    ])
    nx_cflags = " ".join([
        abi,
        " ".join(NX_FLAGS),
        " ".join(LWIP_OPTIONS),
        f"-I{PORT_DIR / 'src'}",
        f"-I{BUILD}",
        " ".join(nxdk_full_flags(nxdk)),
    ])
    for source in sorted((PORT_DIR / "src").glob("*.c")):
        add_object(source, nx_cflags if source.name.startswith("nx_") else platform_cflags)
    add_object(bridge_source, nx_cflags)
    add_object(thunks_source, platform_cflags)
    for name in config.get("linux_platform_sources", []):
        add_object(LINUX_DIR / "src" / name, platform_cflags)
    # the settings file's parser (port/third_party/tomlc17)
    add_object(TOML_DIR / "tomlc17.c", " ".join([abi, "-std=gnu11", "-w", libc]))
    # internet play's reliable streams (port/third_party/kcp; p2p.c)
    add_object(KCP_DIR / "ikcp.c", " ".join([abi, "-std=gnu11", "-w", f"-I{crt_include}", libc]))
    # the game's sin, pow and the rest, the same on every port
    # (port/include/halo_math.h)
    for source in musl_math_sources():
        add_object(source, " ".join([abi, "-std=gnu11", "-w", f"-I{MUSL_MATH_DIR}/include",
                                     f"-include {MUSL_MATH_DIR}/include/libm.h", libc]))

    n.build(
        outputs=executable,
        rule="xbox_link",
        inputs=objects,
        # nxdk's libraries: rebuilt ones relink the game
        implicit=[str(nxdk / "lib" / lib) for lib in NXDK_LIBRARIES],
        variables={
            # as nxdk-link: fixed at 0x10000, where the kernel loads an XBE;
            # the retail XBE's 512 KB of stack for the game's main thread; the automount
            # of D: (the disc, or the folder the XBE runs from)
            "ldflags": " ".join([
                "-subsystem:windows", "-fixed", "-base:0x00010000", "-stack:0x80000",
                # unreferenced functions dropped (-ffunction-sections); none
                # folded together, so the map keeps a name for each address
                "-opt:ref", "-opt:noicf",
                "-merge:.edata=.edataxb", "-include:_automount_d_drive",
                f"-map:{_quote(BUILD / 'halo.map')}",
            ]),
            "libs": " ".join(_quote(nxdk / "lib" / lib) for lib in NXDK_LIBRARIES),
        },
    )
    sse2_stamp = BUILD / "sse2_check.stamp"
    n.build(outputs=str(sse2_stamp), rule="xbox_sse2_check", inputs=executable,
            implicit=str(Path("tools") / "xbox_sse2_check.py"))
    n.build(outputs=xbe, rule="xbox_cxbe", inputs=executable, implicit=str(sse2_stamp))
    n.build(outputs="xbox", rule="phony", inputs=xbe)
    n.newline()
