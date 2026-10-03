# Windows

`ninja windows` compiles the game with clang for 32-bit x86 Windows. The
result is a native executable, `build/windows/halo.exe`, with `SDL3.dll`
next to it. You must build on Windows.

The Windows build uses the platform layer of the Linux build
(`port/linux/src`). The graphics, the sound, the input, the settings and
the multiplayer functions are the same as on Linux. Refer to
[port/linux/README.md](../linux/README.md).

## Requirements

You do not need the Xbox SDK.

- Visual Studio or the Visual Studio Build Tools, with the C++ workload.
  The build uses the 32-bit (x86) MSVC libraries and a Windows 10 or 11
  SDK. It does not use the MSVC compiler.
- LLVM (clang and lld), Python 3 and ninja in the `PATH`. For example,
  enter `scoop install llvm python ninja`.
- clang reads the `INCLUDE` and `LIB` environment variables before it looks
  for Visual Studio itself. If another toolchain sets them (Open Watcom
  does, for every program), build in the environment that
  `vcvarsall.bat x86` of the Build Tools sets instead. MSYS2's clang,
  lld and ninja (the Xbox build's, port/xbox/README.md) serve as well.
- A network connection for the first build. `configure.py` downloads the
  Visual C++ development package of SDL 3.4.16 to
  `build/windows/third_party`.

## Build the game

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja windows`.

`configure.py` generates the Windows build only on Windows.

The build uses all the instructions of the processor of the computer that
builds it (`-march=native`). Such a build does not always start on a
different computer. To make a build for other computers, enter
`python configure.py --portable`. Refer to the build options in the main
[README](../../README.md#build-options).

For a new optimization profile (`--pgo=train`), the build compiles the
profile runtime of LLVM for 32-bit x86 (`pgo/halo_profile_runtime.c`).
LLVM for Windows supplies this runtime only for x86-64.

## Start the game

Enter `build\windows\halo.exe`.

The game finds the game data as on Linux. Refer to "Start the game" in
[port/linux/README.md](../linux/README.md#start-the-game).

| Item | Location |
| --- | --- |
| Settings | `config.toml` next to `halo.exe` |
| Saved games | `%APPDATA%\halo`, or `paths.saves` in `config.toml` |
| Log | `debug.txt` in the data root (the folder that contains `maps\`) |

### The settings window

When the game starts, a window of the settings comes up before anything
else: the renderer, fullscreen, the field of view and the rest of
`[display]` and `[graphics]` on pages of their own, sound and input on
theirs, and every other setting of `config.toml` in a list under
Advanced, each with its description. Play writes the file (keeping its
comments and anything else in it) and starts the game with the new
values; Quit ends the program; Defaults puts every setting shown back to
its default. The window is a plain Win32 dialog
(`port/windows/src/win32_launcher.c`, the Windows 98 build's
`port/win9x/platform/w9x_launcher.c` as it is), so it needs nothing of
SDL.

The checkbox at the bottom (`launcher.show` in the file) decides whether
it comes up next time. `halo.exe -nolauncher` starts without it, and
`halo.exe -launcher` brings it back whatever the file says. A copy
started to open an invite link (`halo://...`) skips it.

## How the port operates

The game is 32-bit code, as on Linux, because its data contains 32-bit
pointers. The clang target `i686-pc-windows-msvc` gives the ABI of MSVC:
the structure layout, the 16-bit `wchar_t` and the calling conventions.
Thus the Windows build needs fewer changes than the Linux build.

The executable is large-address-aware, because the platform layer reserves
the Xbox memory at `0x80000000`, in the upper 2 GB of the address space.

`python configure.py --window-base=0x40000000` puts the Xbox memory at
`0x40000000` instead (`port/include/halo_memory_window.h`). Windows 9x needs
this, because its addresses from `0x80000000` are shared by all programs.
Such a build:

- Moves the addresses in each map when it reads the map, because the maps
  contain addresses at `0x80000000` (`port/linux/src/xbox_map_rebase.h`).
- Reserves `0x80000000` to `0x87FFFFFF` with no access, so that an address
  of a map that was not moved causes a crash at that address. The log
  (`halo.log`) shows the two ranges.
- Does not load the saved checkpoints of a build with a different address,
  and the other builds do not load its checkpoints.

The executable stays large-address-aware: the drivers and the heaps can then
use the upper 2 GB.

### Headers

- The game and the platform layer use the Xbox SDK declarations of
  `port/include/xdk`, not the Windows SDK declarations with the same names.
  The compiler reads `port/include/xdk` before the Windows SDK.
- The C runtime is the static UCRT of Windows.
- `include/halo_windows_prefix.h` is the first header of each file. It gives
  Windows names to the Xbox SDK functions of the platform layer (for example
  `CreateFileA`, `ReadFile`, `Sleep`). Thus the code for Windows gets to
  Windows. The list of names is in `include/halo_windows_api_names.h`.
- `include/crt` changes some C runtime headers. The file functions of the
  game (`fopen`, `open`, `mkdir`) go through the translation of Xbox paths
  (`src/windows_crt.c`).
- `include/posix` declares the POSIX functions that the platform layer uses.

### Code for Windows

These files use only the Windows SDK:

| File | Contents |
| --- | --- |
| `src/win32_files.c`, `src/win32_net.c` | The file and socket functions of `port/linux/src/posix.h`. |
| `src/win32_posix.c` | The POSIX functions on Windows threads, critical sections, condition variables, `VirtualAlloc` and the performance counter. |
| `src/win32_memory_watch.c` | The write tracking of textures, with a vectored exception handler. It also writes reports of crashes. |

`port.json` gives the Linux files that these files replace, and the Windows
libraries of the link.

### Inline functions

MSVC makes a COMDAT copy of an external `__inline` function where it does
not inline a call. Some files call such functions through a prototype.
clang inlines more calls. Thus a function can have no copy. The build
prevents this:

- `port/linux/game/msvc_comdat.c` gives one weak definition of each header
  inline function.
- A wrapper takes the address of each `__inline` function in a `.c` file.
  Thus the compiler makes an external copy (`inline_export_wrapper` in
  `tools/windows_build.py`).

MSVC gives file scope to a structure tag in a prototype. clang does not.
Thus the build also includes the declarations of the Linux build
(`build/windows/halo_msvc_tags.h`).

## Limits

- Bink video is not available. The game skips the movies.
- The game uses only the ANSI file functions. A path to the data or the
  saved games with characters that are not in the code page of the system
  does not always operate.
