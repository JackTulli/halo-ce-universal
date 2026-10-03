# Windows 98 SE / Me

`ninja win9x` compiles the game for Windows 98 Second Edition and
Windows Me on a Pentium III or later processor. The result is
`build/win9x/halo9x.exe`. You must build on Windows.

This build is not complete. The executable builds, links and passes the
image check, but it has not run on Windows 98 or Me yet, and the renderer
for the OpenGL 2.0 and 1.1 drivers of those systems is still being
written: on a driver without the renderer's functions the window stays
black.

`port.json` lists what the build compiles: the Linux build's platform
units by name (`linux_platform_sources`; a new unit in `port/linux/src`
is not part of this build until it is listed), the Windows build's
(`windows_sources`, `windows_platform_sources`), the 9x window, input and
sound layer of `platform/` (`platform_sources` see the Xbox SDK's names,
`win32_platform_sources` see `<windows.h>`), the C runtime and the import
libraries.

`python3 configure.py --win9x-console` links a console program instead,
for debugging: the log is then also written to the console the game was
started from.

At start the game shows the settings window (`platform/w9x_launcher.c`,
which the Windows build also compiles): the display mode and the renderer,
the field of view, the `[graphics]` switches, sound and input, and every
other setting of `config.toml` in a list under Advanced. Play writes the
file and goes on with the new values, Quit ends the program. The window is
built from a dialog template made in memory, of standard controls and
comctl32's tab control, trackbar and tooltips (loaded at run time:
`comctl32.dll` is in `port.json`'s `runtime_dlls`), and fits a 640x480
desktop. `launcher.show` in the file, the checkbox at the bottom of the
window, `-nolauncher` and `-launcher` on the command line decide whether
it comes up. `tests/launcher` checks its layout and the settings file's
writer on the PC.

## How it differs from the Windows build

The Windows build (`port/windows`) needs Windows Vista or later. This
build uses the same game sources and the same platform layer, with these
differences:

| | Windows build | 9x build |
| --- | --- | --- |
| Processor | the computer that builds it (`-march=native`) | Pentium III (`-march=pentium3`): SSE, no SSE2, as on the Xbox |
| C runtime | static UCRT | nxdk's PDCLib with a KERNEL32 platform layer (`pdclib/`, `src/`) |
| Imports | the Windows SDK's import libraries | the lists in `lib/*.def` only |
| SDL3, `bcrypt`, `iphlpapi` | imported | not used; `opengl32`, `dsound`, `dinput`, `shell32` and `iphlpapi` are loaded at run time |
| Header | subsystem 6.0, relocatable | subsystem 4.0, fixed base `0x400000`, file alignment 4096 |

The single-precision arithmetic is in SSE registers and the double
precision on the x87 at 53 bits, as on the Xbox. The results are the
same bit for bit as on the other ports, which cross-play needs.

## Requirements

- MSYS2's MINGW64 clang, lld (`lld-link`), `llvm-dlltool`, `llvm-lib`,
  `llvm-objdump`, Python 3 and ninja in the `PATH`. These are the tools
  of the Xbox build (`port/xbox/README.md`).
- nxdk (`configure.py --nxdk`, `NXDK_DIR`, or `~/nxdk`). The build uses
  its C library sources (PDCLib, dlmalloc, its 64-bit arithmetic
  helpers). It does not need nxdk's libraries to be built.
- The headers of a Windows 10 or 11 SDK (`um/` and `shared/`), found
  through the `vcvarsall.bat` environment or in their usual folder, or
  given with `configure.py --win9x-sdk`. The build does not use the SDK's
  libraries or the UCRT.

Open Watcom sets `INCLUDE` for every program on some computers. The
build passes `-nostdlibinc`, so clang does not read those headers.

## The C runtime

| Part | Source |
| --- | --- |
| stdio, string, stdlib, time conversions, `qsort` | nxdk's PDCLib, generic units |
| PDCLib over KERNEL32 (open, read, write, seek, rename, remove, `tmpfile`, `fdopen`, locks, errno for each thread) | `pdclib/functions/` |
| PDCLib's configuration and headers | `pdclib/include/` (from nxdk's Xbox platform, edited for Win32) |
| `malloc` | nxdk's dlmalloc (`src/w9x_malloc.c`) |
| start-up: processor check, floating-point state, constructors, `argv`, TLS directory | `src/w9x_start.c`, `src/w9x_cpucheck.c`, `src/w9x_tls.s` |
| `__chkstk` that probes each stack page | `src/w9x_chkstk.s` |
| MSVC extras, `_control87` (x87 and MXCSR), time and local time, `getenv`, `abort`, exact `fmod` | `src/w9x_crt.c` |
| MSVC descriptors (`_open`, `_lseeki64`, `_chsize_s`, `_stat`, ...) | `src/w9x_crt_io.c` |
| printf with `%f`, `%I64d`, `%S` (shared with the Xbox build) | `src/msvc_printf.c` |
| `strtod` (shared with the Xbox build) | `src/msvc_strtod.c` |
| `memcpy` and friends, `lrint`, `scalbn` and other math | `port/xbox/src/nx_string.c`, `nx_math.c` |

The game's printf, file names and floating point work as in the Xbox
build. The game opens its files through the Windows build's path
translation (`port/windows/src/windows_crt.c`).

## Build and test the runtime alone

```sh
python3 tools/win9x_build.py --dev
ninja -f build/win9x/dev.ninja
```

This builds the runtime, the import libraries and the test programs of
`tests/`. It checks each program with `tools/win9x_check.py` and runs it.
`build/win9x/dev.ninja` uses its own build log in `build/win9x` and its
own copy of the runtime and the import libraries in `build/win9x/dev`,
so it does not affect `build.ninja`.

If Visual Studio's libraries are found, the printf, strtod, fmod, start-up,
time, local time and stream tests are also built against the UCRT. Their outputs must be
the same line for line. Lines that start with `#` are notes and are not
compared.

## The image check

`tools/win9x_check.py` runs after each link and fails the build if the
image breaks any of these rules:

- The header is i386, GUI subsystem, version 4.0, file alignment 4096,
  not relocatable, with an 8 MB stack.
- The TLS directory has no callbacks.
- Imports are by name, from the DLLs and functions of `lib/*.def` only.
  There are no delay-load imports, and the run-time DLLs are never
  imported.
- No instruction needs more than a Pentium III. The start-up code and
  the processor check (`src/w9x_start.c`, `src/w9x_cpucheck.c`, compiled
  with `-march=pentium`) use nothing the first Pentium lacks: no SSE but
  the check's own probe, no MMX, no conditional moves. The check's
  message then shows on any processor that runs Windows 98.
- Each `ldmxcsr` loads a value that the check follows back to an
  immediate, an `and` or a `stmxcsr` that leaves DAZ clear; anything else
  on the way (an `or`, a call, a jump) fails the check.
- No UCRT or vcruntime code is linked.

To see what the check reports for the Windows build, enter:

```sh
python3 tools/win9x_check.py build/windows/halo.exe
```
