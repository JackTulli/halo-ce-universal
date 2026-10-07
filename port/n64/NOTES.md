# N64 port: working notes

Findings that steer the port (from reading the game's code, October 2026).
`port/n64/README.md` will describe the finished design; this file is the
running notebook. Paths are relative to `source/`.

## Starting a split-screen game without the menus

- Xbox split screen is a real network game (server + client over loopback).
  The N64 does not need that: `_game_connection_local` supports up to 4
  local players, and the game engine runs whenever
  `game_variant_global.game_engine_index != 0`.
- Before `main_setup_connection` (main.c `main_loop` prologue, after
  `game_initialize`/`console_startup`): `game_set_game_variant_from_name`
  ("slayer", "team_slayer", "ctf", ...; game.c:1087), `player_spawn_count = N`,
  `main_set_map_name("levels\\test\\wizard\\wizard")` (clears
  want_to_be_at_main_menu), then `main_disallow_persistent_storage()`
  (main_set_map_name turns it on). `main_menu_scenario_loaded` must be FALSE.
- `create_local_players` (main.c:752): local player index = controller
  index, players in ascending controller order; names empty (NULL network
  player). Team games: round robin; FFA under HALO_LINUX: team = datum.
- Engine table: 1 ctf, 2 slayer, 3 oddball, 4 king, 5 race.
- Local time cap: dt capped at 1/15 s (main.c:2195), at most 7 ticks; the
  game slows down below 15 fps.
- Postgame on a local connection jumps to state 3; the loop must notice
  `game_engine_force_single_screen()` and `main_reset_map()`.
- TRAP: `scenario_switch_structure_bsp` (every map load) does
  `main_stop_time`/`main_start_time`; the end of `main_loop`'s frame resets
  `halt_time_scale`. Keep the game's own `main_loop` (main.c), hook it with
  `#ifdef HALO_N64`.
- Nothing draws before game time 3 when an engine runs (main.c:3297).

## What the game needs from the (excluded) renderer

- No simulation reads render results. Required anyway:
  - `render.frame_index` incremented per frame; particles die when
    `frame_index - last_rendered_frame_index > 15` (the particle renderer
    must refresh it).
  - `render.camera`/`render.frustum` per window (`render_camera_build_frustum`,
    keep render/render_cameras.c: pure maths), `render.local_player_index`.
  - `first_person_weapon_render_update()` per window (builds the FP node
    matrices effects attach to).
  - `interface_draw_screen()` per window: motion sensor, HUD sounds,
    pickup prompts, message timers, target-name hold all advance inside it.
  - `rasterizer_globals`: screen_bounds 0,0,640,480 / frame_bounds
    48,36,592,444 (compute_window_bounds builds the views from them),
    near/far clip 0.0625/1024, the vblank callback
    (`rasterizer_set_vblank_callback`) incrementing
    `frame_and_vertical_blank_index` (main_rasterizer_throttle spins on it).
  - `rasterizer_debug_options` defaults: debug_model_lod (0x08) = NONE,
    draw_first_person_weapon_first (0x0E) = TRUE, dynamic_screen_geometry
    (0x22) = TRUE, zoffset (0x58) = 1/256.
  - `_texture_cache_bitmap_get_hardware_format` returning NULL skips a HUD
    element.
- Keep: render/render_cameras.c, rasterizer/rasterizer_geometry.c (lightmap
  sampling for unit illumination), rasterizer/rasterizer_swizzle.c,
  shaders/*.c, text/draw_string.c.
- HUD primitive: `rasterizer_psuedo_dynamic_screen_quad_draw(params, v[4])`;
  text: `draw_string`/`draw_unicode_string` with a glyph callback.
- Node matrices: computed on the tick (objects.c `object_compute_node_matrices`),
  read with `object_get_node_matrices`. Skinning:
  `node_matrices[i] * node->runtime_default_inverse_matrix` (models.c:818).
  LOD in `render_model` (models.c:838): detail_cutoff_pixels.

## Map data (tools/n64)

- Xbox MP map tag data ~7 MB; 2.9 MB of it is Xbox model geometry.
  Converted Chill Out: 4.13 MB (antr 1.74 MB, effe 0.57 MB, sbsp 0.34 MB,
  coll 0.21 MB). All 345 animations uncompressed (layout per node:
  rotation 4 x int16, translation 3 floats, scale float; animated ones per
  frame, the rest in default data).
- Bitmap pixels and sound samples are outside the tag data (in the file).
- Layouts from Invader's definitions (~/n64/invader, GPL-3, read at run
  time). The sbsp tag instance must have no address and the BSP header
  signature 'sbsp' (cache_files.c asserts).
