# Halo CE on the Nintendo 64 (multiplayer split screen)

A port of the game's multiplayer to the N64 with the Expansion Pak:
Halo's own simulation (objects, physics, collision, weapons, game types,
players, first-person weapons) compiled for the VR4300, a new renderer on
tiny3d, and maps converted from the Xbox game's cache files. No campaign,
no networking, no sound yet. It starts at a menu of its own over the scene
behind Halo's main menu (the ring in space): 1-4 players join, choose the
game type, one of the 13 multiplayer maps and the score, and play; after
the scores, back to the menu. Settings has chunky mode.

`NOTES.md` is the working notebook (what the game needs from the left-out
renderer, how a split-screen game starts without menus).

## Requirements

Everything lives in `~/n64` (none of it needs root):

- libdragon's prebuilt MIPS toolchain: the `gcc-toolchain-mips64-x86_64.deb`
  from libdragon's `toolchain-continuous-prerelease` release, unpacked with
  `dpkg-deb -x` and its `opt/libdragon` moved to `~/n64/toolchain`
  (`N64_INST`).
- libdragon, `preview` branch, in `~/n64/libdragon`:
  `make -j install tools-install` with `N64_INST=~/n64/toolchain`.
- tiny3d in `~/n64/tiny3d`: `make -j` (the build links `build/libt3d.a`).
- Invader in `~/n64/invader` (only its tag definitions, read by the
  converter: `src/tag/hek/definition/*.json`).
- ares (`flatpak install --user flathub dev.ares.ares`) to run the ROM.
- The Xbox game's maps in `build/xbox/disc/maps` (from your own disc).
- Python 3 with Pillow.

## Build

```sh
python3 tools/n64_build.py              # writes build/n64/build.ninja
ninja -f build/n64/build.ninja          # build/n64/halo.z64
```

The build converts the maps listed in `port.json` ("maps"), compiles the
game and the platform layer, links, and packs the ROM with the converted
data on its file system. `port/n64/devtools/package.sh` builds the two ROMs
for playing (`~/n64/roms/halo-n64.z64`) and watching scripted players
(`halo-n64-bots.z64`). The ROMs contain data converted from the retail
game: they are for your own use.

Run it:

```sh
flatpak run dev.ares.ares --system "Nintendo 64" --setting Nintendo64/ExpansionPak=true ~/n64/roms/halo-n64.z64
```

Controller ports 1 and 2 are the two players (stick: look; C buttons: move;
Z: fire; R: grenade; A: jump; B: reload/action; L: melee; D-pad up: switch
weapon; down: crouch; left: zoom; right: switch grenades).

## How it is put together

- `tools/n64_build.py`: the ninja file. Game sources are the decomp
  project's own, less what `port.json` leaves out (AI, networking, scripts,
  sound, the Xbox renderer and menus). `tools/n64_stubs.py` writes stand-ins
  for what the rest still calls (`port/n64/game/n64_stubs_generated.c`,
  regenerate with `devtools/restub.sh`); `tools/n64_link_check.py` fails a
  link where a call to a missing inline body would go to address 0.
- `port/n64/include/halo_n64_prefix.h`: the MSVC/Xbox environment for gcc
  (big-endian MIPS, 32-bit pointers and longs, 16-bit wchar_t).
- `tools/n64/`: the map converter. `convert.py` repacks the tags into one
  big-endian image with relocations (layouts from Invader's definitions),
  streams animation frames to `maps/<map>.anim`; `level.py` converts the
  BSP's render geometry into tiny3d batches lit from each material's
  lighting summary; `models.py` converts the objects' models (and the
  first-person ones) into skinned batches; `hud.py` exports the HUD's
  bitmaps as half-size sprites (`hud/<map>/<n>.sprite`, the image's
  `pixels_offset` = n + 1); `bitmaps.py` decodes the Xbox textures;
  `ui.py` takes the menu's things from `ui.map` (the sky, with its space
  shader's stars and nebula baked into one picture, and the ring as
  models; the camera points; Halo's fonts; the logo and the maps' and game
  types' pictures); `prune.py` drops shared files no map uses any more.
- `port/n64/src/`: the platform layer. `xdk_*.c` implement the Xbox SDK
  calls (files, time, XInput over the N64 pads) with the game's ABI;
  `n64_*.c` are libdragon code: `n64_render.c` (the renderer),
  `n64_anim.c` (animation streaming), `n64_exceptions.c` (x86 behaviour
  the game relies on: unaligned access, NaN arithmetic, float-to-integer
  overflow), `n64_hud.c` (the HUD's quads, meters, text and blips, each
  view's recorded and redrawn by the game every other frame),
  `n64_profile.c` (a sampling profiler), `n64_menu.c` (the menu),
  `n64_font.c` (Halo's fonts). `n64_main.c` runs the game on a
  256 KB stack of its own: it needs ~97 KB, libdragon keeps 64 KB.
- `port/n64/game/`: game-side glue: `n64_game.c` (the menu's choices made
  into a game: variant, score, players, map; map loading, memory; main.c's
  `main_menu_load` runs the menu under `HALO_N64`, and the postgame goes
  back to it), `n64_render_game.c` (the renderer as the game sees it),
  `n64_hud_game.c` (the rasterizer calls Halo's own HUD code makes).

## Debugging

Cartridge flags (files on the ROM's file system, `port.json` "flags"):
`autotest` (scripted controllers), `debug_chase` (view 1 follows its
player), `debug_overview`, `debug_untextured`, `debug_no_level`,
`debug_no_objects`, `debug_no_first_person`, `debug_no_hud` (a plain
crosshair instead), `debug_map=<map>` (straight into a two-player Slayer
game there, as `autotest` does, without the menu), `debug_menu=<script>`
(drives the menu: a press every 20 frames, `u d l r a b z s` on port 1,
upper case on port 2, `.` none), `profile` (samples at the vertical blank from 20 to
80 s; `tools/n64_profile.py <log> build/n64/halo.elf`), `profile_zone` (with
`profile`: only while the HUD draws, with return addresses).
`devtools/n64_timing.c` times chosen game functions through `--wrap`.

`devtools/ares-run.sh` and `ares-shots.sh` run a ROM in ares on a hidden
Xvfb display (`:97`) with the debug log captured; `ares-gdb.sh` + `rsp.py`
read the CPU's state through ares' GDB server without gdb.

## State (October 2026)

8 October: the menu and all 13 multiplayer maps (Battle Creek, Sidewinder,
Damnation, Rat Race, Prisoner, Hang 'em High, Chill Out, Derelict, Boarding
Action, Blood Gulch, Wizard, Chiron TL-34, Longest) in Slayer, Team Slayer,
CTF, King of the Hill, Team King, Oddball, Team Oddball, Race and Team
Race; 10-25 fps in ares depending on the map and the view. Chunky mode
draws each view's 3D at half width and doubles its pixels in place before
the HUD (the RDP's fill halves; ares hardly charges for fill, so there it
looks chunkier more than it runs faster).

Wizard, 2 players: level, players, weapons, first-person view, Halo's own
HUD (shields, health, ammo, motion sensor, crosshairs, messages). 13-20 fps
in ares; a game tick costs ~16 ms. The HUD's own code costs ~7 ms a view
when it runs (every other frame per view). Text uses libdragon's debug font,
not Halo's.
