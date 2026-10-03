# Xbox

`ninja xbox` builds the game for the original Xbox with
[nxdk](https://github.com/XboxDev/nxdk), the open-source Xbox SDK. The
result is `build/xbox/default.xbe`. The game runs on the console's kernel
with its own Direct3D device over the NV2A, sound through the console's
audio hardware and USB controllers, and it plays system link and internet
games with the desktop builds. It runs in xemu and on a real console (a
128 MB one so far), where it has played multiplayer games with the Windows
build ("First test on a real console"). On a 128 MB console with the HDTV
cable it can run at 720p ("720p").

## Requirements

You do not need the Xbox SDK.

- The tools of the Linux build (Python, ninja) and clang with lld
  (`lld-link`).
- nxdk, cloned with its submodules to `~/nxdk` (or `NXDK_DIR`, or
  `configure.py --nxdk DIR`), with its libraries and tools built,
  optimised and with the network options below: `make` in one of its
  samples builds them with the sample's `CFLAGS`, and nxdk adds no
  optimisation of its own, so without `-O2` every library the game links
  (the C library, pbkit, USB, lwIP, the Win32 layer) is unoptimised code:

  ```sh
  make -C ~/nxdk/samples/hello NXDK_DIR=~/nxdk NXDK_NET=y clean
  make -C ~/nxdk/samples/hello NXDK_DIR=~/nxdk NXDK_NET=y \
    CFLAGS="-O2 -fno-omit-frame-pointer $(python3 -c 'from tools.xbox_build import LWIP_OPTIONS; print(" ".join(LWIP_OPTIONS))')"
  ```

  (run from the repository's root). Before that, `python3
  tools/xbox_pbkit_patch.py ~/nxdk` patches pbkit for 720p in 64 MB: the
  frame buffer count and pbkit's depth buffer chosen before `pb_init`, and
  `pb_flip_pending` (the default, triple buffered with a depth buffer, is
  unchanged; the game does not link without the patch; a vertical blank
  the DPC handles flips too), `python3 tools/xbox_lwip_patch.py ~/nxdk`
  turns lwIP's single-interface mode off and its loopback interface on
  (127.0.0.1 without a cable or an address), and `python3
  tools/xbox_usb_patch.py ~/nxdk` makes the USB stack claim its transfer
  descriptors atomically. All are idempotent. nxdk's makefiles miss a
  changed header or source (their dependency files hold `C:/` paths under
  MSYS2), so after the patches run `make ... clean-net`, delete
  `lib/nxdk/net.obj`, `lib/libnxdk.lib`, `lib/pbkit/pbkit.obj`,
  `lib/libpbkit.lib`, `lib/usb/libusbohci/src_core/mem_alloc.obj` and
  `lib/nxdk_usb.lib`, then build as above. nxdk needs `clang`, `lld`, `lld-link`,
  `llvm-lib` and `llvm-dlltool` under those names in the `PATH`.

On Windows, [MSYS2](https://www.msys2.org)'s MINGW64 shell has all of it:
`pacman -S make git bison flex mingw-w64-x86_64-clang mingw-w64-x86_64-lld
mingw-w64-x86_64-llvm mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake
mingw-w64-x86_64-ninja mingw-w64-x86_64-python`, then nxdk cloned with its
submodules (with MSYS2's git, which keeps nxdk's shell scripts' line ends)
and built as above with `NXDK_DIR` set and `$NXDK_DIR/bin` in the `PATH`.
nxdk's tools are then `cxbe.exe` and `extract-xiso.exe`, which the scripts
find.

## Build the game

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja xbox`.

The build compiles the game sources, the multiplayer units of the Linux
build, the Xbox-facing platform units of the Linux build that are
host-independent (`port.json` names them) and the Xbox platform layer in
`src/`, links `build/xbox/halo.exe` with lld and converts it with nxdk's
`cxbe`. `build/xbox/halo.map` names every address.

## Run the game

The game needs the `maps/` folder from an Xbox disc image of the game. To
make a disc image with the build:

1. Enter `python tools/xbox_disc.py --extract <the game's .iso>` once. It
   keeps `maps/` in `build/xbox/disc`.
2. After each build, enter `python tools/xbox_disc.py`. It writes
   `build/xbox/halo-xbox.iso`, in place: an emulator that has the image
   open boots the new build after a reset. `--out <path>` writes (and
   rewrites in place) the image wherever the emulator reads it instead,
   e.g. `--out ~/xemufiles/halo-xbox.iso`.

Start the image in xemu (Machine, Load Disc) or on a console that starts
unsigned XBEs. The game runs in the retail console's 64 MB, which is
xemu's default. This build of the game is the January beta, whose debug
buffers take 2 MB more than the retail game's, so its texture cache is
12 MB instead of 22 (22 at 720p, which uses a 128 MB console's top half).

The disc's `config.toml` holds the settings (`--setting name=value` when
packing sets one): `display.hd_720p`, `display.field_of_view`,
`display.unlocked_framerate`, `display.fps_counter` (the frame rate in the
top left during play), and the `[graphics]` switches, each of which
turns one effect off (shadows, soft_shadows, dynamic_lights, specular,
reflections, decals, detail_objects, bump_mapping, lens_flares,
lens_flare_occlusion, sun_rays, fog, particles, weather, screen_flashes,
screen_effects, active_camouflage_quality). The file lists them all with
their meanings.

### The log

The console shows the game's log (`debug.txt`, which goes to the cache
partition, `Z:`, the one a dashboard's FTP server calls `X:`) and the
platform layer's messages on a text screen. It also sends every line by UDP
to port 9999 of the machine beside it: the host of xemu's user network,
10.0.2.2, or, from a real console on any other network, every machine of
that network (broadcast). To keep them:

    python tools/xbox_log.py --print

When the game exits (a failed start-up ends that way), the screen shows
the first errors of the run (the game's fatal assertions first, then failed
calls) and its last lines for 30 seconds before the console restarts.

A watchdog thread reports every five seconds which SDK function the game
called last. A fault on any of the game's threads, or an assertion inside
nxdk's own code, is reported with its registers, the last SDK call and a
walk of the stack (`src/nx_crash.c`), after which the thread holds and
the watchdog goes on; `python tools/xbox_symbolize.py <address>` names
the addresses. A read or write into memory the console cannot have fails
with its callers instead of reaching the kernel's drivers, where it would
be a silent bugcheck.

## What operates

| Area | Status |
| --- | --- |
| Game code | All of it, as the other native ports build it (`HALO_LINUX`), at the limits every build shares: 16 players on up to 16 machines, the retail pools (`port/include/halo_session_limits.h`; `include/halo_xbox_limits.h` adds the console's game state). |
| Kernel, files, threads, memory | nxdk's Win32 subset and the kernel: `src/xbox_kernel.c`, `src/nx_kernel.c`. The game's fixed memory map (the game state at `0x80061000`, the tag cache at `0x803A6000`) comes from the kernel's contiguous memory, as on the console. Asynchronous reads and writes complete from the alertable waits, as the XAPI's did. |
| Saves, signatures | `src/xbox_xapi.c`: saves on `U:`, the title's folder of the data partition; SHA-1 signatures with a fixed key. |
| Graphics | `src/d3d8_nv2a.c`: the game's Direct3D 8 device over nxdk's pbkit, driving the NV2A directly (the push buffer is `src/nx_gpu.c`). Resources are real (`port/linux/src/d3d8_resources.c`), frames are paced on the display's vertical blank, and the dashboard's 480p and widescreen settings decide the mode, with 720p on a 128 MB console when `display.hd_720p` asks (`src/nx_video.c`). |
| Sound | The Linux build's DirectSound mixer, played through one voice of the MCPX APU (`src/nx_apu.c`, the output xemu plays) or through the AC97 (`src/nx_audio.c`), as the `audio.backend` setting chooses. Refer to "Sound". |
| Input | USB gamepads through nxdk's USB stack, with rumble (`src/nx_input.c`, `src/xinput_usb.c`). |
| Networking | The Linux port's Winsock and XNet layer (`port/linux/src/xnet.c`) over nxdk's lwIP stack (`src/nx_net.c`, which translates the socket address layouts and error codes). System link works, and internet play goes through the master server with the desktop builds. Refer to "Internet play". |
| Bink video | Not available; the game skips the movies (`bink_playback_start` returns without a movie file, and never asks for the 16 MB pool the 12 MB texture cache cannot lend). |
| C library gaps | `src/nx_stdlib.c`: `strtod` and family, which nxdk only stubs; the game's printf family translates MSVC's `%I64d` (`src/msvc_crt.c`). |

## How the port operates

- The compiler is clang for `i386-pc-win32`, the ABI the game was written
  against, with `-Os` (`tools/xbox_build.py`). Single-precision arithmetic
  is SSE's (`-mfpmath=sse`), as on the other ports, so each operation is
  rounded to single precision and the results match theirs; double
  precision stays on the x87 (the Pentium III has no SSE2), at the 53 bits
  the game sets. No vectorising and no SSE registers for plain copies
  (`-mno-implicit-float`): clang 19's constant fix-up loads an 8-byte
  constant copied through one with SSE2's `movsd`
  (llvm/llvm-project#134607). `tools/xbox_sse2_check.py` disassembles
  the linked image and the build stops if any SSE2 instruction is in it.
  `sqrt` and `fabs` are the x87's own instructions (`-fno-math-errno`;
  `include/crt/math.h` maps them to the builtins). The math library's
  shim (`port/third_party/musl-math/include/math.h`) follows the x87
  evaluation method for its double precision.
- `src/nx_string.c` replaces the C library's `memcpy`, `memmove`,
  `memset` and `memcmp` (byte loops in PDCLib) with the string
  instructions a word at a time, for the game and nxdk's libraries alike.
- `include/halo_xbox_prefix.h` is force-included ahead of every game unit
  and Xbox-facing platform unit, as the Windows build's prefix is.
- `include/crt` stands in for the parts of the Microsoft C runtime that
  nxdk's C library (PDCLib) does not have, and sends the game's `debug.txt`
  to `Z:` and the screen (`src/msvc_crt.c`).
- nxdk implements the Win32 functions with the cdecl convention; the game
  calls them as `__stdcall`. `tools/xbox_win32_bridge.py` writes a thunk
  for each function in `win32_names.txt` from its prototype in
  `port/include/xdk/xdk_pdb.h`.
- The nxdk-facing units (`src/nx_*.c` and the generated bridge) see nxdk's
  headers; the rest see the XDK declarations, which name the same things
  differently. `src/nx.h` is the boundary, with plain C types.

## Where it stands

The campaign and multiplayer play on the emulated console with graphics,
sound and controllers, and multiplayer on a real one. System link and
internet play work, also with the desktop builds. Memory is the
constraint on a 64 MB console: this beta build's debug systems (AI
debugging, profiling) take several megabytes the retail build does not,
so the texture cache is 12 MB instead of 22. The dated sections below
record each step. "Bug audit" lists what is still open in the netcode.

## Next

1. 720p on a real console: untested there (see "720p").
2. Memory on a 64 MB console: trim the beta's debug systems to give the
   texture cache its 22 MB back.
3. Movies, once Bink or a substitute decoder exists.
4. The netcode items the bug audit left open.

## Disc, saves and the cache partition (2026-09-28)

- The January build ran from a development kit whose D: was writable. On
  the console D: is the disc, so `platform_redirect_path` (xbox_kernel.c)
  sends every D: path whose first component is not on the disc to the
  cache partition, Z:, reads and writes alike: the checkpoints in d:\core,
  debug.txt, gamestate.txt and the other dumps. The generated Win32 thunks
  (win32_names.txt, "path"), the find, stdio and CreateFileA thunks apply it.
- Z: is no longer wiped at boot; only its cache*.map copies are (a stopped
  run leaves one half-written and the game halts on it). Profiles and
  saved games on Z: therefore persist between boots.
- The game's map cache (z:\cache000.map to cache005.map, the disc's
  compressed maps decompressed: two slots of 278 MB, one of 35 MB, three
  of 47 MB, all made at start-up) adds up to 767 MB, more than the 750 MB
  utility partition holds, so on Z: the last slots failed and every save
  after them got ERROR_DISK_FULL ("failed to create new player profile").
  `platform_redirect_path` sends z:\cacheNNN.map to E:\halo (the data
  partition, made at start-up); the boot-time clearing covers both places,
  so a Z: filled by an earlier build is freed on the next boot.
- A save reaches the disk image with every write, not only when its handle
  closes (WriteFile is a hand-written thunk; an asynchronous write is
  flushed before its completion routine runs; xbox_kernel.c, nx_kernel.c).
  The game keeps a profile open while it plays, and the console's cache
  held a profile the user had just made until a reset of the emulated
  console threw it away: the profile then listed without its "valid" flag
  and every selection ended in "Unable to load saved game file". Z:\saved
  (the profiles' map file, the default profiles) counts as a save path too.
  Verified: a profile made and then the emulator killed outright is listed
  as valid and loads after the reboot.
- SaveMeta.xbx is written and read in the console's format: a byte-order
  mark, `Name=<display name>`, CR LF (xbox_xapi.c). The pre-built disk
  image's own saves use it; the library's earlier files (the bare name)
  still read. Before this the list showed "Name=aj" for such a profile.
- A profile whose blam.sav fails the content checksum (saves from another
  build, or one whose data was lost as above) is listed but cannot be
  selected: the game answers error 31, "Unable to load saved game file",
  and the log says "checksum validation failed for 'u:\...\blam.sav'" at
  boot. The console build now leaves such entries off the selection list
  (saved_game_files.c, HALO_XBOX); they stay on the disk. Creating a new
  profile is the way past them.
- Files opened for writing on U: or T: are flushed (NtFlushBuffersFile)
  when closed. xemu's qcow2 image keeps newly written clusters unlinked
  until the guest flushes; when xemu itself aborted, a profile written
  minutes earlier read back as zeros.
- The printf family substitutes "(null)" for a NULL %s (msvc_crt.c,
  null_safe_arguments): MSVC did, PDCLib crashes, and the game's logs do
  pass NULL (game_state_malloc's type).

## Texture headers (2026-09-28)

- Every texture and surface header the library builds (d3d8_resources.c,
  the back and depth buffers in d3d8_nv2a.c) carries
  D3DFORMAT_BORDERSOURCE_COLOR, as the retail library's did (the map's own
  bitmaps read 0x29 in the low byte). Without it the GPU takes the texture
  to hold a four-texel border and samples it offset by 3.5 texels (xemu:
  `apply_border_adjustment`), which a 256-texel atlas hides but which
  turned the text cache's 8x10 glyphs into their neighbours: the console
  font, the version string and the on-screen keyboard were unreadable
  while the 16-texel menu glyphs passed. Found by drawing a test grid
  through the game's own text path and measuring the lines.
- The text cache is 256x256 on the console (rasterizer_text.c: 1024
  characters), and a character's cache index, which lives in the map's
  tag data, is checked against the slot before use.

## Network (2026-09-29)

- nxdk's start-up brings lwIP up with DHCP before main (nxNetInit waits
  up to ten seconds for the address); the console's log lines go to the
  host over UDP the same way. The port reads the interface through lwIP's
  `netif_default` (nxdk's `g_pnetif` was NULL from the port's side) and
  treats "up" as the link: the emulator's adapter never raises the link
  flag itself (nx_net.c, posix_network_link_up).
- XNetGetEthernetLinkStatus reports the link before any address, and
  XNetGetTitleXnAddr answers "pending" while DHCP may still finish (link up,
  first fifteen seconds), then the address; with no address after that it
  answers as before, "ethernet, no address", because the game halts on a
  transport that failed to initialise (create_transport_endpoint asserts
  it). The log now shows "xbox ethernet link is connected at 100 Mbps in
  full-duplex mode" and "network address 10.0.2.15" under xemu's NAT.
- A local game's client connects to its own server at 127.0.0.1. lwIP
  here first had no loopback interface (LWIP_HAVE_LOOPIF off), so the
  address was swapped for the console's own, which lwIP loops back
  (LWIP_NETIF_LOOPBACK), and swapped back in what the game reads
  (nx_net.c, to_lwip_address/from_lwip_address). The swap was there but
  keyed on nxdk's `g_pnetif`, NULL from the port's side, so hosting a
  system link game ended in WSAEHOSTUNREACH and "Failed to join the System
  Link game". Since 2026-10-01 lwIP has its loopback interface
  (tools/xbox_lwip_patch.py) and 127.0.0.1 goes to it unchanged: the
  console's own address needs the cable, 127.0.0.1 does not. What comes
  from the console's own address still reads as 127.0.0.1.
- System link on the LAN needs the consoles on one LAN: xemu's default NAT
  backend isolates each instance (no broadcasts leave it), so two copies
  must use xemu's udp backend (each pointed at the other) or the pcap
  backend on a real adapter. That is an emulator setting, not the port's.
  Games over the internet come through the master server instead (the
  next section), which the NAT backend reaches.

## Internet play: the server browser (2026-09-29)

- The console plays system link over the internet as the desktop builds
  do (port/linux/README.md, "Internet play"): the master server
  (`network.master_server`; port/master) lists the hosted games, passes the
  machines' signalling and tells each its public address, and the machines
  then talk directly (UDP hole punching; KCP carries the game's TCP
  connections). The Xbox, Windows and Linux builds use the same server,
  protocol (message version 3) and netcode, and since 2026-09-29 the same
  session limits and pools (port/include/halo_session_limits.h: 16 players
  on up to 16 machines, the retail pools), so consoles and computers play
  together. Before that the desktop builds' 128-player layout of the game
  settings made a joining PC drop with "Network connection lost" as soon as
  the console sent them; a desktop build made with `--big-sessions` still
  does.
- The System Link screen is the browser. Its search broadcast also goes to
  the server, every listed host's game answers it through the server, and
  the answers reach this game from the hosts' virtual addresses
  (100.64.0.0/10), so the internet's games list next to the LAN's and are
  joined the same way. Choosing one connects to the host: p2p_outgoing
  starts the join, and the game's connection waits in its local stand-in
  until the tunnel is up. Creating a game lists it while it is hosted.
- The overlay is the Linux build's (port/linux/src/p2p.c, p2p_master.c,
  p2p_crypto.c, port/third_party/kcp), compiled again here over the
  console's sockets and threads: include/crt/pthread.h and xbox_posix.c
  give it its thread and mutexes (kernel critical sections, made on first
  use, so static initializers work); nx_net.c looks names up through
  lwIP's DNS; the stand-ins are fewer (96 UDP, 16 listeners, 40 streams);
  lwIP's socket table is 160 (LWIP_OPTIONS in tools/xbox_build.py, which
  nxdk's network library must be built with: see Requirements). Discord, invite
  hand-offs and the link scheme are the desktop's (`HALO_XBOX` guards).
- Settings on the console are read from `D:\config.toml` on the disc;
  `tools/xbox_disc.py --setting network.master_server=10.0.2.2:27920`
  writes one for a test disc whose master server runs on the emulator's
  host (xemu's NAT reaches the host at 10.0.2.2). A machine the server
  sees at 127.0.0.1 (an emulator's NAT on the server's host) is offered to
  others at the server's address, so two emulated consoles on one machine
  reach each other through their NATs' mappings.
- The console's random seed mixes in the cycle counter, so two emulated
  consoles started alike get different identifiers and tokens.
- The first internet games (Xbox hosting, a PC and another console joining)
  took the host down about a minute in, twice: an access violation in
  strlen under error() from players_update_before_game. The host's
  distributed-netcode diagnostic for a remote player pressing the action
  button near nothing (players.c, network_player_log_idle_action) named
  the nearest item through tag_get_name with an index that was no loaded
  object tag, and the garbage name pointer (0x40011c50, 0x402092c0) was
  read. It now names the tag only when tag_index_is_group says it is one,
  and logs the raw index otherwise. Why the index was bad is open.

## Sound (2026-09-29)

The console plays sound with the Linux build's DirectSound
(port/linux/src/dsound_sdl.c: the streams, Xbox ADPCM, the 3D voices and
the software mixer are shared; on the console the mixer's per-sample work
is integer arithmetic, see Performance audit), whose SDL output the
console's platform layer stands in for. Two paths take the mixer's 48 kHz 16-bit stereo
output, chosen by the `audio.backend` setting (`"auto"`, `"apu"`,
`"ac97"`):

- **The audio processor (nx_apu.c)**, what the emulator plays. The
  retail game's DirectSound (the XDK library the original repository
  links; there is none for nxdk) plays every sound as a voice of the
  MCPX APU's voice processor. xemu's host sound comes from that voice
  processor alone: with its DSP emulation off (the default) it sums every
  audible voice straight to the speakers; with it on, it takes the global
  processor's mix buffers for bins 0 and 1 after the processor's program
  ran. So the mixer's output goes into one looping hardware voice: a ring
  of 16384 frames in contiguous memory that the voice reads through its
  scatter-gather table, which a thread (above the game's priority) keeps
  about 64 ms ahead of the voice's read position, the current buffer
  offset the hardware writes back into the voice's 128-byte structure
  every 32-sample frame. The voice processor is programmed through its
  front end's methods (the NV1BA0 PIO window); the voice lists start
  empty (0xFFFF), the front end's idle-voice trap is enabled as
  DirectSound does, and the global processor boots a three-word program
  from its scratch pages that only ends each frame at once (xemu's DSP
  mode would otherwise run it forever); the encode processor stays in
  reset. Not tested on a console: there the codec is fed by the encode
  processor, whose program only the XDK has, so a console would play
  nothing this way.
- **The AC97 controller (nx_audio.c)**, nxdk's way, which the hardware
  plays and the emulator does not (xemu's AC97 DMA has no host output:
  its QEMU audio layer has no SDL backend built). nxdk's XAudio takes the
  PCM from a ring of four 1024-frame buffers; the same kind of thread
  fills them and queues them, keeping three ahead, and learns what was
  played from the bus master's current descriptor index (CIV), not from
  nxdk's callback: nxdk raises that only once both the analogue and the
  S/PDIF channel completed a buffer, and the emulator does not run the
  S/PDIF one.

`"auto"` tells the emulator by the AC97 codec's identity registers: QEMU's
answers as a SigmaTel STAC9700 (0x8384 0x7600), a console's is an Analog
Devices part. The CPU's signature is no help: xemu presents the console's
own Pentium III (family 6 model 8 stepping 10). The frame statistics
count what the hardware played (frames or buffers) and how often the
mixer was late; the log prints the path chosen and, every five seconds,
the voice's position, the ring's, the restarts and the mixer's counters.
The mixer's math is the C runtime's double one on the console (powf,
cosf, tanhf shims). audio.enabled and audio.volume apply. Packets are
decoded to PCM when the game submits them, as on the desktop: at most
four compressed packets of 2,304 bytes per stream, about 64 KB of PCM a
stream. Not modelled, as on the desktop: Doppler, cones, the
high-frequency filters and I3DL2 reverb (the game's own DSP effects image,
which the retail DirectSound would download to the global processor, is
not used).

## Performance audit (2026-09-29)

Measured first, from the frame statistics the user's sessions logged:
steady play holds the game's 30 Hz (34 ms a frame), but the heaviest
scenes fell to 100-124 ms a frame with 1,000-1,500 draws, while the
device's own share of the processor stayed at 3-5% and it never waited for
the GPU. The time went to processor work outside the device, and in the
emulator that work is dearer than on the console in two ways: x87
arithmetic is an 80-bit software one (a helper call per operation, where
SSE's single precision runs on the host's own), and every memory access
goes through a software memory unit. What changed:

- **nxdk's libraries were unoptimised.** nxdk adds no optimisation flag of
  its own and the sample that built them set none, so the C library,
  pbkit, the USB and network stacks and the Win32 layer were `-O0` code.
  They are built with `-O2` now (Requirements).
- **The C library's memcpy, memmove and memset copied a byte per loop
  pass** (some fifteen instructions a byte unoptimised). `src/nx_string.c`
  moves words with the string instructions, for every caller.
- **Single precision in SSE** (How the port operates): the game's float
  arithmetic left the x87, which cut the image's x87 instructions from
  some 119,000 to 12,000 and its control-word loads (float to integer
  conversions) from 997 to 341, and made its results the other ports'.
- **sqrt and fabs inline**: 577 calls into the C library became the x87's
  instruction.
- **The software mixer in integers** (Sound): some thirty x87 operations
  on every frame of every voice, in a thread that runs ahead of the game.
- **A processor report** every five seconds: `cpu: ... a pass X ms: ticks,
  render, texture, idle, other` from the game's own profiler (idle is the
  wait for the next vertical blank), and the mixer's milliseconds a second
  in the audio processor line.

A second round, from reading the device and the game for per-frame costs:

- **The push buffer's fast path.** Every state the game set went through
  some 115 instructions and five calls to write two words: the ring's room
  check, a trace call, and a walk of the block's headers that read them
  back from the ring's write-combined memory (slow on the console). The
  ring layer now keeps two bounds (`nx.h`: `nx_gpu_room_end`,
  `nx_gpu_kick_at`) that the device's pushes check inline; the header walk
  is a debugging build's (`NX_GPU_VALIDATE`). The GPU is told of new
  commands every 32 KB (the kick-off size the game asks for) rather than
  every 4 KB, with the free-space count corrected for the unflushed part.
  Deferred render and texture stage states whose value the tables already
  hold are not marked dirty again.
- **The vertical blank thread flushed the ring** from its own thread while
  the renderer wrote it: at the ring's wrap that could tell the GPU a
  stale position. It only waits now, runs above the game (as the retail
  callback ran in the interrupt), and makes up a blank it missed from
  pbkit's own count.
- **Clock reads.** The collision code timed every query for a display that
  is off, two clock reads each through the Win32 thunks; the profiler's
  timestamps went the same way. Collision timing now runs only while its
  display is on, the profiler reads the time stamp counter directly, and
  `GetTickCount` and the performance counter skip the trace.
- **The log.** Every line was drawn glyph by glyph on nxdk's text screen,
  hidden behind the game (and cleared, 1.2 MB, every page), trapped into
  the kernel's debug print, sent on the network from the printing thread
  under lwIP's lock, and flushed to the disk. Now the lines go to the ring
  and a sender thread every 50 ms; the screen gets them only when a crash
  shows it (with the ring's last lines replayed); `debug.txt` is flushed
  every 16 lines or 2 seconds. A distributed-netcode client also stopped
  logging three lines a tick ("failed to get an update"): its update ring
  is not used.
- **Waits.** The frame pacing waits on an event the vertical blank thread
  sets, instead of spinning on `GetTickCount` (which held the processor
  from the other threads of the game's priority); an alertable zero wait
  yields; the audio processor's thread naps 8 ms against its 64 ms lead.
- **Debug work nobody sees.** The AI's path debugging storage (a 115 KB
  clear and an 82 KB copy per path search) is kept only while an AI debug
  display is on; the texture and sound caches' block walk on every
  allocation and their "stabbed" dump to a file are skipped; the per-frame
  vertical blank debug string is built only for its display.
- **Memory.** The frame interpolation's snapshots (unlocked frame rate)
  grew with every map; they are freed with the old map, grow in powers of
  two, and the table is the game's size rather than the editor's. The
  sound streams' packet queues hold 8 (the game attaches 4) instead of 64.

## Bug audit, multiplayer first (2026-09-29)

Read for what real play over the internet could run into (loss, delay,
hostile packets, players coming and going, long sessions); what was found
and changed:

- **A tunnel peer could reach any local port.** A peer's stream named the
  port to connect to on this machine, and its datagrams the port to send
  to: any holder of a listed game's token could reach the telnet console
  (port 23 on the console and on Windows, where it evaluates scripts) or
  anything else listening locally. Streams now connect only to the game's
  server port (5150), datagrams, relayed searches and answers only between
  the game's ports (5150, 5151), and the stand-in listeners take only this
  machine's connections (on the console the loopback is its LAN address).
- **The master server** took any message for any identifier from anywhere:
  one packet could take over a listed game (its searches, its joiners) or
  delist it, and spoofed searches made it a traffic reflector. Identifiers
  are now bound to the endpoint they said hello from (taken over only after
  a silence), each address has a packet budget and an identifier cap,
  searches a minimum interval, and hosts that never answer probes stop
  being probed; the service restarts however often it stops and has a
  memory ceiling; the status page no longer calls the server down when the
  viewer's clock is off.
- **The peers' table** (127 records) was shared by games only listed by the
  master (anyone can answer a search) and machines being connected (anyone
  with a listed token can ask to join): a flood of either hid real games
  and turned players away. Listed-only games take at most 32 records (the
  oldest giving way), pending connections 8, a peer's streams 8, and a
  joiner's candidate addresses may not be unusable ones (0/8, multicast,
  broadcast) for the host's punches to go to.
- **KCP** aborted on an allocation failure (and a NULL segment was used);
  both are now a lost packet, which it sends again.
- **The protocol version** named two layouts: the released builds' 128
  players and this layout's 16 both said 2, so each listed the other's games
  and joining failed. This layout is 3 (big sessions keep 2).
- **Split-screen games were listed** on the master server as internet games
  (their server listens too); they are not now.
- **LAN joins went through the internet** whenever the host was also
  listed (it is, whenever it is online): a host that answered from the LAN
  lately is joined on the LAN.
- **The console's random bytes** (tokens, tunnel keys, nonces) came from a
  linear congruential generator whose byte stream a public identifier pins
  down: now an HMAC-SHA-256 generator (nx_net.c). The console's errno was
  one for all threads (PDCLib's); each thread has its own now. lwIP's
  "socket table full" maps to Winsock's too-many-sockets.
- **Internet play's thread** runs above the game on the console, so the
  tunnel socket is drained even while the game waits for a vertical blank
  (a socket holding the network card's 64 receive buffers stops the card).
- **The controllers' rumble** was a USB control transfer the game's
  high-priority rumble thread waited out (spinning, ahead of everything),
  while the game's thread polled the same USB stack, which has no lock:
  the transfer is made by the game thread's next poll now.
- **The log could deadlock**: a print took lwIP's lock, so an nxdk assertion
  inside lwIP hung the printing thread (and then the sound and the log).
- **The master's injector socket** dropped out of the p2p thread's select
  set past 85 sockets (a count compared with a remainder).

In the distributed netcode (`port/linux/game`, `port/linux/NETCODE.md`):

- **A client's hit reports** passed the host's checks with a NaN position,
  carried their own damage flags (killing outright, past shields), scale,
  multiplier and owner, and an unchecked region index that one message could
  use to write past the host's object memory. Every float must be finite
  and the range checks fail on NaN; the region must be the target's; the
  flags are the hit's own ones; the scale is 0 to 1, the multiplier the
  host's, the owner the reporting player's unit (or its vehicle) and team;
  and the rate limit is taken before any tag is walked.
- **A late "dead" state killed the next life**: the unreliable per-tick
  states applied in any order, and a dead state killed whatever unit the
  player had. The snapshots (unit states, where objects are, inventories,
  statistics, predictions) now apply in order per sender, and a dead state
  names the unit that died.
- **Predictions** from a client (its own player's and vehicle's) must be of
  the current life, finite, and one a player a tick (each moved the unit
  from where the last left it).
- **Quits** used the host's tick on a client's own clock, which may be past
  it: the player was never removed, and a line a tick went to the log.
- **Clients take the netcode's messages from anyone**, not only from their
  host (still open). Checking them against the host's address, as the game
  checks its other server messages, dropped every per-tick state over
  internet play: a client's unreliable socket is connected to the tunnel's
  local stand-in, and the socket layer translates a peer name back to the
  host's virtual address for TCP stand-ins only (`xnet.c`,
  `halo_ws_getpeername`), so every client waited for its unit ("Waiting for
  space to clear"). The check comes back once the translation covers UDP.
- **Stale inventories gave grenades back and undid reloads** (a host count
  sent before it had the client's shots, taken for a pickup). A client's
  own players' ammunition, batteries and grenades take the host's counts
  once they have been still for 1.5 seconds, exactly, both ways.

## The dashboard's video settings, performance mode, the frame rate (2026-09-29)

- The console's dashboard settings (the EEPROM's video flags, nxdk's
  XVideoGetEncoderSettings) decide the display mode as they did for retail
  games (nx_video.c, at the GPU's start): with the HDTV pack and 480p
  enabled, the mode is progressive (nxdk's XVideoSetMode would take the
  interlaced mode first in its table, so the progressive one is set through
  nxdk's XVideoInit with its code); with widescreen enabled, the game draws
  854 columns, as the desktop builds do on a 16:9 display (the view is
  16:9, the menus are centred, the HUD sits at the edges), into a 720x480
  buffer (the mode table's 720-column mode), and the encoder is told the
  picture is 16:9 (AvSendTVEncoderOption's AV_OPTION_WIDESCREEN, 18), so the
  television stretches it back. The device does the squeeze (d3d8_nv2a.c:
  logical_width, scale_x): viewports, clears and visibility counts on the
  screen's targets are scaled, the menus' offset (halo_screen_ui_offset)
  shifts the game's coordinates, and device.viewport stays in the game's
  columns. The frame taken as a texture is addressed in the buffer's
  texels, so the effects that do so squeeze their coordinates by
  halo_screen_texture_scale_x (the screen effects' texture transforms, the
  sun glow's source copy, and since 2026-09-30 active camouflage's screen
  projection and the loading screen's). The log's "video:"
  line says what was found and chosen, and "screen: 854x480 drawn in
  720x480" follows. In xemu the window's shape is its own setting: 16:9 (or
  Auto, where xemu honours the signal).
- 720p: not on a 64 MB console, whose room it does not have; on a 128 MB
  one since 2026-09-30 (see "720p"). 1080i is not offered.
- Performance mode: a `performance.txt` on the disc (`tools/xbox_disc.py
  --performance`; on the desktop, next to maps/) turns off, at the
  rasterizer's start (rasterizer.c, rasterizer_performance_mode), the soft
  shadows and the environment's shadows, dynamic lights, specular
  lightmaps and reflections, grass, bump mapping, the sun's rays, the lens
  flares' visibility tests (until 2026-10-01: without them every flare drew
  through the walls as a white glow) and active
  camouflage's second pass, and stops the terminal's lines from being drawn
  every frame (interface.c; the console itself still shows). The device's
  frame statistics stay quiet in it.
- `display.unlocked_framerate` (config.toml on the disc, `--setting
  display.unlocked_framerate=true`): frames between the game's 30 Hz ticks
  with movement interpolated (port/linux/game/render_interpolation.c,
  halo_interpolation_enabled), up to the display's 60; off draws one frame
  per tick as the retail game did.

## Zoom and the screen effects: the depth buffer as a colour surface (2026-09-29)

Zooming with a scoped weapon filled the screen with noise, and the effect
could end in a crash. The scope is a screen effect
(source/rasterizer/xbox/rasterizer_xbox_screen_effect.c): pass 0 draws the
back buffer, as a texture, through the mask into the "render primary copy"
target; pass 1 draws that copy back. The retail game puts that copy in the
depth buffer's memory: rasterizer_xbox.c builds the copy's surface and
texture headers from the primary surface's and takes the depth surface's
Data as their address. The device had reported its depth buffer at offset
0 of pbkit's depth DMA window, so the copy's address was 0: the render
target mapping made for a30 sent pass 0 into the back buffer itself, and
pass 1 sampled physical address 0, the kernel, as its texture.

The device now owns its depth buffer (nx_gpu_depth_buffer, contiguous
memory) and addresses depth through the same all-memory DMA window as
colour, so the depth surface's Data is a physical address like any other
surface's, the copy lands in real memory, and depth surfaces the game
creates itself are addressed correctly as well. The window is re-bound
after pbkit's flips along with the colour one. Two smaller things came with
it: the game's two headers for "the back buffer" (one per frame parity,
rasterizer_set_target_as_texture) cache whichever of pbkit's three rotating
buffers they first saw, so the device substitutes the current back buffer
for any texture or target that names one of them (frame_buffer_seen,
is_frame_buffer); and the a30 mapping of an offset-0 target to the back
buffer stays as a safety net.

## The a30 cinematic and the console's memory (2026-09-29)

- The game keeps a D3DSurface header of its own for the back buffer,
  640x480 A8R8G8B8 with Data (the offset) 0 and the retail descriptor
  0x271df27f, and its cinematics set it as the render target. On the retail
  runtime the colour DMA window started at the frame buffer, so offset 0
  was the screen; the device's window spans all of memory, so offset 0 is
  physical 0: the kernel. a30's opening rendered a frame over the kernel's
  interrupt table and the executable's data (the dumped IDT entries held
  ARGB pixels), the next timer interrupt found its gate broken, and the
  console triple-faulted (xemu: "paused (shutdown)"; the `-d int` log's
  #GP with an IDT error code, then #DF) some 35 s into the level; the
  cinematic itself showed black or white. `D3DDevice_SetRenderTarget` maps
  a target with Data 0 to the real back buffer, and `apply_render_target`
  refuses anything below the game state (physical 0x61000). a30 plays.
- Found with hardware write breakpoints (DR0-DR3 on the IDT entries: no
  CPU write) and a `pmemsave` of the IDT from the stopped emulator.

## Frame rate: the push buffer's kicks (2026-09-29)

- pbkit tells the GPU about new commands in `pb_end`: a write-combine
  cache flush (a register write and a polling read) and the PUT register,
  three or four register accesses that an emulator turns into VM exits. The
  device called it after every Direct3D call, tens of thousands of times a
  frame, and the user saw 5 to 12 fps where the retail game holds 30. The
  ring layer (nx_gpu.c) now keeps its own write pointer and kicks every
  4 KB (`nx_gpu_flush`), and before anything that waits on the GPU: the
  ring wrap, `nx_gpu_drain`, the flip queue, the vertical blank, a pending
  visibility test result. Every pbkit call that pushes on its own
  (`pb_finished`, `pb_reset`, the debug screen) is preceded by a flush and
  followed by a resync of the pointers.
- The present no longer resets the ring each frame: `pb_reset` waits for
  the pusher to reach the head, a full drain that serialised the CPU and
  the GPU. The ring wraps on its own (a jump at the tail, PUT to the head,
  and a wait only if the pusher is still reading the region about to be
  written).
- Without the per-frame reset the pusher can be a whole lap behind the
  CPU, so the room check is a free-space computation against the pusher's
  fetch pointer (GET): a GET beyond the write position in address is the
  previous lap (free up to it), a GET behind it is this lap, a GET at PUT
  is an empty ring (`pusher_free_dwords`). The first version only checked
  whether GET lay inside the block about to be written and let the CPU
  pass the pusher; the GPU then executed overwritten commands and the
  console triple-faulted minutes into a level. The tail keeps a 1024-dword
  margin (pbkit's flip pushes, blocks over their reservation, which
  `push_end` reports), and pbkit's own pushes get room first.
- Visibility test results (the lens flare occlusion tests, which the game
  spins on in the frame that issued them) are answered from the slot's
  previous frame when the GPU has not reached them yet: on the console the
  GPU runs close behind; here the spin was the whole frame's GPU work.
- Every 120 frames the log gives the frame statistics: ms per frame,
  draws, kicks, KB pushed, ms waiting for the ring, ms in present, the
  share of CPU cycles inside the device's draw, constant, immediate-mode,
  texture and shader entry points, and stale query answers. Under the
  headless rig (software CPU and GPU) a level runs at 40 to 50 ms a frame
  with the device at a tenth of the CPU. The log also goes into a 16 KB
  The pusher's GET register is read only when the cached free count runs
  short (`free_cache`), and consecutive immediate-mode draws (a string's
  glyphs) skip the vertex-array disable after the first.
  ring in contiguous memory (start-up prints its physical address) for
  `pmemsave` from a stopped emulator; the rig's launcher logs CPU resets
  and exceptions (`-d int,cpu_reset`), which is how the triple fault above
  was seen.

## GPU push buffer and USB (2026-09-28)

- pbkit's `pb_reset` gives the GPU 100 ms to reach the head of the ring and
  then carries on; a slow frame (software rendering) then had the CPU
  overwrite commands the GPU had not read, and xemu died with "Reserved pb
  command". `nx_gpu_reserve` (nx_gpu.c) now waits for the pusher's GET
  (NV_PFIFO_CACHE1_DMA_GET, 0xFD003244) to leave the words about to be
  written. `push_end` validates every block's headers (d3d8_nv2a.c).
- Rumble goes out as a HID SET_REPORT class request (nx_input.c); the same
  6-byte report on the interrupt OUT endpoint made xemu's USB core assert
  (`usb_packet_copy`) and took the emulator down at the first rumble.
- The retail UI tags lack the wrapper the January profile list expects
  (`player_profile_1wide_list_update`); the Xbox build returns instead of
  halting.

## First test on a real console (2026-09-30)

The build that runs in xemu exited on a real console (128 MB) within
seconds of starting, after the game had started the GPU. What the screen
showed was not the error:

- Once the GPU shows the game, log lines go only to the ring; a crash or
  exit shows the text screen again and replays the ring's last 1600 bytes.
  nxdk's `debugPrint` formats into 512 bytes, so the replay showed its
  first 511 bytes (stack walk addresses) and cut off the rest, which held
  the assertion. `screen_print` (nx_kernel.c) prints in pieces now, and the
  exit screen is a summary (see "The log").
- The last I/O before the failure was the first 2048-byte write of the map
  cache's set-up (`cached_map_files_initialize`: a blank header, then the
  file extended to its full size). The six files take 732 MB of E:, which
  a console with games copied to E: may not have; xemu's image has room.
  Unconfirmed as the cause, but the files now go where there is room: E:
  when it has it, otherwise the cache partitions Partition4 and Partition5
  (750 MB each, mounted as Y: and W:), chosen at start-up from their free
  space (`place_map_cache`; the log names the places and each partition's
  free space).
- The network log also broadcasts on a real network, and I/O reports go
  through the ring.
- nxdk's PDCLib has no floating point in its printf: `%f`, `%e`, `%g` print
  nothing and leave their double on the argument list, so every later
  argument is read out of step. The host's crash in a game with the Windows
  build (a player pressed action with nothing to pick up:
  `network_player_log_idle_action`'s `%.2f` values, then its `%s` took the
  low half of a double, 0, as its pointer) was this, as was the "garbage
  name pointer" of 2026-09-29, which a check of the tag index did not
  cure; the platform log's `cpu:` line printed empty numbers the same way.
  msvc_crt.c now formats the floating-point conversions itself (musl's
  method, integer arithmetic only; checked against the Microsoft C
  runtime's printf on 290,000 values and formats) and passes them to
  PDCLib as strings, and the platform layer's `snprintf` family goes
  through the same functions (crt/stdio.h).
- The real NV2A checks method data that xemu does not; pbkit reports the
  trap and stops the console (the report now starts on a cleared screen,
  since nxdk's text screen pages). Found so far on a console, each an
  "invalid data error": NV097_SET_TEXTURE_FILTER with the convolution
  kernel field (bits 13-15) 0 (it must be 1 or 2 whatever the filters;
  values 0x02040000 and 0x01010000), and NV097_SET_SURFACE_FORMAT pairing
  a 16-bit swizzled target (a texture the game renders into) with the
  32-bit depth format (0x07070223): a swizzled target now gets a depth
  format of its own size and no depth buffer of another. Also fixed from a
  comparison with libs/d3d8: the shadow function went to 0x1D74 (a
  semaphore method) instead of 0x1E6C; Present re-targeted only the DMA
  windows when the target was not the back buffer, leaving pbkit's colour
  and depth offsets of 0 (physical 0, the kernel); clears could pass the
  720-column surface while the menus are centred; a single-level texture
  gets a single-level filter.
- Particles and other dynamic geometry were torn and oversized on a
  console: the game rewrites its dynamic vertex buffers from their start
  each frame after a lock that, on the retail runtime, waited for the GPU
  to finish with them; here it did not wait, and the CPU overwrote
  vertices the GPU (a frame or two behind on a console) was still
  drawing. A lock without D3DLOCK_NOOVERWRITE or D3DLOCK_READONLY now
  waits for the GPU to finish with that resource (at first by draining
  the GPU; since the performance audit, by fence), and resources are
  write-combined as the runtime's were (d3d8_resources.c, HALO_XBOX).
- xemu with its network bridged (pcap) onto the network card of the
  computer that runs the Windows build cannot reach that computer: the
  console's joins timed out while the PC could not be reached, and worked
  at once with xemu's NAT network. Two machines on a network do not have
  this problem. Seen again 2026-10-01: the console's packets reached the
  PC (its log, its LAN game answers, its tunnel punches), but nothing the
  PC sent reached the console (no ping answer, port 5150 silent while it
  hosted). The tunnel takes one packet heard as connected, so the PC's
  side said "connected to player" while the console's never heard back;
  the PC's direct LAN join to the console failed after the game's 1 s
  connect wait.

## Performance audit for the console (2026-09-30)

What cost the real console time, and what changed. None of it lowers the
graphics.

- Resource waits by fence (d3d8_nv2a.c). A waiting lock drained the whole
  GPU. Now the device writes a fence number into the report page at the
  end of each frame (NV097_BACK_END_WRITE_SEMAPHORE_RELEASE), marks every
  vertex buffer, texture and palette a draw uses with the next fence, and
  a lock waits only until that resource's fence has passed (the retail
  runtime's BlockOnResource). `D3DResource_IsBusy` answers the same way.
  The map file's vertex and index buffer headers carry junk in their Lock
  words, which the first build took for a fence far ahead and waited on at
  every map change (loading never finished): `D3DResource_Register` clears
  the word, and a value beyond the next fence counts as no use.
- The CPU's lead: Present waits (asleep) until the GPU has drawn the frame
  before, so the CPU is at most one frame ahead, as the game expects. With
  `display.unlocked_framerate` nothing else held it back, pbkit's flip
  queue let it run three frames ahead, and on a console (the emulator's
  GPU keeps up) the game rewrote what the GPU had still to draw: heavy
  artifacts in that mode only. Fence waits sleep after a short spin, a
  busy answer to the texture cache yields, and Present yields once a
  frame, so the threads of the game's priority (the network stack's) are
  not held off while the renderer waits or runs flat out.
- Visibility tests (lens flare occlusion) by frame parity: the game numbers
  its tests within a frame and reads them in the same frame. With the CPU a
  frame ahead, the GPU's reports for the frame before landed in the slots
  this frame had just marked pending, and the game took another frame's
  (often another flare's) answer: flares flashed white over the walls with
  the frame rate unlocked. The report page has a half (127 tests) for
  each frame parity, and an unanswered test is waited for up to 2 ms a
  frame before the number's last answer serves.
- The GPU's vertex cache: the retail runtime emptied it
  (NV097_INVALIDATE_VERTEX_CACHE_FILE) at every vertex buffer lock without
  D3DLOCK_NOFLUSH; the port did not, and the game rewrites its dynamic
  buffers at the same addresses every frame, so a real NV2A can draw a
  vertex it fetched the frame before. xemu has no such cache.
- Texture registers: every texture change sent all four stages' address,
  control and filter registers again before the next draw. The device
  now remembers what each register holds and sends only changes; the
  shader stage program likewise.
- Rumble: the game sets the motors every tick, and each set was a USB
  transfer with a busy wait. Now at most one every 60 ms unless a motor
  stops or the level changes noticeably (nx_input.c).
- The device's CPU timers (two `rdtsc` per call) are built only with
  `-DNX_DEVICE_TIMERS`.
- Sound: the AC97 ring is write-combined with a fence before the hardware
  reads it, and the mixer thread runs above normal priority.
- `memcpy`/`memset` under 16 bytes skip the string instructions' set-up
  (nx_string.c); the printf family passes formats without floating point
  or strings straight to PDCLib (msvc_crt.c).
- Joining a game asked the host to add the player every frame until it
  answered; now once a second per controller
  (ui_widget_game_data_input_functions.c, HALO_LINUX), which spared both
  machines' logs and CPUs the flood.
- The asynchronous I/O completion scan checks its 64 slots without the
  lock first.

Not done: `-O2` for the hot game units (it grows the image, and the
Pentium III's cache may lose what the compiler gains; worth measuring on
the console), and caching the vertex array formats.

## 720p (2026-09-30)

`display.hd_720p = true` in the disc's config.toml (`--setting
display.hd_720p=true`) asks for 1280x720. The game switches when all of
these hold, and otherwise stays at 480 and logs why ("video: display.hd_720p
is on, but ..."):

- the HDTV (component) pack, with 720p on in the dashboard, NTSC;
- a console with 128 MB whose top 64 MB the title can use. Only the 720p
  disc's XBE loses the kernel's 64 MB limit (`tools/xbox_disc.py` runs
  `tools/xbox_xbe_flags.py` on it when packing with `display.hd_720p=true`;
  for a hard disk copy, `default-720p.xbe`), and at start-up the log says
  whether a page there could be had ("memory: the top 64 MB of a 128 MB
  console is there").

The first build that left the limit off everywhere failed at start-up on a
128 MB console: the tag cache's fixed 22 MB at physical 0x3A6000 could not
be allocated (27,450 of 32,768 pages free; something already sat in the
range). The build's XBE keeps the limit again, and the game's two fixed
blocks (the game state at 0x61000, the tag cache at 0x3A6000) are now
reserved first thing at start-up, before the video mode, the GPU, the
network or the sound allocate anything, and handed to the game when it
asks (nx_kernel.c, reserve_fixed_blocks). If one still cannot be had, the
start-up lists the taken 64 KB spans of its range.

How it works:

- The frame is rendered at 1280x720: the GPU rasterises every pixel into
  pbkit's 1280x720 buffers with a 1280x720 depth buffer; nothing is drawn
  at 480 and enlarged. The game's own coordinates stay in its 480-line
  layout (854x480 at 16:9, as the desktop builds do at any resolution),
  and the device maps them onto the 1280x720 pixels: viewports, clears,
  visibility counts and point sizes (d3d8_nv2a.c, scale_x and scale_y).
  The effects that read the frame as a texture scale their texel
  coordinates by `halo_screen_texture_scale_x` and `_y` (screen effects,
  the sun glow copy, active camouflage, the loading screen).
- pbkit's three 1280x720 buffers and its depth buffer take 14 MB of the
  low 64 MB (4.7 MB at 640x480). To make room, everything the game
  allocates "anywhere" goes to the top 64 MB from then on (the texture
  cache, now the retail 22 MB, the sound cache, every Direct3D resource
  and the device's depth buffer: nx_physical_alloc), and the GPU's
  all-memory DMA window reaches the top (its colour, depth, texture and
  vertex contexts). The mode is chosen at the game's first question about
  its screen, before the GPU starts and before the game's memory map.
- 720p is 2.25 times the pixels of 640x480, so heavy scenes run slower;
  performance mode helps.

On the user's 128 MB console (a retail kernel with a 128 MB BIOS) the
kernel gives no contiguous memory above 64 MB, even with the limit off:
it hands the top half out only as ordinary pages. 720p then fits the low
64 MB instead, if a trial at the mode's choice finds the room
(nx_video.c, low_memory_fits_hd): pbkit keeps two frame buffers instead
of three and no depth buffer of its own (the device has its own), Present
waits for the flip before the next frame draws into the buffer that was on
the screen, the texture cache is 10 MB, and the sound cache, which only
the processor reads, is ordinary memory (halo_xbox_sound_cache_alloc).
With `display.mode_notice` on (off by default), under the frame rate
counter the game shows "720p", "720p (in 64 MB:
double buffered, N MB textures)" or why not.

A stock 64 MB console (2026-10-01): nxdk's text screen, which
`XVideoSetMode` makes at the mode's size (1280x720x4, 3.5 MB) for start-up
and crash text only, is also the device's depth buffer, the same size
(nx_gpu_depth_buffer; the crash screen clears it first and waits for the
GPU), and the texture cache is 9 MB there. Then 720p leaves about as much
memory free after start-up as 480 does (6.4 MB in the emulator at 64 MB),
and the trial asks for that room in 1 MB pieces. The frame statistics now
log `memory: N KB free (lowest M KB)`. The 22 MB tag cache cannot give
room: every map loads its structure (BSP) at fixed addresses at the cache's
top, and the campaign's tag data reaches nearly to them (b40 leaves 0.2 MB;
the multiplayer maps leave 13 to 15 MB in the middle, but the cache is
allocated once). Untested on a console yet. In xemu: System, Memory 128 MiB, the AV pack set to HDTV,
720p on in the dashboard.

What 720p taught (2026-09-30 and 2026-10-01):

- nxdk's `XVideoInit` switches the encoder but leaves the mode
  `XVideoGetMode` reports, which pbkit sizes its frame buffers from: 720p
  went out of 720x480 buffers (a psychedelic picture). The mode is set
  through `XVideoSetMode`.
- A 64 MB console's ordinary memory is the same 64 MB, so the trial counts
  the sound cache there and the USB driver's pool: in xemu at 64 MB the USB
  driver found 83 pages left (with nxdk's text screen still apart from the
  depth buffer).
- Double buffering: pbkit flags a frame ready only when the GPU reaches its
  end, so right after `pb_finished` nothing was pending and the next frame
  drew into the buffer on the screen (tearing at the bottom, in the clear
  colour). The patch's second version counts frames finished against flips
  done (`pb_flip_pending`).

## Second audit, the scope, the menus, the debug settings (2026-10-01)

- The scope on a widescreen display: `rasterizer_set_target` sets the
  viewport from `GetDesc`, the surface's own 720x480, for the frame's copy;
  the device took that in the game's 854 columns and drew 607 of the
  buffer's 720, so the scope's second pass read the rest stale. A viewport
  of a screen target's whole surface now means the whole screen
  (`D3DDevice_SetViewport`).
- White geometry and flickering particles at 60 frames a second: the texture
  cache took a texture the GPU might still draw from as locked; with the CPU
  a frame ahead that was every texture of the frame before, the cache split
  into holes too small for the next one, which drew as the default (white)
  bitmap ("YOU GOT STABBED" in the log). Such a texture can go now:
  deleting it waits for the GPU (`texture_cache_locked_block_proc`,
  HALO_XBOX).
- The menus say "ONLINE PLAY" for "SYSTEM LINK PLAY", and the system link
  messages say online (`unicode_string_list_get_string`, text_group.c).
- Settings: `debug.console` (the developer console and its lines on the
  screen), `debug.log_file` (debug.txt), `debug.network_log` (the console's
  UDP log). The two logs are off by default (the user's choice, 2026-10-01):
  turn them on to report a problem.
- Performance mode no longer turns lens flare occlusion off: every flare
  then drew through the walls, a white glow on the geometry (the likeliest
  "geometry going white" at 60 frames a second, which only the performance
  disc runs).

The audits' fixes (a bug audit and a performance audit, 2026-10-01):

- The `cpu:` line passed the profiler's self times: the frame's was the pass
  less its ticks, render and idle, the render's left out the windows (the
  scene), and "5 s" covered a minute. It passes the sections' totals now.
  The frame statistics line shows in performance mode too, with the time
  spent waiting for visibility answers.
- `-O2` for the hot units (math, collision, animation, the scene's render,
  particles, the device, the push buffer, the mixer: `XBOX_HOT_UNITS` in
  tools/xbox_build.py); `csmemcpy`/`csmemset` inline on the console (the
  builtins, no checks); `square_root` as SSE's `sqrtss`.
- The device sends the vertex array formats and offsets only when they
  change; a fence wait yields for 3 ms before it sleeps (a sleep wakes on
  the kernel's 1 ms tick); visibility tests reach the GPU at once.
- Interpolation works out each node's rotation once a tick, not every frame.
- The mixer's chunk is 256 frames on the console: the game's sound calls
  wait a quarter as long behind its lock.
- A refused render target at Present now means the back buffer: before,
  nothing was pushed and pbkit's colour offset 0 (the kernel) stood.
- Local games (split screen) with no cable or no DHCP: lwIP with a single
  interface routed nothing to 127.0.0.1 then, and a looped-back packet was
  only taken on an interface with an address; lwIP's loopback interface
  carries it now (tools/xbox_lwip_patch.py, and nx_net.c no longer swaps
  127.0.0.1 for the console's address, which needs the cable). AutoIP when
  DHCP does not answer, a 32-entry ARP table with queueing, and the link as
  the cable's (`posix_network_link_up`). The console's address is asked
  again whenever the game wants it (`transport_get_xnaddr`): it was
  0.0.0.0 until DHCP answered, and a late DHCP answer replaces an AutoIP
  address the LAN cannot reach; the log shows each change. The network log
  starts when DHCP is slow too (it waits in the ring for an address).
- Internet play ignores packets from a peer only listed by the master
  server (its key is zeros until signalling offers one).
- A controller whose report transfer fails is read again (the endpoint's
  halt cleared) instead of freezing with its last report, at the USB
  DPC's level; the USB stack claims its descriptors atomically
  (tools/xbox_usb_patch.py), since its DPC and the game thread both submit
  transfers.
- pbkit: a vertical blank the DPC handled itself (the ISR had the
  interrupt masked) counted a flip it never made, and a double-buffered
  frame was drawn on the screen (tools/xbox_pbkit_patch.py, version 3).
- Interpolation grows its per-object arrays with fresh blocks: the game's
  debug realloc unregisters the old block before it tries, so a failed
  one left a pointer that halted at the next free.
- `graphics.weather` hides rain and snow (nothing read its flag; weather
  went with `graphics.particles`).
- Smaller: `--setting` values are written in the setting's type
  (`display.hd_720p=True` was a string the game ignored; numbers in
  spellings TOML reads, and the file is checked before packing), waits take
  64 handles at most, the I/O lock is made at start-up, the pthread shim's
  wait sleeps, a failed broadcast keeps its error.

Checked again (2026-10-01, afternoon), and fixed:

- A crash report owns the GPU: the thread that shows the console makes
  every other thread that pushes park at its next push (the game kept
  drawing on its own thread, and at 720p in the low 64 MB the report's
  screen is the depth buffer, which every frame clears); the wait for what
  was queued is bounded (200 ms) and skipped at a raised IRQL, where the
  GPU's traps cannot be served (the firmware path shows the report again
  once it has lowered it). pbkit's pusher error now halts as its GPU error
  does (patch version 5).
- The drain and idle waits read the engine's status register instead of
  calling pbkit's `pb_wait_until_gr_not_busy`, which ran the interrupt
  handlers from the thread and could count a flip twice.
- Visibility tests get their report slots in the order they come (by
  number, flare 127 shared flare 0's, and the transparent counter's 0xfff
  flare 31's), each number remembers its slot and frame, and an answer the
  GPU has not given within the wait is "not ready": the lens flare keeps its
  last visibility (rasterizer_lights.c, HALO_XBOX) instead of taking
  another flare's old count, a white flash through a wall.
- The decal buffer waits for the GPU only after a deletion that made room
  for a new decal, not after expiries (deleted before the frame draws).
- A map copy takes a 1 MB write buffer instead of 4 when the texture cache
  is the reduced one (720p in the low 64 MB): its buffers come out of the
  texture cache, and a copy in the lobby left 1.9 MB of textures.

## Performance round: AI on screen (2026-10-01, evening)

The user saw large drops in single player with AI on screen, which the
original does not have. An audit (eight areas, each finding checked by two
skeptics) found the drop to be a cliff, not a slope: Present held the CPU
until the next frame's buffer had left the screen, and at 720p in the low
64 MB (double buffered) that is the frame it had just finished, so the
ticks, the render, the GPU and the vertical blank ran one after another and
frame times snapped from 16.7 to 33.3 ms once a few more AI were drawn. The
rest is cost the original did not have, each a little per AI: the software
mixer (the retail game used the APU), shadow and light passes performance
mode never draws, x87 rounding calls, interpolation for every object in the
map, and checked-build accessor calls. Every change keeps the game's
behaviour and pictures; the mixer and ADPCM decoder were compared bit for
bit on the host over millions of cases, matrix4x3_multiply and fraction_of
too.

- Pacing: Present leaves the buffer wait and the re-target to the next
  frame's beginning (`resolve_pending_present`: the game's frame_begin,
  any draw, clear or back-buffer question), so the ticks run while the GPU
  finishes and the display flips; the flip wakes the wait (an event the
  vertical blank thread sets), not a 1 ms sleep. pbkit no longer re-targets
  its own DMA objects at each flip (patch version 6: eight GPU interrupts a
  frame); the device binds its DMA windows once. Double buffered, the GPU
  is kicked every 8 KB and after the frame's clear.
- Audio: integer mixer kernels (`dsound_mix_kernels.c`, -O2 without a frame
  pointer), voice gains worked out only when something changed, ADPCM from
  tables.
- Performance mode (or `graphics.shadows`/`dynamic_lights`/`specular` off):
  the object shadow pass and the dynamic light loops are skipped when
  nothing of them is drawn.
- Rounding: float to integer as the retail code's FISTP (`halo_fistp`),
  not a call to rint with two control word loads.
- Interpolation: a snapshot's rotations are worked out when a drawn object
  first blends it, not for every object each tick; the first-person
  weapon's previous pose once a tick.
- Accessors: `TAG_BLOCK_GET_ELEMENT` and `datum_get` take a valid element
  inline (anything the checked function would stop on still goes to it),
  `tag_get` inlines its instance lookup; `matrix4x3_multiply` in SSE1;
  `fraction_of` for fmod(x, 1).
- AI and objects (`HALO_XBOX && !HALO_HEAVY_CHECKS`; `-DHALO_HEAVY_CHECKS`
  brings every check back): debug-only walks out of the tick (the obstacle
  heap's, the memory pool's full walk on every allocation, now once a tick,
  activation checks, AI profile sampling), unit and biped vector checks at
  the end of each update instead of between steps, path searches clear only
  what they use, marker lookups without repeated searches.
- Decals: each decal keeps the fence after its last draw, and a new decal
  waits only if the GPU has not passed the one it replaces.
- Memory: functions nothing calls are dropped at link (-ffunction-sections,
  -opt:ref), libtiff's fax tables and other dead units are left out, pbkit
  allocates no depth buffer at 480 either (1.3 MB): the image is 216 KB
  smaller, more than paying for ten more -O2 units.
- Logs: a `waits:` line every 120 frames (Present's waits: the GPU's
  previous frame, the yield, the flip queue, the buffer; how far behind the
  GPU is) and a `ticks:` line every 5 s (AI, units, effects, objects by
  kind, interpolation, actors and objects), and the audio lines' mixing,
  voices and decoding time.

Not done, for later: the depth buffer tiled and compressed (the biggest GPU
win the audit found, but only a console can check it), double buffering at
30 frames for a bigger texture cache, rumble without the USB stack's busy
wait, `--release` (no checks) for the console, the lwIP mailbox size.

## Rubberbanding over the internet (2026-10-02)

A remote player on a real console rubberbanded in the user's games (mixed
discs; the PC players in the lobby were idle). The client's queue for the
host's unreliable datagrams was the retail 1600 bytes, and the read loop
stops once less than a datagram's room is left, so one idle took in one or
two datagrams: fewer than the host sends a tick (object and unit states,
inventories, statistics). The rest waited in the socket, and a client at 30
frames or fewer applied the host's states seconds late; the late state of
its own player is past the local correction tolerance, so the player
snapped back. Not new with the last builds, but the locked and 720p discs
run at 30 frames or fewer, which shows it. Fixed:

- The client's unreliable queue is 128 KB, as the host's already was
  (`network_connection.c`).
- The game's TCP connections and the tunnel's stream stand-ins set
  TCP_NODELAY, so small reliable messages are not held back by Nagle's
  algorithm (`transport_endpoint_winsock.c`, `p2p.c`).

New log lines (the PC's `halo.log`; the console's with `debug.network_log`
on), every 5 s during a game:

- `net: host|client: N idles, N datagrams (at most N in one), N stopped by a
  full queue; ...`: the intake. "Stopped by a full queue" should stay 0.
  The host adds how many of the clients' predictions it took and refused,
  a client how many corrections it took for its own player and others (the
  largest distance) and how old the host's states were in ticks.
- `Internet play: host|player <id>: round trip N ms (least-most), datagrams
  N in, N out, longest silence N ms; N streams: round trip, timeout, N
  waiting, N segments sent`: the tunnel to each peer.

Both sides should run discs and a PC build from the same tree.

## Test rig

`python3 tools/xbox_disc.py --init "map_name a10" --out <test.iso>` packs a
disc whose init.txt loads a level at boot (the console runs the file's
commands), for a private emulator; the user's image is packed without it.
