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
#include "game/local_players.h"
#include "interface/first_person_weapons.h"
#include "main/main.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_debug_options.h"
#include "render/render.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "models/model_definitions.h"

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
	n64_render_initialize();
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

/* ---------- maps */

char const *n64_loaded_map_name(void);

void render_initialize(void) { return; }
void render_dispose(void) { return; }
void render_dispose_from_old_map(void) { return; }

void render_initialize_for_new_map(
	void)
{
	n64_render_load_level(n64_loaded_map_name());
	return;
}

/* ---------- objects */

/* every visible object but the viewer's own body (the view is first person) */
static void render_objects_for_view(
	short local_player_index)
{
	long viewer_unit = NONE;
	long player_index = local_player_get_player_index(local_player_index);
	struct object_iterator iterator;
	struct object_datum *object;

	if (player_index != NONE)
		viewer_unit = player_get(player_index)->unit_index;
	object_iterator_new(&iterator, _object_mask_all, FLAG(_object_header_visible_bit));
	while ((object = object_iterator_next(&iterator)) != NULL)
	{
		struct object_definition *definition;
		struct n64_object n64;

		if (TEST_FLAG(object->object.flags, _object_invisible_bit))
			continue;
		if (viewer_unit != NONE && object_get_ultimate_parent(iterator.index) == viewer_unit)
			continue;
		definition = object_definition_get(object->definition_index);
		if (definition->object.model.index == NONE)
			continue;
		n64.model_tag_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(definition->object.model.index);
		n64.node_count = (short)model_definition_get(definition->object.model.index)->nodes.count;
		n64.node_matrices = (const float *)object_get_node_matrices(iterator.index);
		n64.region_permutations = object->object.region_permutations;
		n64.region_count = MAXIMUM_REGIONS_PER_OBJECT;
		n64.center[0] = object->object.bounding_sphere_center.x;
		n64.center[1] = object->object.bounding_sphere_center.y;
		n64.center[2] = object->object.bounding_sphere_center.z;
		n64.radius = object->object.bounding_sphere_radius;
		n64_render_object(&n64);
	}
}

/* ---------- frames */

static boolean frame_begun;

static void render_frame_begin(
	void)
{
	if (!frame_begun)
	{
		n64_render_frame_begin();
		frame_begun = TRUE;
	}
}

void render_frame(
	struct render_window *windows,
	short window_count,
	point2d const *screenshot_page_index,
	point2d const *screenshot_index,
	struct bitmap_data *screenshot_bitmap,
	real time_delta_since_tick_sec)
{
	short window_index;

	(void)screenshot_page_index;
	(void)screenshot_index;
	(void)screenshot_bitmap;
	render.frame_index++;
	render.time_delta_since_tick_sec = time_delta_since_tick_sec;
	render_frame_begin();
	for (window_index = 0; window_index < window_count; window_index++)
	{
		struct render_window *window = &windows[window_index];
		struct render_camera const *camera = &window->render_camera;
		struct n64_view view;

		if (window->console_window)
			continue;
		view.position[0] = camera->position.x;
		view.position[1] = camera->position.y;
		view.position[2] = camera->position.z;
		view.forward[0] = camera->forward.i;
		view.forward[1] = camera->forward.j;
		view.forward[2] = camera->forward.k;
		view.up[0] = camera->up.i;
		view.up[1] = camera->up.j;
		view.up[2] = camera->up.k;
		view.vertical_field_of_view = camera->vertical_field_of_view;
		view.z_near = camera->z_near;
		view.z_far = 200.f;
		/* the game lays the views out on a 640x480 screen */
		view.x0 = camera->viewport_bounds.x0 / 2;
		view.y0 = camera->viewport_bounds.y0 / 2;
		view.x1 = camera->viewport_bounds.x1 / 2;
		view.y1 = camera->viewport_bounds.y1 / 2;
		view.local_player_index = window->local_player_index;
		render.local_player_index = window->local_player_index;
		render.window_index = window_index;
		render.camera = *camera;
		n64_render_view(&view);
		render_objects_for_view(window->local_player_index);
	}
	return;
}

void render_frame_pregame(
	struct render_window const *window,
	struct bitmap_data *bitmap)
{
	(void)window;
	(void)bitmap;
	render.frame_index++;
	render_frame_begin();
	return;
}

void render_frame_present(
	const point2d *screenshot_index,
	struct bitmap_data *bitmap)
{
	(void)screenshot_index;
	(void)bitmap;
	render_frame_begin();
	n64_render_frame_end();
	frame_begun = FALSE;
	if (vertical_blank_callback)
		vertical_blank_callback(0);
	return;
}
