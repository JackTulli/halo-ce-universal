/*
N64_GAME.C

The game-side half of the N64 platform layer, compiled like the game's own
units (it sees the game's headers):

- the start of a game without menus (n64_game_setup, called from main.c),
- the memory map: the game state and the loaded map are ordinary
  allocations (physical_memory_map.c is left out),
- the cache files: maps are converted ahead of time (tools/n64/convert.py)
  and read from the cartridge whole; the Xbox's copy-to-hard-disk cache
  (cache_files_windows.c) is left out,
- the game state's buffer and its (absent) persistent storage,
- the hooks the Linux port added to the game (interpolation, screen width).
*/

#include "cseries.h"

#include "ai/ai_debug.h"
#include "cache/cache_files.h"
#include "cache/physical_memory_map.h"
#include "game/game.h"
#include "main/main.h"
#include "saved games/game_state.h"
#include "game/players.h"
#include "objects/objects.h"
#include "memory/data.h"
#include "units/units.h"
#include "items/weapons.h"

#include "../src/n64.h"
#include "errors.h"

#include <stdlib.h>
#include <string.h>

void *memalign(size_t alignment, size_t size);

extern short player_spawn_count;

/* ---------- settings of the first milestone */

#define N64_MAP_NAME "levels\\test\\wizard\\wizard"
#define N64_GAME_VARIANT "slayer"
#define N64_PLAYER_COUNT 2

/* ---------- the start of a game */

void n64_game_setup(
	void)
{
	/* ai_debug_initialize's defaults (the AI is left out) */
	ai_debug.selected_actor_index = NONE;
	ai_debug.selected_squad_index = NONE;

	game_set_game_variant_from_name(N64_GAME_VARIANT);
	player_spawn_count = N64_PLAYER_COUNT;
	main_set_map_name(N64_MAP_NAME);
	main_disallow_persistent_storage();
	n64_log("n64: " N64_GAME_VARIANT " on " N64_MAP_NAME);
}

/* ---------- memory map */

static struct
{
	void *game_state;
	void *tag_cache;
} n64_memory;

void physical_memory_allocate(
	void)
{
	/* game_state.c allocates its own buffer (game_state_allocate_buffer) */
	return;
}

void physical_memory_verify(
	void)
{
	return;
}

void physical_memory_free(
	void)
{
	return;
}

void *physical_memory_get_game_state_base_address(
	void)
{
	return n64_memory.game_state;
}

void *physical_memory_get_tag_cache_base_address(
	void)
{
	return n64_memory.tag_cache;
}

void *physical_memory_get_texture_cache_base_address(
	void)
{
	return NULL;
}

void *physical_memory_get_sound_cache_base_address(
	void)
{
	return NULL;
}

/* ---------- the game state's buffer */

void *game_state_allocate_buffer(
	unsigned long address,
	unsigned long cpu_size,
	unsigned long gpu_size)
{
	(void)address;
	n64_memory.game_state = memalign(16, cpu_size + gpu_size);
	if (!n64_memory.game_state)
		n64_fatal("no memory for the game state");
	memset(n64_memory.game_state, 0, cpu_size + gpu_size);
	return n64_memory.game_state;
}

void game_state_free_buffer(
	void)
{
	return;
}

void game_state_create_or_open_file(void) { return; }
void game_state_close_file(void) { return; }
boolean game_state_write_core(char const *name, void *buffer, long size) { (void)name; (void)buffer; (void)size; return FALSE; }
boolean game_state_read_core_header(char const *name, void *header, long size) { (void)name; (void)header; (void)size; return FALSE; }
boolean game_state_write_to_file(void) { return FALSE; }
void game_state_read_core(char const *name, void *buffer, long size) { (void)name; (void)buffer; (void)size; return; }

/* ---------- maps

An N64 map (tools/n64/convert.py): a 64-byte header, the image, then the
image offsets of its pointers. The image is copied into one allocation and
every pointer moved to it. */

struct n64_map_header
{
	char magic[4];
	unsigned long version;
	char name[32];
	unsigned long image_size;
	unsigned long relocation_count;
	unsigned long tag_header_offset;
	unsigned long structure_bsp_header_offset;
	unsigned long reserved[2];
};

#define N64_MAP_VERSION 1

static struct
{
	char name[32];
	byte *image;
	unsigned long image_size;
	struct n64_map_header header;
} n64_map;

static void *n64_map_load(
	char const *name)
{
	char path[64];
	long file;
	unsigned long remaining, offset;
	unsigned long *relocations;
	unsigned long base;

	if (n64_map.image && !strcmp(n64_map.name, name))
		return n64_map.image + n64_map.header.tag_header_offset;
	if (n64_map.image)
	{
		free(n64_map.image);
		n64_map.image = NULL;
	}

	snprintf(path, sizeof(path), "rom:/maps/%s.map", name);
	file = n64_file_open(path);
	if (file < 0)
	{
		error(_error_silent, "n64: no map %s", path);
		return NULL;
	}
	n64_file_read(file, &n64_map.header, sizeof(n64_map.header));
	if (memcmp(n64_map.header.magic, "H64M", 4) || n64_map.header.version != N64_MAP_VERSION)
		n64_fatal("the map was converted for another build (tools/n64/convert.py)");

	n64_map.image = memalign(16, n64_map.header.image_size);
	if (!n64_map.image)
		n64_fatal("no memory for the map");
	n64_map.image_size = n64_map.header.image_size;
	n64_file_read(file, n64_map.image, n64_map.header.image_size);

	/* the relocations follow the image, 8-byte aligned */
	offset = (sizeof(n64_map.header) + n64_map.header.image_size + 7) & ~7UL;
	n64_file_seek(file, offset);
	base = (unsigned long)n64_map.image;
	relocations = malloc(4096);
	remaining = n64_map.header.relocation_count;
	while (remaining)
	{
		unsigned long count = remaining < 1024 ? remaining : 1024;
		unsigned long i;

		n64_file_read(file, relocations, count * 4);
		for (i = 0; i < count; i++)
			*(unsigned long *)(n64_map.image + relocations[i]) += base;
		remaining -= count;
	}
	free(relocations);
	n64_file_close(file);

	csstrncpy(n64_map.name, name, sizeof(n64_map.name) - 1);
	n64_animation_open(name);
	n64_memory.tag_cache = n64_map.image;
	{
		char line[128];

		snprintf(line, sizeof(line), "n64: map %s loaded, %lu KB, %lu relocations, %lu KB of heap left",
			name, n64_map.header.image_size / 1024, n64_map.header.relocation_count, n64_heap_free() / 1024);
		n64_log(line);
	}
	return n64_map.image + n64_map.header.tag_header_offset;
}

/* the loaded map's name ("wizard"), for the renderer's level geometry */
char const *n64_loaded_map_name(
	void)
{
	return n64_map.name;
}

/* cache_files.c (HALO_N64): the loaded map's tag header */
void *n64_cache_file_tag_header(
	char const *scenario_name)
{
	return n64_map_load(tag_name_strip_path(scenario_name));
}

/* ---------- the cache files' platform half (cache_files_windows.c) */

void cache_files_initialize(void) { return; }
void cache_files_dispose(void) { return; }
void cache_files_precache_set_priority(boolean blocking) { (void)blocking; return; }
boolean cache_files_precache_in_progress(void) { return FALSE; }
boolean cache_files_precache_is_copying_map(const char *map_name) { (void)map_name; return FALSE; }
boolean cache_files_precache_map_loaded(const char *map_name) { (void)map_name; return TRUE; }
boolean cache_files_precache_map_begin(const char *map_name, boolean copy_map) { (void)map_name; (void)copy_map; return TRUE; }
void cache_files_precache_map_end(void) { return; }
void cache_files_precache_map_queue_end(void) { return; }
short cache_files_precache_map_status(real *progress) { if (progress) *progress = 1.f; return 1; }
void cache_file_promote_read(short request) { (void)request; return; }
void cache_file_block_until_not_busy(void) { return; }
void cache_file_close(void) { return; }

boolean cache_file_open(
	char const *map_name,
	struct cache_file_header *header)
{
	(void)map_name;
	(void)header;
	return TRUE;
}

short cache_file_read(
	long tag_index,
	long offset,
	long size,
	void *buffer,
	boolean *completion_flag_reference,
	boolean blocking)
{
	(void)tag_index;
	(void)offset;
	(void)size;
	(void)buffer;
	(void)blocking;
	if (completion_flag_reference)
		*completion_flag_reference = TRUE;
	return NONE;
}

void tags_header_register_vertex_and_index_buffers(struct cache_file_tag_header *header) { (void)header; return; }
void tags_header_deregister_vertex_and_index_buffers(struct cache_file_tag_header *header) { (void)header; return; }
void structure_bsp_header_register_vertex_buffers(struct cache_file_structure_bsp_header *header) { (void)header; return; }
void structure_bsp_header_deregister_vertex_buffers(struct cache_file_structure_bsp_header *header) { (void)header; return; }

/* ---------- the Linux port's hooks into the game */

int halo_interpolation_enabled(void) { return 0; }
long halo_screen_width(void) { return 640; }
int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch) { (void)gamepad_index; (void)yaw; (void)pitch; return 0; }
void network_lobby_update(void) { return; }
void render_interpolation_tick(void) { return; }
void render_interpolation_frame_begin(void) { return; }
void render_interpolation_frame_end(void) { return; }
void render_interpolation_dispose_from_old_map(void) { return; }
float render_interpolation_fraction(void) { return 1.f; }
struct real_matrix4x3 *render_interpolation_object_node_matrices(long object_index) { (void)object_index; return NULL; }
struct observer_result const *render_interpolation_camera(short local_player_index, struct observer_result const *observer) { (void)local_player_index; return observer; }
void render_interpolation_first_person(short local_player_index, struct real_matrix4x3 *node_matrices,
	short node_count, struct render_camera const *camera) { (void)local_player_index; (void)node_matrices; (void)node_count; (void)camera; return; }
float render_interpolation_game_time_sec(long ticks) { return (float)ticks / 30.f; }
float halo_pixel_aspect(void) { return 1.f; }
float halo_screen_texture_scale_x(void) { return 1.f; }
int halo_performance_mode(void) { return 1; }
long halo_screen_commit(void) { return 640; }
void halo_screen_ui_offset(unsigned char centered) { (void)centered; return; }
void p2p_lobby_report(const void *data, long size) { (void)data; (void)size; return; }
long p2p_peer_round_trip(unsigned long address) { (void)address; return -1; }
void p2p_accept_remote_connections(int accept) { (void)accept; return; }

/* ---------- the stack walk (cseries/stack_walk_windows.c walks x86 frames) */

void stack_walk(short levels_to_ignore) { (void)levels_to_ignore; n64_backtrace(); }
void stack_walk_initialize(void) { return; }
void stack_walk_dispose(void) { return; }
void stack_walk_disregard_symbol_names(boolean disregard) { (void)disregard; return; }

/* ---------- the tick's cost (linked with --wrap=game_tick)

Every game tick is timed; every 150 ticks (5 s of game time) the log gets
the average and the worst, and how far game time keeps up with real time. */

void __real_game_tick(void);

/* game.c's phases of a tick (HALO_N64), in CPU ticks, summed over the log
window */
unsigned long n64_tick_phases[8];
/* objects.c's objects_update, by object type (HALO_N64) */
unsigned long n64_object_type_time[16], n64_object_type_count[16];
/* objects.c's object_update, by stage */
unsigned long n64_object_stage_time[8];
static char const *const n64_object_stage_names[5] =
{
	"type update", "damage/export", "node matrices", "functions/colours", "lights",
};
static char const *const n64_object_type_names[12] =
{
	"biped", "vehicle", "weapon", "equipment", "garbage", "projectile", "scenery", "machine", "control",
	"light fixture", "placeholder", "sound scenery",
};
static char const *const n64_tick_phase_names[8] =
{
	"quit/allegiance", "units", "players before", "effects", "first person", "engine", "objects", "players after/hud",
};

/* one line per player: where, whether alive, kills, deaths, shots */
static void n64_log_players(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		char line[160];

		if (player->unit_index != NONE)
		{
			struct unit_datum *unit = unit_get(player->unit_index);
			long weapon_index = unit->unit.current_weapon_index >= 0 ?
				unit->unit.weapon_object_indices[unit->unit.current_weapon_index] : NONE;
			char const *weapon_name = "none";
			int rounds = -1;

			if (weapon_index != NONE)
			{
				struct weapon_datum *weapon = weapon_get(weapon_index);

				weapon_name = tag_name_strip_path(tag_get_name(object_get(weapon_index)->definition_index));
				rounds = weapon->weapon.magazines[0].rounds_loaded;
			}
			snprintf(line, sizeof(line), "player %d: alive at %.1f %.1f %.1f, %s (%d rounds), kills %d, deaths %d, colour %.2f %.2f %.2f / base %.2f %.2f %.2f",
				(int)iterator.absolute_index,
				unit->object.position.x, unit->object.position.y, unit->object.position.z,
				weapon_name, rounds, player->statistics.kills[0], player->statistics.deaths,
				unit->object.outgoing_change_colors[0].red, unit->object.outgoing_change_colors[0].green,
				unit->object.outgoing_change_colors[0].blue, unit->object.base_change_colors[0].red,
				unit->object.base_change_colors[0].green, unit->object.base_change_colors[0].blue);
		}
		else
		{
			snprintf(line, sizeof(line), "player %d: dead (respawn in %ld), kills %d, deaths %d",
				(int)iterator.absolute_index, player->respawn_timer,
				player->statistics.kills[0], player->statistics.deaths);
		}
		n64_log(line);
	}
}

static struct
{
	unsigned long count;
	unsigned long long total;
	unsigned long worst;
	unsigned long long window_start;
} n64_tick_profile;

void __wrap_game_tick(
	void)
{
	unsigned long start = n64_ticks();
	unsigned long elapsed;

	if (!n64_tick_profile.window_start)
		n64_tick_profile.window_start = n64_ticks64();
	__real_game_tick();
	elapsed = n64_ticks() - start;
	{
		extern unsigned long n64_last_tick_us;

		n64_last_tick_us = (unsigned long)((unsigned long long)elapsed * 1000000 / N64_TICKS_PER_SECOND);
	}
	n64_tick_profile.total += elapsed;
	if (elapsed > n64_tick_profile.worst)
		n64_tick_profile.worst = elapsed;
	if (++n64_tick_profile.count == 150)
	{
		char line[160];
		unsigned long long now = n64_ticks64();
		unsigned long average_us = (unsigned long)(n64_tick_profile.total * 1000000 / N64_TICKS_PER_SECOND / 150);
		unsigned long worst_us = (unsigned long)((unsigned long long)n64_tick_profile.worst * 1000000 / N64_TICKS_PER_SECOND);
		unsigned long window_ms = (unsigned long)((now - n64_tick_profile.window_start) * 1000 / N64_TICKS_PER_SECOND);

		snprintf(line, sizeof(line), "tick: %lu.%02lu ms average, %lu.%02lu ms worst; 150 ticks (5 s of game) took %lu ms",
			average_us / 1000, (average_us % 1000) / 10, worst_us / 1000, (worst_us % 1000) / 10, window_ms);
		n64_log(line);
		{
			char phases[200];
			int length = 0;
			int i;

			for (i = 0; i < 8; i++)
			{
				length += snprintf(phases + length, sizeof(phases) - length, "%s %lu.%lu ", n64_tick_phase_names[i],
					(unsigned long)((unsigned long long)n64_tick_phases[i] * 1000 / N64_TICKS_PER_SECOND / 150),
					(unsigned long)((unsigned long long)n64_tick_phases[i] * 10000 / N64_TICKS_PER_SECOND / 150 % 10));
				n64_tick_phases[i] = 0;
			}
			n64_log(phases);
			length = 0;
			for (i = 0; i < 12; i++)
			{
				if (!n64_object_type_count[i])
					continue;
				length += snprintf(phases + length, sizeof(phases) - length, "%s %lu.%02lu ms x%lu ", n64_object_type_names[i],
					(unsigned long)((unsigned long long)n64_object_type_time[i] * 1000 / N64_TICKS_PER_SECOND / 150),
					(unsigned long)((unsigned long long)n64_object_type_time[i] * 100000 / N64_TICKS_PER_SECOND / 150 % 100),
					n64_object_type_count[i] / 150);
			}
			for (i = 0; i < 16; i++)
				n64_object_type_time[i] = n64_object_type_count[i] = 0;
			n64_log(phases);
			length = 0;
			for (i = 0; i < 5; i++)
			{
				length += snprintf(phases + length, sizeof(phases) - length, "%s %lu.%02lu ", n64_object_stage_names[i],
					(unsigned long)((unsigned long long)n64_object_stage_time[i] * 1000 / N64_TICKS_PER_SECOND / 150),
					(unsigned long)((unsigned long long)n64_object_stage_time[i] * 100000 / N64_TICKS_PER_SECOND / 150 % 100));
				n64_object_stage_time[i] = 0;
			}
			n64_log(phases);
		}
		n64_log_players();
		memset(&n64_tick_profile, 0, sizeof(n64_tick_profile));
		n64_tick_profile.window_start = now;
	}
}

/* ---------- player colours (saved games/player_profile.c is left out) */

static long const n64_profile_color_table[18] =
{
	0x00FFFFFF, 0x00000000, 0x00FE0000, 0x000201E3, 0x00707E71, 0x00FFFF01,
	0x0000FF01, 0x00FF56B9, 0x00AB10F4, 0x0001FFFF, 0x006493ED, 0x00FF7F00,
	0x001ECC91, 0x00006401, 0x00603814, 0x00C69C6C, 0x009D0B0E, 0x00F5999E,
};

real_rgb_color player_profile_get_rgb_color(
	long color_index)
{
	real_rgb_color rgb_color;
	long color;

	color_index = PIN(color_index, 0, NUMBEROF(n64_profile_color_table) - 1);
	color = n64_profile_color_table[color_index];
	rgb_color.red = ((color >> 16) & 0xFF) / 255.f;
	rgb_color.green = ((color >> 8) & 0xFF) / 255.f;
	rgb_color.blue = (color & 0xFF) / 255.f;
	return rgb_color;
}

word player_profile_number_of_available_primary_colors(void) { return NUMBEROF(n64_profile_color_table); }
