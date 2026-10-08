# Halo CE on the Nintendo 64 (multiplayer split screen)

A port of the game's multiplayer to the N64 with the Expansion Pak:
Halo's own simulation (objects, physics, collision, weapons, game types,
players, first-person weapons) compiled for the VR4300, a new renderer on
tiny3d, and maps converted from the Xbox game's cache files, with Halo's
music in the menu and its sounds in the game (not the environment's). No
campaign, no networking. It starts at a menu of its own over the scene
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
- libdragon, `preview` branch, in `~/n64/libdragon`, with
  `devtools/libdragon-rspq-buffer.patch` applied (`git apply`: the RSP
  command queue's buffer size becomes a variable the game sets; stock
  libdragon builds too, only slower):
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
upper case on port 2, `.` none), `debug_players=<1-4>` (with `debug_map`),
`debug_gpu_time` (each render phase waits for the RSP/RDP: their times),
`debug_guard=<n>`, `debug_cull_front`, `debug_level_parts=<1|2>` (level
measurements: vertices alone, materials alone), `profile` (samples at the vertical blank from 20 to
80 s; `tools/n64_profile.py <log> build/n64/halo.elf`), `profile_zone` (with
`profile`: only while the HUD draws, with return addresses), `debug_sound`
(each sound started, the first 300, and every five seconds the voices,
the sounds playing and the mixer's CPU time), `debug_no_sound` (the
game's sounds off, to measure them), `profile_sound` (with `profile
profile_zone`: only while the mixer is fed).
`devtools/n64_timing.c` times chosen game functions through `--wrap`.

`devtools/ares-run.sh` and `ares-shots.sh` run a ROM in ares on a hidden
Xvfb display (`:97`) with the debug log captured; `ares-gdb.sh` + `rsp.py`
read the CPU's state through ares' GDB server without gdb.

## State (October 2026)

8 October, a deep optimisation round (`devtools/bench-set.sh`: eight
scenes, scripted players; ares, fps before -> after):

| scene | 1 player | 2 players | 4 players |
|---|---|---|---|
| Wizard | 29.9 -> 30.0 | 26.2 -> 29.2 | 10.7 -> 18.3 |
| Blood Gulch | 29.8 -> 29.9 | 16.6 -> 26.9 | 8.6 -> 14.0 |
| Firefight, Blood Gulch | 23.4 -> 25.2 | | 5.9 -> 8.4 |

What did it, largest first: the debug flag cache held 16 names and the
rest opened a cartridge file on every call, one of them per object per
view; the RSP command queue's buffers at 2048 words (libdragon patched:
the CPU waited for the RSP mid-frame, now it runs the next ticks while
the RSP draws); link-time optimisation of the game; single precision
square root and trigonometry, fmod(x, 1) as an exact float fraction,
matrix4x3_multiply written out; parked vehicles keep their node matrices,
far Covenant pose every other tick, swaying plants too; the N64 makes no
particles, particle systems, contrails or widgets (nothing draws them;
their point physics cast ~1,500 rays a second in Firefight); the 3-4
player models' first-person weapons from one 64-texel atlas; the unit and
biped vector diagnostics off; HUD quads without rdpq_tex_blit where their
rectangle fits TMEM; tag block element access inline; the drawable
objects gathered once a frame for all views. At 4 players the game's
ticks are the limit now (a biped's movement, biped_update_physics, is
~1 ms of its ~3 ms a tick); Firefight with four players still runs below
real time. -Os and -O3 do not fit: the bigger code leaves too little
heap, -Os crashed.

8 October, the game's sounds: weapons (fire, reload, ready, charge,
overheat), impacts and explosions, footsteps, vehicles (their engines'
loops follow the vehicle's speed), the shields' and health's HUD sounds,
pickups, the announcer, the players' and the Covenant's pain and death
cries, and the Covenant's lines in Firefight (spotting a player, throwing
a grenade, taunts, melee, a player killed, a grunt panicking when its
elite falls). Halo's sound code stays left out: the calls the game makes
into it are `port/n64/game/n64_sound_game.c`'s, after Halo's sound manager
(random permutation not played lately, pitch and gain from the scale,
pitch ranges, skip fractions; the nearest player hears a sound, full
within its minimum distance, then minimum / distance, nothing past its
maximum; panned to the camera's left or right; a class's limits of the
same sound at once and on one object). Sound tags are kept as records of
the N64's own (`tools/n64/sounds.py`; 27 KB for Blood Gulch), the
environment's (ambience, devices, music) left out; each choosable
permutation is decoded from Xbox ADPCM, made mono 22 kHz VADPCM without
Huffman packing (that was undone by the CPU: a quarter of it) and packed
with every map's into one bank, `snd/sounds.bank` (`tools/n64/
soundbank.py`, 12 MB, 1005 sounds; a map's `snd/<map>.ofs` says where its
sounds are), streamed from the cartridge on 12 voices
(`port/n64/src/n64_sound.c`; a new sound takes a free voice or the
cheapest of no higher priority). Firefight's Covenant bring their
dialogue from a30 (`merge.py`: the vocalizations Firefight plays, three
lines each at most; not their animations' foley). The mixer is fed between
ticks and views as well as at the frame's end (150 ms of audio queued).
ROM 57 MB. In ares sound costs ~8% of the CPU (Blood Gulch Firefight,
one player); the heap's low point there is ~90 KB (was ~118).

8 October, sound starts: the main menu plays Halo's title music
(`tools/n64/music.py`: ui.map's title1 looping sound, its chains of Xbox
ADPCM permutations decoded -- 64 samples a 36-byte block, as
`port/linux/src/dsound_sdl.c`; ffmpeg's IMA WAV decoder adds a 65th --
resampled to 22 kHz and compressed to VADPCM wav64 by audioconv64, 5 MB;
`port/n64/src/n64_sound.c`: libdragon's audio and mixer, the start once
then a random loop version after another, streamed from the cartridge;
the mixer fed at every frame's end). Halo's `profile_dump` is renamed on
the N64 (libdragon's audio brings its own).

8 October, Firefight (`port/n64/game/n64_firefight.c`): the title's
FIREFIGHT entry, 1-4 players, a map and a difficulty, against waves of
the campaign's grunts, jackals and elites (imported into every
multiplayer map from a30 by `tools/n64/merge.py`, at their lowest level
of detail, their bitmaps read from a30's file). Their AI is the N64's
own, through `unit_control()`, thinking at 15 Hz (half of them each
tick); they find their way with Halo's own pathfinding
(`source/ai/path*.c`, one search a tick at most, obstacle avoidance off:
it needs the AI's object records). Units must be kept active
(`object_set_automatic_deactivation`) and actively controlled
(`unit_set_actively_controlled`), or Halo freezes them out of sight and
resets their throttles. Shared lives, waves of five a set, results on
the menu. ares: Wizard ~28 fps with one player, Blood Gulch ~21, Wizard
four players ~9 (each Covenant biped costs ~3.4 ms of a tick, like a
player).

8 October, performance: the views see 55 world units (one player), 45
(two) or 32 (three or four) into fog of the map's own colour (its sky's
outdoor fog), nothing wholly past it (left out by distance); three or
four players get lighter levels
(`<map>-low.lvl`, no lightmap splitting) and models (`<map>-low.mdl`: the
objects at half their triangles, first-person views at 350); the level's
visible batches are drawn a material at a time and the objects' batches
sorted by mode and texture (switching between rdpq's and tiny3d's RSP code
was costly); each level material's mode and texture are recorded once (the
CPU worked them out every change, ~7 ms a frame with four views); objects
whose node matrices cannot have changed keep them (scenery too: half of
Blood Gulch's tick); with three or four views each view's HUD code runs
every fourth frame. In ares: Wizard 30 / 27 / 11 fps with 1 / 2 / 4
players, Blood Gulch 30 / 18 / 9 (were 30 / 19 / 8 and 27 / 12 / 6). Four
players also crashed in the motion sensor (a decompiled array sized for
two players), fixed.

Distant terrain flashing in and out (worse on big maps) was a race: the
viewports were tiny3d's unbuffered kind, one copy of the camera and
projection matrices overwritten by every attach, and once the CPU ran
ahead of the RSP the world's later batches and objects were drawn with the
first-person weapon's (16-unit far plane). They are buffered now (two
attaches a frame for each frame in flight). The frustum's far plane is
also made from the eye and its direction: tiny3d's, the difference of two
projection rows equal to four digits, moved about in floats. Measuring:
`debug_spin_period=<ticks>`, `debug_far`, `debug_no_pvs`,
`debug_fog_test=<1-5>`.

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
