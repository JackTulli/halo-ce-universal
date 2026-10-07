/*
N64_RENDER_STORAGE.C

The render globals (render/render.h struct render_globals). The Xbox
renderer's half of that record -- per-surface flags and indices for its own
BSP drawing, 590 KB -- is never touched on the N64 (structure_render.c and
the BSP visibility pass that fills them are left out), so only the fields
before environment_surface_flags get storage. This unit does not include
render.h; n64_render_game.c checks the size against the real layout.
*/

#define N64_RENDER_GLOBALS_USED_SIZE 0xD290

__attribute__((aligned(16))) unsigned char render[N64_RENDER_GLOBALS_USED_SIZE + 0x10];
