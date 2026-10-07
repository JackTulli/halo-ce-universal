/*
N64_RENDER_GAME.C

The renderer as the game sees it (render/ and rasterizer/ are left out of
the N64 build): the globals other subsystems read, with the Xbox build's
defaults, the lifecycle calls, and the frame entry points main.c calls
(render_frame, render_frame_present). The drawing itself is the N64
renderer's (port/n64/src/n64_render.c).
*/

#include "cseries.h"

#include "game/game.h"
#include "game/players.h"
#include "interface/first_person_weapons.h"
#include "main/main.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_debug_options.h"
#include "render/render.h"

#include "../src/n64.h"

typedef char verify_render_globals_used_size[
	offsetof(struct render_globals, environment_surface_flags) == 0xD290 ? 1 : -1];

/* ---------- globals (rasterizer.c) */

struct rasterizer_globals_definition rasterizer_globals =
{
	FALSE,
	0,
	0,
	{ 0 },
	0,
	0,
	{ 0 },
	{ 0 },
	{ 0 },
	{ 0, 3, 0, 0 },
	FALSE,
	FALSE, /* framerate_throttle: the N64 never outruns the display */
	FALSE,
	0,
	0,
	{ 0 },
	0.0625f,
	1024.f,
	0.01171875f,
	1024.f,
	{ 0 }
};

struct rasterizer_debug_options_definition rasterizer_debug_options =
{
	FALSE, 0, 0, 0, FALSE, FALSE,
	NONE, /* debug_model_lod */
	FALSE, FALSE,
	TRUE, TRUE, TRUE, TRUE, /* models, model_transparents, draw_first_person_weapon_first, stencil_mask */
	2,
	TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE,
	TRUE, TRUE, TRUE,
	TRUE, /* dynamic_screen_geometry */
	TRUE, TRUE, TRUE, FALSE, TRUE, TRUE, TRUE,
	{ 0 },
	1.0f,
	{ 0 },
	0,
	TRUE, TRUE,
	{ 0 },
	0.0f,
	TRUE, TRUE, TRUE, FALSE, FALSE, TRUE, TRUE, TRUE, TRUE, FALSE, TRUE,
	TRUE, TRUE, FALSE, FALSE, FALSE, FALSE,
	{ 0 },
	0.4f,
	8,
	1.0f / 256.0f, /* zoffset */
	FALSE, FALSE, 0, TRUE, TRUE, TRUE, FALSE, FALSE,
	{ 0 },
	0.0f,
	1.0f,
	{ 0 },
	FALSE, FALSE,
	{ 0 },
};

real_argb_color *global_rasterizer_model_ambient_reflection_tint;

static void (*vertical_blank_callback)(unsigned long context);

/* ---------- lifecycle */

boolean rasterizer_initialize(
	void)
{
	/* the logical screen the game lays its views and HUD out on; the N64
	renderer scales it to its 320x240 frame */
	rasterizer_globals.reserved04.screen_bounds.x0 = 0;
	rasterizer_globals.reserved04.screen_bounds.y0 = 0;
	rasterizer_globals.reserved04.screen_bounds.x1 = 640;
	rasterizer_globals.reserved04.screen_bounds.y1 = 480;
	rasterizer_globals.reserved04.frame_bounds.x0 = 16;
	rasterizer_globals.reserved04.frame_bounds.y0 = 12;
	rasterizer_globals.reserved04.frame_bounds.x1 = 624;
	rasterizer_globals.reserved04.frame_bounds.y1 = 468;
	rasterizer_globals.initialized = TRUE;
	return TRUE;
}

void rasterizer_dispose(void) { return; }
void rasterizer_preinitialize__fill_you_up_with_the_devils_cock(void) { return; }

void rasterizer_set_vblank_callback(
	void (*callback)(unsigned long context))
{
	vertical_blank_callback = callback;
	return;
}

/* ---------- frames */

void render_frame(
	struct render_window *windows,
	short window_count,
	point2d const *screenshot_page_index,
	point2d const *screenshot_index,
	struct bitmap_data *screenshot_bitmap,
	real time_delta_since_tick_sec)
{
	(void)windows;
	(void)window_count;
	(void)screenshot_page_index;
	(void)screenshot_index;
	(void)screenshot_bitmap;
	render.frame_index++;
	render.time_delta_since_tick_sec = time_delta_since_tick_sec;
	return;
}

void render_frame_pregame(
	struct render_window const *window,
	struct bitmap_data *bitmap)
{
	(void)window;
	(void)bitmap;
	render.frame_index++;
	return;
}

void render_frame_present(
	const point2d *screenshot_index,
	struct bitmap_data *bitmap)
{
	(void)screenshot_index;
	(void)bitmap;
	if (vertical_blank_callback)
		vertical_blank_callback(0);
	return;
}
