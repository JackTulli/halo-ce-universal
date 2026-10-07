---
name: halo-n64-port
description: "Halo CE multiplayer splitscreen port to the Nintendo 64 (started 2026-10-07) -- decisions, tools, state, where things live"
metadata:
  node_type: memory
  type: project
  originSessionId: 55f8f983-b5a1-4f35-90a1-ac53b68ce2ea
  modified: 2026-10-07T21:19:16.129Z
---

On 2026-10-07 the user set a "very difficult challenge": port Halo CE to the N64 -- multiplayer splitscreen only (no campaign, no LAN), Expansion Pak allowed. Source = the decomp in ~/halo-ce-universal (see [[halo-ce-universal-study]]), branch `n64-port`, pushed to the user's PRIVATE github.com/JackTulli/halo-ce-universal (asked 2026-10-07 "upload all this to a private github repo"). port/n64/README.md describes build/design; port/n64/NOTES.md the findings.

Decisions (user, 2026-10-07): I MAY test builds myself in ares on a hidden Xvfb (:97, never :0 -- [[never-touch-the-live-display]]); target = emulator only (ares); tools fetched per-user into ~/n64 (toolchain from libdragon's .deb unpacked, libdragon preview, tiny3d, invader defs, ares user Flatpak). First milestone scope: no sound, only Wizard, boot straight into 2-player split screen, no lightmaps.

Tools: ~/n64/tools (copied into port/n64/devtools/): build.sh, restub.sh, package.sh (ROMs -> ~/n64/roms/halo-n64.z64 + halo-n64-bots.z64), ares-run.sh/ares-shots.sh (hidden display, debugf log), ares-gdb.sh+rsp.py (GDB protocol without gdb; the toolchain gdb needs libpython3.10), ab.sh (A/B a flag). User runs: `flatpak run dev.ares.ares --system "Nintendo 64" --setting Nintendo64/ExpansionPak=true ~/n64/roms/halo-n64.z64`.

State 2026-10-07 17:15: Wizard 2-player Slayer runs; level (vertex-lit from BSP material lighting, CI4 textures doubled), players/objects (skinned low LOD), first-person weapon+hands (game's own first_person_weapons.c posing), crosshair, FPS counter, red/blue players. ~9 fps in ares; tick ~25 ms (HALO_RELEASE-style asserts compiled out, -O2). A/B: first-person models cost ~30 ms of ~50 ms draw (no FP: 13-14 fps); level ~7 ms. RAM: tag image 2.5 MB (animation frames 1.4 MB streamed from rom:/maps/<map>.anim via 192 KB cache), ~2.8 MB heap free.

Gotchas learned: Halo winds front faces clockwise (reverse for tiny3d); CI textures + mipmaps must fit 2 KB TMEM; libdragon's sprite mipmaps force a 2-cycle combiner conflict; VR4300 traps on NaN/denormal operands (emulated in n64_exceptions.c) and unaligned access; HALO_LINUX hook stubs must return TRUE for network_damage_deals / network_objects_may_delete and NONE for network_objects_new_object_index; weak undefined inline refs silently go to address 0 (tools/n64_link_check.py); stale debug-flag files in build/n64/filesystem leaked into a user ROM (n64_build.py now removes unrequested flags); a 2 kHz timer interrupt corrupted s0 in ares -- the profiler samples on the VI interrupt.

State 2026-10-07 18:10 (commit "N64 port: Halo's own HUD"): the game's own HUD code draws through port/n64/game/n64_hud_game.c -> port/n64/src/n64_hud.c (sprites from tools/n64/hud.py, half size; meters rebuilt on the CPU per Halo's decoded meter shader; premultiplied HUD bitmaps stored straight because the RDP's additive blend wraps; each view's HUD recorded in an rspq block and re-run by the game every other frame). 13-20 fps in ares. FOUND: Halo needs ~97 KB stack, libdragon reserves 64 KB -> the game now runs on its own 256 KB stack (n64_main.c n64_call_on_stack); heap ~370-395 KB free. tag_loaded memoized (Slayer looked up strings every frame). Text still uses libdragon's debug font. ROMs packaged 18:10.

**Why:** the user wants it as autonomous as possible and plays it themselves in ares.
**How to apply:** never push ROMs or converted map data (retail-derived); keep docs/claude-memory/halo-n64-port.md in sync with this file. Next: Halo's own font for HUD text, user check of the animation freezes (cache fix + stack fix may have cured them), faster game ticks (bipeds ~3 ms each, node matrices ~5.6 ms).
