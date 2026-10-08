/*
N64_RENDER_GAME.C

The renderer as the game sees it (render/ and rasterizer/ are left out of
the N64 build): the globals other subsystems read, with the Xbox build's
defaults, the lifecycle calls, and the frame entry points main.c calls
(render_frame, render_frame_present). The drawing itself is the N64
renderer's (port/n64/src/n64_render.c).
*/

#include "cseries.h"

#include <math.h>
#include <stdio.h>

#include "game/game.h"
#include "game/players.h"
#include "game/local_players.h"
#include "interface/first_person_weapons.h"
#include "interface/interface.h"
#include "main/main.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_debug_options.h"
#include "render/render.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "models/model_definitions.h"
#include "items/weapon_definitions.h"
#include "tag_files/tag_groups.h"

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

/* debugging (port.json flag "debug_weapon=<weapon tag name>"): players
spawn with that weapon first (game_engine.c's starting equipment) */
static long debug_weapon = NONE;

static void n64_debug_starting_weapon(
	void)
{
	char name[128];

	debug_weapon = NONE;
	if (n64_debug_text("debug_weapon", name, sizeof(name)))
		debug_weapon = tag_loaded(WEAPON_DEFINITION_TAG, name);
	return;
}

long n64_debug_spawn_weapon(
	long definition_index,
	boolean first_weapon)
{
	return first_weapon && debug_weapon != NONE ? debug_weapon : definition_index;
}

void render_initialize_for_new_map(
	void)
{
	n64_render_load_level(n64_loaded_map_name());
	n64_hud_set_map(n64_loaded_map_name());
	n64_debug_starting_weapon();
	return;
}

/* ---------- objects */

/* every visible object but the viewer's own body (the view is first person) */
static void render_objects_for_view(
	short local_player_index,
	boolean third_person)
{
	long viewer_unit = NONE;
	long player_index = local_player_get_player_index(local_player_index);
	struct object_iterator iterator;
	struct object_datum *object;

	if (player_index != NONE && !third_person)
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
		n64.tint[0] = n64.tint[1] = n64.tint[2] = n64.tint[3] = 0xFF;
		if (TEST_FLAG(_object_mask_unit, object->object.type) || TEST_FLAG(_object_mask_unit, object_get(object_get_ultimate_parent(iterator.index))->object.type))
		{
			/* a player (or what a player holds): the armour colour, where the
			model's textures take it */
			struct object_datum *owner = object_get(object_get_ultimate_parent(iterator.index));
			real_rgb_color const *color = &owner->object.base_change_colors[_object_change_color_a];

			n64.tint[0] = (unsigned char)PIN(color->red * 255.f, 0.f, 255.f);
			n64.tint[1] = (unsigned char)PIN(color->green * 255.f, 0.f, 255.f);
			n64.tint[2] = (unsigned char)PIN(color->blue * 255.f, 0.f, 255.f);
		}
		n64_render_object(&n64);
	}
}

/* ---------- the first-person weapon (interface/first_person_weapons.c poses
it; its draw hands each model here) */

static boolean first_person_begun;

void n64_first_person_model_draw(
	long model_index,
	struct real_matrix4x3 const *node_matrices,
	real_rgb_color const *tint)
{
	static unsigned char const no_permutations[MAXIMUM_REGIONS_PER_OBJECT] = { 0 };
	struct n64_object n64;

	if (model_index == NONE)
		return;
	if (!first_person_begun)
	{
		n64_render_first_person_begin();
		first_person_begun = TRUE;
	}
	n64.model_tag_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(model_index);
	n64.node_count = (short)model_definition_get(model_index)->nodes.count;
	n64.node_matrices = (const float *)node_matrices;
	n64.region_permutations = no_permutations;
	n64.region_count = 0;
	n64.center[0] = render.camera.position.x;
	n64.center[1] = render.camera.position.y;
	n64.center[2] = render.camera.position.z;
	n64.radius = 4.f;
	/* the player's colour, where the model's textures take it */
	n64.tint[0] = tint ? (unsigned char)PIN(tint->red * 255.f, 0.f, 255.f) : 0xFF;
	n64.tint[1] = tint ? (unsigned char)PIN(tint->green * 255.f, 0.f, 255.f) : 0xFF;
	n64.tint[2] = tint ? (unsigned char)PIN(tint->blue * 255.f, 0.f, 255.f) : 0xFF;
	n64.tint[3] = 0xFF;
	n64_render_object(&n64);
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
		if (window_index == 0 && n64_debug_flag("debug_chase"))
		{
			/* debugging: the first view follows its player from behind */
			long player_index = local_player_get_player_index(window->local_player_index);

			if (player_index != NONE && player_get(player_index)->unit_index != NONE)
			{
				struct object_datum *unit = object_get(player_get(player_index)->unit_index);
				real_vector3d forward = unit->object.forward;

				view.position[0] = unit->object.position.x - forward.i * 1.6f;
				view.position[1] = unit->object.position.y - forward.j * 1.6f;
				view.position[2] = unit->object.position.z + 0.9f;
				view.forward[0] = forward.i * 0.94f;
				view.forward[1] = forward.j * 0.94f;
				view.forward[2] = -0.34f;
				view.up[0] = 0.f;
				view.up[1] = 0.f;
				view.up[2] = 1.f;
			}
		}
		if (window_index == 0)
		{
			/* debugging (port.json flag "debug_spin=x y z [yaw]"): the first
			view stands there, turning once every 16 seconds (or facing yaw
			degrees) */
			static int spin = -1;
			static float spin_at[4];
			char text[64];

			if (spin < 0)
			{
				spin = n64_debug_text("debug_spin", text, sizeof(text)) ?
					sscanf(text, "%f %f %f %f", &spin_at[0], &spin_at[1], &spin_at[2], &spin_at[3]) : 0;
				if (spin < 3)
					spin = 0;
			}
			if (spin)
			{
				float angle = spin == 4 ? spin_at[3] * (6.2831853f / 360.f) :
					(float)(game_time_get() % 480) * (6.2831853f / 480.f);

				view.position[0] = spin_at[0];
				view.position[1] = spin_at[1];
				view.position[2] = spin_at[2];
				view.forward[0] = cosf(angle);
				view.forward[1] = sinf(angle);
				view.forward[2] = -0.15f;
				view.up[0] = 0.f;
				view.up[1] = 0.f;
				view.up[2] = 1.f;
			}
		}
		render.local_player_index = window->local_player_index;
		render.window_index = window_index;
		render.camera = *camera;
		n64_render_view(&view);
		render_objects_for_view(window->local_player_index, window_index == 0 && n64_debug_flag("debug_chase"));
		if (!(window_index == 0 && n64_debug_flag("debug_chase")) && !n64_debug_flag("debug_no_first_person"))
		{
			first_person_begun = FALSE;
			first_person_weapon_render_update();
			first_person_weapon_draw();
		}
		if (!n64_debug_flag("debug_no_hud"))
		{
			/* the game's own HUD (port/n64/game/n64_hud_game.c) */
			if (n64_hud_begin(window_index, view.x0, view.y0, view.x1, view.y1))
				interface_draw_screen();
			n64_hud_end();
		}
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
