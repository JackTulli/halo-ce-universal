/*
N64_MENU.C

The N64's own menu, split screen multiplayer only, over the scene behind
Halo's main menu (tools/n64/ui.py): the space sky and the ring, seen from
the menu's "multiplayer" camera point, slowly drifting. Halo's fonts and
pictures (the logo, the maps' and the game types').

The title offers MULTIPLAYER and SETTINGS. Multiplayer is the game's
setup: the players join with A on their controller (B leaves, Z changes
team), any of them chooses the game type, the map and the score to win;
START (or A on "START GAME") plays. Settings: chunky mode (the game drawn
half as wide, its pixels twice as wide: n64_render_set_chunky). After a
game, its results first. n64_menu_run returns the choices; the
game side (port/n64/game/n64_game.c) makes the game.
*/

#include <libdragon.h>
#include <math.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "n64.h"

/* ---------- what can be played */

static const struct
{
	const char *folder, *name;
	int picture; /* in ui/mp_map_grafix's order */
} maps[] = {
	{"beavercreek", "BATTLE CREEK", 0},
	{"sidewinder", "SIDEWINDER", 1},
	{"damnation", "DAMNATION", 2},
	{"ratrace", "RAT RACE", 3},
	{"prisoner", "PRISONER", 4},
	{"hangemhigh", "HANG 'EM HIGH", 5},
	{"chillout", "CHILL OUT", 6},
	{"carousel", "DERELICT", 7},
	{"boardingaction", "BOARDING ACTION", 8},
	{"bloodgulch", "BLOOD GULCH", 9},
	{"wizard", "WIZARD", 10},
	{"putput", "CHIRON TL-34", 11},
	{"longest", "LONGEST", 12},
};
#define MAP_COUNT ((int)(sizeof(maps) / sizeof(maps[0])))

static const struct
{
	const char *variant, *name;
	int teams;
	int picture; /* in ui/game_type_grafix's order */
	const char *unit;
	int scores[5];
	int default_score;
} game_types[] = {
	{"slayer", "SLAYER", 0, 2, "KILLS", {5, 10, 15, 25, 50}, 2},
	{"team_slayer", "TEAM SLAYER", 1, 2, "KILLS", {10, 25, 50, 75, 100}, 2},
	{"ctf", "CAPTURE THE FLAG", 1, 0, "FLAGS", {1, 3, 5, 7, 10}, 1},
	{"king", "KING OF THE HILL", 0, 1, "MINUTES", {1, 2, 3, 5, 10}, 1},
	{"team_king", "TEAM KING", 1, 1, "MINUTES", {1, 2, 3, 5, 10}, 1},
	{"oddball", "ODDBALL", 0, 3, "MINUTES", {1, 2, 3, 5, 10}, 1},
	{"team_oddball", "TEAM ODDBALL", 1, 3, "MINUTES", {1, 2, 3, 5, 10}, 1},
	{"race", "RACE", 0, 4, "LAPS", {1, 3, 5, 7, 10}, 1},
	{"team_race", "TEAM RACE", 1, 4, "LAPS", {1, 3, 5, 7, 10}, 1},
};
#define GAME_TYPE_COUNT ((int)(sizeof(game_types) / sizeof(game_types[0])))

/* the players' colours (Halo's colour indices: red, blue, green, yellow)
and how the menu shows them */
static const int player_colors[4] = {2, 3, 6, 5};
static const unsigned long player_rgb[4] = {0xC82020, 0x2050D8, 0x20A040, 0xE0C820};
static const unsigned long team_rgb[2] = {0xC82020, 0x2050D8};

const char *n64_menu_map_folder(int map) { return map >= 0 && map < MAP_COUNT ? maps[map].folder : "wizard"; }
const char *n64_menu_map_name(int map) { return map >= 0 && map < MAP_COUNT ? maps[map].name : ""; }
const char *n64_menu_game_variant(int type) { return type >= 0 && type < GAME_TYPE_COUNT ? game_types[type].variant : "slayer"; }
int n64_menu_game_teams(int type) { return type >= 0 && type < GAME_TYPE_COUNT && game_types[type].teams; }

int n64_menu_map_from_folder(const char *folder)
{
	for (int i = 0; i < MAP_COUNT; i++)
	{
		if (!strcmp(maps[i].folder, folder))
			return i;
	}
	return -1;
}

/* ---------- the menu's things */

#define TEXT_COLOR 0xFF8CC0FFu
#define TEXT_BRIGHT 0xFFFFFFFFu
#define TEXT_DIM 0xC05880B0u

static struct
{
	int loaded;
	struct n64_font *large, *small;
	sprite_t *logo;
	sprite_t *map_pictures[16], *type_pictures[8];
	/* the settings */
	int chunky;
	/* the scene */
	int sky_model, ring_model;
	float ring[13];
	float camera[10];
	float frame;
} menu;

static sprite_t *picture(sprite_t **cache, const char *kind, int number)
{
	char path[48];

	if (number < 0 || number >= 16)
		return NULL;
	if (!cache[number])
	{
		snprintf(path, sizeof(path), "rom:/ui/%s%d.sprite", kind, number);
		cache[number] = sprite_load(path);
	}
	return cache[number];
}

static void load_scene(void)
{
	FILE *file = fopen("rom:/ui/scene.bin", "rb");
	uint8_t header[8];
	uint32_t count;

	menu.sky_model = menu.ring_model = -1;
	if (!file)
		return;
	if (fread(header, 1, 8, file) == 8 && !memcmp(header, "H64S", 4))
	{
		menu.sky_model = (header[4] << 8) | header[5];
		menu.ring_model = (header[6] << 8) | header[7];
		fread(menu.ring, 4, 13, file);
		fread(&count, 4, 1, file);
		/* the "multiplayer" camera point: the menu's own place in Halo */
		for (uint32_t i = 0; i < count; i++)
		{
			char name[32];
			float point[10];

			if (fread(name, 1, 32, file) != 32 || fread(point, 4, 10, file) != 10)
				break;
			if (i == 0 || !strcmp(name, "multiplayer"))
				memcpy(menu.camera, point, sizeof(point));
		}
	}
	fclose(file);
}

static void menu_load(void)
{
	if (!menu.loaded)
	{
		menu.large = n64_font_load("large");
		menu.small = n64_font_load("small");
		menu.logo = sprite_load("rom:/ui/logo.sprite");
		load_scene();
		menu.loaded = 1;
	}
	/* the game's level and models give way to the scene's */
	n64_render_load_scene("ui");
	n64_render_set_clear_color(0x000000);
	n64_render_set_fog(0, 0.f, 0.f);
}

/* the menu's things give their memory back to the game (once the RSP and
the RDP are done with them) */
static void menu_unload(void)
{
	rspq_wait();
	n64_font_free(menu.large);
	n64_font_free(menu.small);
	menu.large = menu.small = NULL;
	if (menu.logo)
		sprite_free(menu.logo);
	menu.logo = NULL;
	for (int i = 0; i < 16; i++)
	{
		if (menu.map_pictures[i])
			sprite_free(menu.map_pictures[i]);
		menu.map_pictures[i] = NULL;
	}
	for (int i = 0; i < 8; i++)
	{
		if (menu.type_pictures[i])
			sprite_free(menu.type_pictures[i]);
		menu.type_pictures[i] = NULL;
	}
	menu.loaded = 0;
	n64_render_load_scene(NULL);
}

/* ---------- drawing */

static void draw_scene(void)
{
	struct n64_view view;
	float t = menu.frame * 0.004f;
	/* a slow drift about the camera point */
	float yaw = sinf(t) * 0.06f, pitch = sinf(t * 0.7f) * 0.03f;
	float *c = menu.camera;
	float forward[3] = {c[3], c[4], c[5]}, up[3] = {c[6], c[7], c[8]};
	float left[3] = {up[1] * forward[2] - up[2] * forward[1], up[2] * forward[0] - up[0] * forward[2],
		up[0] * forward[1] - up[1] * forward[0]};

	memset(&view, 0, sizeof(view));
	for (int k = 0; k < 3; k++)
	{
		view.position[k] = c[k];
		view.forward[k] = forward[k] + left[k] * yaw + up[k] * pitch;
		view.up[k] = up[k];
	}
	/* the camera point's field of view is horizontal */
	view.vertical_field_of_view = c[9] > 0.1f ? 2.f * atanf(tanf(c[9] * 0.5f) * 0.75f) : 0.9f;
	view.z_near = 0.02f;
	view.z_far = 200.f;
	view.x0 = 0;
	view.y0 = 0;
	view.x1 = 320;
	view.y1 = 240;
	view.local_player_index = 0;
	view.visible_clusters = NULL;
	n64_render_view(&view);
	if (menu.sky_model >= 0)
	{
		/* the sky about the eye */
		static const unsigned char permutations[16];
		float matrix[13] = {1.f, 1, 0, 0, 0, 1, 0, 0, 0, 1, c[0], c[1], c[2]};
		struct n64_object sky = {
			.model_tag_index = (short)menu.sky_model, .node_count = 1, .node_matrices = matrix,
			.region_permutations = permutations, .region_count = 0,
			.center = {c[0], c[1], c[2]}, .radius = 1000.f, .tint = {0xFF, 0xFF, 0xFF, 0xFF},
		};

		n64_render_object(&sky);
	}
	if (menu.ring_model >= 0)
	{
		static const unsigned char permutations[16];
		struct n64_object ring = {
			.model_tag_index = (short)menu.ring_model, .node_count = 1, .node_matrices = menu.ring,
			.region_permutations = permutations, .region_count = 0,
			.center = {menu.ring[10], menu.ring[11], menu.ring[12]}, .radius = 40.f, .tint = {0xFF, 0xFF, 0xFF, 0xFF},
		};

		n64_render_object(&ring);
	}
	/* the scene before the menu over it */
	n64_render_flush();
}

static void panel(int x0, int y0, int x1, int y1, unsigned long argb)
{
	rdpq_sync_pipe();
	rdpq_set_mode_standard();
	rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
	rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
	rdpq_set_prim_color(RGBA32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, argb >> 24));
	rdpq_fill_rectangle(x0, y0, x1, y1);
}

static void frame_line(int x0, int y0, int x1, int y1, unsigned long argb)
{
	panel(x0, y0, x1, y0 + 1, argb);
	panel(x0, y1 - 1, x1, y1, argb);
	panel(x0, y0, x0 + 1, y1, argb);
	panel(x1 - 1, y0, x1, y1, argb);
}

static void blit(sprite_t *sprite, int x, int y, float scale)
{
	surface_t surface;

	if (!sprite)
		return;
	surface = sprite_get_pixels(sprite);
	rdpq_sync_pipe();
	rdpq_set_mode_standard();
	rdpq_mode_alphacompare(1);
	rdpq_mode_filter(FILTER_BILINEAR);
	if (sprite_get_format(sprite) == FMT_RGBA16 || sprite_get_format(sprite) == FMT_RGBA32)
		rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
	rdpq_tex_blit(&surface, x, y, &(rdpq_blitparms_t){ .scale_x = scale, .scale_y = scale });
}

static void text_centered(struct n64_font *font, int center, int y, unsigned long color, const char *text)
{
	n64_text_draw(font, center - n64_text_width(font, text) / 2, y, color, text);
}

/* ---------- input: any controller, with the stick's repeats */

struct pad
{
	joypad_buttons_t pressed;
	int up, down, left, right; /* this frame, from the d-pad or the stick */
	int stick_held, stick_timer;
};

static struct pad pads[4];

/* testing (port.json flag "debug_menu=<script>"): a press every 20 frames,
one character each: u d l r (d-pad), a b z s (A, B, Z, START) on port 1,
upper case on port 2, '.' none */
static char script[64];
static int script_position = -1;

static void scripted_press(void)
{
	char c;
	struct pad *pad;

	if (script_position < 0)
		script_position = n64_debug_text("debug_menu", script, sizeof(script)) ? 0 : (int)sizeof(script);
	if (script_position >= (int)sizeof(script) || !script[script_position] || (int)menu.frame % 20)
		return;
	c = script[script_position++];
	pad = &pads[c >= 'A' && c <= 'Z' ? 1 : 0];
	switch (c | 0x20)
	{
	case 'u': pad->up = 1; break;
	case 'd': pad->down = 1; break;
	case 'l': pad->left = 1; break;
	case 'r': pad->right = 1; break;
	case 'a': pad->pressed.a = 1; break;
	case 'b': pad->pressed.b = 1; break;
	case 'z': pad->pressed.z = 1; break;
	case 's': pad->pressed.start = 1; break;
	}
}

static void read_pads(void)
{
	joypad_poll();
	for (int p = 0; p < 4; p++)
	{
		struct pad *pad = &pads[p];
		joypad_inputs_t inputs = joypad_get_inputs(p);
		int direction = 0;

		pad->pressed = joypad_get_buttons_pressed(p);
		pad->up = pad->pressed.d_up;
		pad->down = pad->pressed.d_down;
		pad->left = pad->pressed.d_left;
		pad->right = pad->pressed.d_right;
		if (inputs.stick_y > 50)
			direction = 1;
		else if (inputs.stick_y < -50)
			direction = 2;
		else if (inputs.stick_x < -50)
			direction = 3;
		else if (inputs.stick_x > 50)
			direction = 4;
		if (!direction)
		{
			pad->stick_held = 0;
			continue;
		}
		/* a step when pushed, then every 8 frames after 20 */
		if (direction != pad->stick_held)
		{
			pad->stick_held = direction;
			pad->stick_timer = 20;
		}
		else if (--pad->stick_timer > 0)
			continue;
		else
			pad->stick_timer = 8;
		pad->up |= direction == 1;
		pad->down |= direction == 2;
		pad->left |= direction == 3;
		pad->right |= direction == 4;
	}
	scripted_press();
}

/* ---------- the screens */

enum { SCREEN_RESULTS, SCREEN_TITLE, SCREEN_SETUP, SCREEN_SETTINGS };
enum { ROW_GAME_TYPE, ROW_MAP, ROW_SCORE, ROW_START, ROW_COUNT };
enum { TITLE_MULTIPLAYER, TITLE_SETTINGS, TITLE_COUNT };
enum { SETTING_CHUNKY, SETTING_BACK, SETTING_COUNT };

/* a choice in a list: brighter, framed and pulsing when chosen */
static void list_item(int center, int y, int width, int selected, const char *text)
{
	int pulse = (int)(200 + 55 * sinf(menu.frame * 0.1f));

	if (selected)
	{
		panel(center - width / 2, y - 16, center + width / 2, y + 6, 0xA0306CB8);
		frame_line(center - width / 2, y - 16, center + width / 2, y + 6, 0xFF8CC0FF);
	}
	text_centered(menu.large, center, y, selected ? ((unsigned long)pulse << 24) | (TEXT_BRIGHT & 0xFFFFFF) : TEXT_COLOR,
		text);
}

static void draw_title(int item)
{
	static const char *items[TITLE_COUNT] = {"MULTIPLAYER", "SETTINGS"};

	blit(menu.logo, 32, 30, 1.f);
	for (int i = 0; i < TITLE_COUNT; i++)
		list_item(160, 168 + i * 26, 150, i == item, items[i]);
	text_centered(menu.small, 160, 228, TEXT_DIM, "SPLIT SCREEN  -  UP TO 4 PLAYERS");
}

static void draw_settings(int item)
{
	char value[32];

	panel(0, 0, 320, 240, 0x40000818);
	n64_text_draw(menu.large, 14, 26, TEXT_BRIGHT, "SETTINGS");
	panel(12, 32, 308, 33, 0xC08CC0FF);
	snprintf(value, sizeof(value), "CHUNKY MODE: %s", menu.chunky ? "ON" : "OFF");
	list_item(160, 76, 220, item == SETTING_CHUNKY, value);
	text_centered(menu.small, 160, 100, TEXT_COLOR, "THE 3D DRAWN HALF AS WIDE, EACH");
	text_centered(menu.small, 160, 114, TEXT_COLOR, "PIXEL DOUBLED (THE HUD STAYS SHARP)");
	list_item(160, 160, 120, item == SETTING_BACK, "BACK");
	n64_text_draw(menu.small, 14, 232, TEXT_DIM, "A/LEFT/RIGHT CHANGE   B BACK");
}

static void draw_setup(const struct n64_menu_choice *c, int row, int score_index)
{
	static const char *labels[ROW_COUNT] = {"GAME TYPE", "MAP", "SCORE TO WIN", "START GAME"};
	char value[48];
	int teams = n64_menu_game_teams(c->game_type);

	panel(0, 0, 320, 240, 0x40000818);
	n64_text_draw(menu.large, 14, 26, TEXT_BRIGHT, "MULTIPLAYER");
	n64_text_draw(menu.small, 222, 26, TEXT_DIM, "SPLIT SCREEN");
	panel(12, 32, 308, 33, 0xC08CC0FF);
	/* the rows: a label on the left, its value on the right */
	for (int r = 0; r < ROW_COUNT; r++)
	{
		int y = 40 + r * 26;
		int selected = r == row;

		panel(12, y, 206, y + 22, selected ? 0xA0306CB8 : 0x80102040);
		if (selected)
			frame_line(12, y, 206, y + 22, 0xFF8CC0FF);
		if (r == ROW_START)
		{
			text_centered(menu.large, 109, y + 17, c->player_count ? (selected ? TEXT_BRIGHT : TEXT_COLOR) : TEXT_DIM,
				labels[r]);
			continue;
		}
		n64_text_draw(menu.small, 18, y + 15, selected ? TEXT_COLOR : TEXT_DIM, labels[r]);
		if (r == ROW_GAME_TYPE)
			snprintf(value, sizeof(value), "%s", game_types[c->game_type].name);
		else if (r == ROW_MAP)
			snprintf(value, sizeof(value), "%s", maps[c->map].name);
		else
			snprintf(value, sizeof(value), "%d %s", game_types[c->game_type].scores[score_index],
				game_types[c->game_type].unit);
		if (selected)
		{
			char arrows[56];

			snprintf(arrows, sizeof(arrows), "< %s >", value);
			n64_text_draw(menu.small, 200 - n64_text_width(menu.small, arrows), y + 15, TEXT_BRIGHT, arrows);
		}
		else
			n64_text_draw(menu.small, 200 - n64_text_width(menu.small, value), y + 15, TEXT_COLOR, value);
	}
	/* the map's and the game type's pictures */
	panel(212, 40, 312, 92, 0xC0000000);
	blit(picture(menu.map_pictures, "map", maps[c->map].picture), 212, 41, 100.f / 128.f);
	frame_line(212, 40, 312, 92, 0xFF8CC0FF);
	panel(229, 100, 295, 134, 0xC0000000);
	blit(picture(menu.type_pictures, "gametype", game_types[c->game_type].picture), 230, 101, 0.5f);
	frame_line(229, 100, 295, 134, 0xFF8CC0FF);
	/* the players */
	for (int p = 0; p < 4; p++)
	{
		int x = 12 + p * 75, y = 148;
		int slot = -1;

		for (int i = 0; i < c->player_count; i++)
		{
			if (c->ports[i] == p)
				slot = i;
		}
		panel(x, y, x + 71, y + 58, slot >= 0 ? 0xA0102040 : 0x60000000);
		frame_line(x, y, x + 71, y + 58, slot >= 0 ? 0xFF8CC0FF : 0x806080A0);
		if (slot < 0)
		{
			text_centered(menu.small, x + 36, y + 26, joypad_is_connected(p) ? TEXT_COLOR : TEXT_DIM,
				joypad_is_connected(p) ? "PRESS A" : "NO PAD");
			text_centered(menu.small, x + 36, y + 40, TEXT_DIM, joypad_is_connected(p) ? "TO JOIN" : "");
			continue;
		}
		snprintf(value, sizeof(value), "PLAYER %d", p + 1);
		text_centered(menu.small, x + 36, y + 16, TEXT_BRIGHT, value);
		panel(x + 8, y + 22, x + 63, y + 36, 0xFF000000 | (teams ? team_rgb[c->teams[slot]] : player_rgb[slot & 3]));
		if (teams)
			text_centered(menu.small, x + 36, y + 33, TEXT_BRIGHT, c->teams[slot] ? "BLUE" : "RED");
		text_centered(menu.small, x + 36, y + 52, TEXT_DIM, teams ? "Z: TEAM" : "B: LEAVE");
	}
	text_centered(menu.small, 160, 226, TEXT_DIM, "A JOIN    B LEAVE    Z TEAM    START PLAY");
}

static void draw_results(const char *const *lines, int count)
{
	panel(40, 40, 280, 200, 0xC0001028);
	frame_line(40, 40, 280, 200, 0xFF8CC0FF);
	text_centered(menu.large, 160, 66, TEXT_BRIGHT, "GAME OVER");
	for (int i = 0; i < count && i < 6; i++)
		text_centered(menu.small, 160, 92 + i * 16, i == 0 ? TEXT_BRIGHT : TEXT_COLOR, lines[i]);
	text_centered(menu.small, 160, 192, TEXT_DIM, "PRESS A");
}

static void draw_loading(const struct n64_menu_choice *c)
{
	char text[64];

	panel(0, 96, 320, 144, 0xC0000818);
	snprintf(text, sizeof(text), "%s ON %s", game_types[c->game_type].name, maps[c->map].name);
	text_centered(menu.large, 160, 118, TEXT_BRIGHT, "LOADING");
	text_centered(menu.small, 160, 136, TEXT_COLOR, text);
}

/* ---------- the menu */

int n64_menu_run(struct n64_menu_choice *c, const char *const *results, int result_count)
{
	static int score_index = -1;
	int screen = result_count > 0 ? SCREEN_RESULTS : (c->player_count ? SCREEN_SETUP : SCREEN_TITLE);
	int row = ROW_GAME_TYPE, item = TITLE_MULTIPLAYER, setting = SETTING_CHUNKY;

	menu_load();
	/* the menu itself always at full width */
	n64_render_set_chunky(0);
	n64_render_show_counter(0);
	if (c->map < 0 || c->map >= MAP_COUNT)
		c->map = n64_menu_map_from_folder("wizard");
	if (c->game_type < 0 || c->game_type >= GAME_TYPE_COUNT)
		c->game_type = 0;
	if (score_index < 0)
		score_index = game_types[c->game_type].default_score;
	for (;;)
	{
		int start = 0;

		menu.frame += 1.f;
		read_pads();
		if (screen == SCREEN_RESULTS)
		{
			for (int p = 0; p < 4; p++)
			{
				if (pads[p].pressed.a || pads[p].pressed.start)
					screen = SCREEN_SETUP;
			}
		}
		else if (screen == SCREEN_TITLE)
		{
			for (int p = 0; p < 4 && screen == SCREEN_TITLE; p++)
			{
				if (pads[p].up)
					item = (item + TITLE_COUNT - 1) % TITLE_COUNT;
				if (pads[p].down)
					item = (item + 1) % TITLE_COUNT;
				if (!pads[p].pressed.start && !pads[p].pressed.a)
					continue;
				if (item == TITLE_SETTINGS)
				{
					screen = SCREEN_SETTINGS;
					setting = SETTING_CHUNKY;
					continue;
				}
				screen = SCREEN_SETUP;
				row = ROW_GAME_TYPE;
				/* the player who pressed is in */
				if (!c->player_count)
				{
					c->ports[0] = p;
					c->teams[0] = 0;
					c->player_count = 1;
				}
			}
		}
		else if (screen == SCREEN_SETTINGS)
		{
			for (int p = 0; p < 4 && screen == SCREEN_SETTINGS; p++)
			{
				struct pad *pad = &pads[p];

				if (pad->up)
					setting = (setting + SETTING_COUNT - 1) % SETTING_COUNT;
				if (pad->down)
					setting = (setting + 1) % SETTING_COUNT;
				if (setting == SETTING_CHUNKY && (pad->left || pad->right || pad->pressed.a))
					menu.chunky = !menu.chunky;
				if (pad->pressed.b || pad->pressed.start || (setting == SETTING_BACK && pad->pressed.a))
					screen = SCREEN_TITLE;
			}
		}
		else
		{
			for (int p = 0; p < 4; p++)
			{
				struct pad *pad = &pads[p];
				int slot = -1;

				for (int i = 0; i < c->player_count; i++)
				{
					if (c->ports[i] == p)
						slot = i;
				}
				if (slot < 0)
				{
					/* A joins (on the START row it starts the game, once
					someone has) */
					if (pad->pressed.a && !(row == ROW_START && c->player_count) && c->player_count < 4)
					{
						c->ports[c->player_count] = p;
						c->teams[c->player_count] = c->player_count & 1;
						c->player_count++;
						continue;
					}
					if (pad->pressed.b && !c->player_count)
						screen = SCREEN_TITLE;
				}
				else
				{
					if (pad->pressed.b)
					{
						for (int i = slot; i + 1 < c->player_count; i++)
						{
							c->ports[i] = c->ports[i + 1];
							c->teams[i] = c->teams[i + 1];
						}
						c->player_count--;
						if (!c->player_count)
							screen = SCREEN_TITLE;
						continue;
					}
					if (pad->pressed.z || pad->pressed.r)
						c->teams[slot] ^= 1;
				}
				if (pad->up)
					row = (row + ROW_COUNT - 1) % ROW_COUNT;
				if (pad->down)
					row = (row + 1) % ROW_COUNT;
				if (pad->left || pad->right)
				{
					int step = pad->right ? 1 : -1;

					if (row == ROW_GAME_TYPE)
					{
						c->game_type = (c->game_type + GAME_TYPE_COUNT + step) % GAME_TYPE_COUNT;
						score_index = game_types[c->game_type].default_score;
					}
					else if (row == ROW_MAP)
						c->map = (c->map + MAP_COUNT + step) % MAP_COUNT;
					else if (row == ROW_SCORE)
						score_index = (score_index + 5 + step) % 5;
				}
				if (pad->pressed.start || (pad->pressed.a && row == ROW_START && slot >= 0))
					start = c->player_count > 0;
			}
		}
		c->score = game_types[c->game_type].scores[score_index];
		for (int i = 0; i < c->player_count; i++)
			c->colors[i] = n64_menu_game_teams(c->game_type) ? (c->teams[i] ? 3 : 2) : player_colors[i & 3];

		n64_render_frame_begin();
		draw_scene();
		if (screen == SCREEN_RESULTS)
			draw_results(results, result_count);
		else if (screen == SCREEN_TITLE)
			draw_title(item);
		else if (screen == SCREEN_SETTINGS)
			draw_settings(setting);
		else if (start)
			draw_loading(c);
		else
			draw_setup(c, row, score_index);
		n64_render_frame_end();
		if (start)
		{
			/* the game in chunky mode or not from its first frame */
			n64_render_set_chunky(menu.chunky);
			n64_render_show_counter(1);
			n64_render_set_clear_color(0x60708C);
			menu_unload();
			return 1;
		}
	}
}
