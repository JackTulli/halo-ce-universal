# Xbox

`ninja xbox` builds the game for the original Xbox with
[nxdk](https://github.com/XboxDev/nxdk), the open-source Xbox SDK. The
result is `build/xbox/default.xbe`. The game runs on the console's kernel
with its own Direct3D device over the NV2A, sound through the console's
audio hardware and USB controllers, and it plays system link and internet
games with the desktop builds. So far it has been tested in xemu only.

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

  (run from the repository's root). nxdk needs `clang`, `lld`, `lld-link`,
  `llvm-lib` and `llvm-dlltool` under those names in the `PATH`.

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
12 MB instead of 22.

### The log

The console shows the game's log (`debug.txt`, which goes to the cache
partition, `Z:`) and the platform layer's messages on a text screen. It
also sends every line by UDP to port 9999 of the machine beside it (the
host of xemu's user network, 10.0.2.2). To keep them:

    python tools/xbox_log.py --print

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
| Graphics | `src/d3d8_nv2a.c`: the game's Direct3D 8 device over nxdk's pbkit, driving the NV2A directly (the push buffer is `src/nx_gpu.c`). Resources are real (`port/linux/src/d3d8_resources.c`), frames are paced on the display's vertical blank, and the dashboard's 480p and widescreen settings decide the mode (`src/nx_video.c`). |
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
sound and controllers. System link and internet play work, also with the
Linux build. Memory is the constraint: this beta build's debug systems (AI
debugging, profiling) take several megabytes the retail build does not,
so the texture cache is 12 MB instead of 22. The dated sections below
record each step. "Bug audit" lists what is still open in the netcode.

## Next

1. Memory: trim the beta's debug systems to give the texture cache its
   22 MB back. That would also make room for 720p, which needs about 7 MB
   more and a change to pbkit's frame buffers.
2. Movies, once Bink or a substitute decoder exists.
3. The netcode items the bug audit left open.
4. A test on a real console.

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
- A local game's client connects to its own server at 127.0.0.1, and lwIP
  here has no loopback interface (LWIP_HAVE_LOOPIF is off), so the address
  is swapped for the console's own, which lwIP loops back
  (LWIP_NETIF_LOOPBACK), and swapped back in what the game reads
  (nx_net.c, to_lwip_address/from_lwip_address). The swap was there but
  keyed on nxdk's `g_pnetif`, NULL from the port's side, so hosting a
  system link game ended in WSAEHOSTUNREACH and "Failed to join the System
  Link game".
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
  sun glow's source copy; active camouflage's screen projection is not
  yet, so its refraction is a little off on widescreen). The log's "video:"
  line says what was found and chosen, and "screen: 854x480 drawn in
  720x480" follows. In xemu the window's shape is its own setting: 16:9 (or
  Auto, where xemu honours the signal).
- 720p and 1080i are not offered. A 1280x720 frame with its depth buffer
  needs about 7 MB more than the console has left mid-game with the port's
  caches, even double-buffered with one shared depth buffer, so it would
  only run by cutting the texture cache from 12 to about 6 MB.
- Performance mode: a `performance.txt` on the disc (`tools/xbox_disc.py
  --performance`; on the desktop, next to maps/) turns off, at the
  rasterizer's start (rasterizer.c, rasterizer_performance_mode), the soft
  shadows and the environment's shadows, dynamic lights, specular
  lightmaps and reflections, grass, bump mapping, the sun's rays, the lens
  flares' visibility tests (which the game waits on) and active
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

## Test rig

`python3 tools/xbox_disc.py --init "map_name a10" --out <test.iso>` packs a
disc whose init.txt loads a level at boot (the console runs the file's
commands), for a private emulator; the user's image is packed without it.
