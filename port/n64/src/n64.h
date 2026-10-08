/*
N64.H

The bridge between the two halves of the N64 platform layer. Units compiled
with the game's ABI and the XDK declarations (xdk_*.c, the game itself)
cannot include libdragon's headers (both declare the same names), so the
libdragon-facing units (n64_*.c) export what the game side needs through
these plain-C declarations.
*/

#ifndef __HALO_N64_H
#define __HALO_N64_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- system (n64_main.c) */

/* debug output, one line per call (no newline needed) */
void n64_log(const char *text);
/* a fatal error: shows the message on screen and stops */
void n64_fatal(const char *text) __attribute__((noreturn));
/* prints the call stack to the debug output */
void n64_backtrace(void);
/* the CPU's count register: 46.875 MHz, wraps every 91 s */
unsigned long n64_ticks(void);
/* the same counter in 64 bits, never wrapping */
unsigned long long n64_ticks64(void);
#define N64_TICKS_PER_SECOND 46875000
/* microseconds since boot, 64 bits */
unsigned long long n64_microseconds(void);
/* busy-waits */
void n64_wait_microseconds(unsigned long microseconds);
/* memory left on the heap, and the largest free block */
unsigned long n64_heap_free(void);

/* TRUE when the cartridge carries the file rom:/<name> (debug switches,
port.json "flags"); looked up once per name */
int n64_debug_flag(const char *name);
/* a debug setting's value (port.json flag "name=value"): 0 when unset */
int n64_debug_text(const char *name, char *buffer, int size);
/* the profiler samples only inside a zone with the flag profile_zone */
extern volatile int n64_profile_zone;

/* ---------- cartridge files (n64_files.c): DragonFS, read-only */

/* returns a handle (>= 0) or -1 */
long n64_file_open(const char *path);
long n64_file_size(long handle);
long n64_file_read(long handle, void *buffer, long size);
long n64_file_seek(long handle, long offset);
void n64_file_close(long handle);

/* ---------- animation frames streamed from the cartridge (n64_anim.c) */

void n64_animation_open(const char *map_name);
void *n64_animation_frame(long data_offset, long data_size, short frame_index, short frame_size);

/* ---------- controllers (n64_input.c) */

struct n64_pad
{
	unsigned char connected;
	unsigned char pad[3];
	signed char stick_x, stick_y;
	unsigned short buttons; /* N64_BUTTON_* */
};

enum
{
	N64_BUTTON_A = 1 << 0,
	N64_BUTTON_B = 1 << 1,
	N64_BUTTON_Z = 1 << 2,
	N64_BUTTON_START = 1 << 3,
	N64_BUTTON_D_UP = 1 << 4,
	N64_BUTTON_D_DOWN = 1 << 5,
	N64_BUTTON_D_LEFT = 1 << 6,
	N64_BUTTON_D_RIGHT = 1 << 7,
	N64_BUTTON_L = 1 << 8,
	N64_BUTTON_R = 1 << 9,
	N64_BUTTON_C_UP = 1 << 10,
	N64_BUTTON_C_DOWN = 1 << 11,
	N64_BUTTON_C_LEFT = 1 << 12,
	N64_BUTTON_C_RIGHT = 1 << 13,
};

void n64_input_poll(void);
void n64_input_get(int port, struct n64_pad *pad);

/* ---------- the renderer (n64_render.c)

A frame is n64_render_frame_begin, one n64_render_view per player's view,
then n64_render_frame_end, which shows it. Positions are the game's world
units; the views' rectangles are pixels of the 320x240 frame. */

struct n64_view
{
	float position[3];
	float forward[3];
	float up[3];
	float vertical_field_of_view;
	float z_near, z_far;
	short x0, y0, x1, y1;
	short local_player_index;
	short pad;
};

/* an object to draw in the current view: its model (the model tag's index),
its node matrices as the game keeps them (real_matrix4x3: scale, forward,
left, up, position; world units), the permutation of each region, and its
bounding sphere */
struct n64_object
{
	short model_tag_index;
	short node_count;
	const float *node_matrices;
	const unsigned char *region_permutations;
	short region_count;
	short pad;
	float center[3];
	float radius;
	/* the colour the model's light is doubled by: white for most objects,
	a player's armour colour for players */
	unsigned char tint[4];
};

void n64_render_initialize(void);
/* loads rom:/levels/<map>.lvl and its textures; FALSE if there is none */
int n64_render_load_level(const char *map_name);
void n64_render_frame_begin(void);
void n64_render_view(const struct n64_view *view);
/* after n64_render_view: objects in that view */
void n64_render_object(const struct n64_object *object);
/* then the first-person weapon: its own near plane, drawn over the world */
void n64_render_first_person_begin(void);
void n64_render_frame_end(void);

/* ---------- the HUD (n64_hud.c): drawn by the game's own HUD code
(port/n64/game/n64_hud_game.c passes its quads, text and blips on).
Positions are pixels of the 320x240 frame, texture coordinates texels of
the half-size HUD sprite. */

struct n64_hud_quad
{
	short sprite;
	short pad;
	float x[4], y[4];
	float s[4], t[4];
	/* ARGB; the sprites are stored straight (not premultiplied): an opaque
	colour blends over, a transparent one adds (Halo's HUD blends
	premultiplied, colour + framebuffer * (1 - alpha)) */
	unsigned long color;
	/* a meter (Halo's meter shader, rasterizer_xbox_dynavobgeom.c): the
	sprite's intensity is each texel's fill order f, its alpha the shape.
	Texels with f under the flash colour's alpha are full, coloured from
	minimum_color to maximum_color by f (plus the flash colour where f is at
	least the minimum colour's alpha), the rest background_color; all scaled
	by the tint's intensity, over the framebuffer times the tint's alpha
	(full) or the background's (empty) */
	int meter;
	unsigned long minimum_color, maximum_color, flash_color, background_color, tint_color;
};

/* a new map: its sprites are rom:/hud/<map>/<n>.sprite */
void n64_hud_set_map(const char *map_name);
/* a view's HUD: 1 when the game should draw it now (it is recorded and
shown until the view's next one), 0 when the last one is shown again (the
game's HUD code runs for each view every other frame) */
int n64_hud_begin(int view, int x0, int y0, int x1, int y1);
void n64_hud_end(void);
void n64_hud_quad(const struct n64_hud_quad *quad);
void n64_hud_text(int x, int y, unsigned long color, const char *text);
/* a motion sensor blip: centre in pixels, position and size in pixels */
void n64_hud_blip(float x, float y, float size, unsigned long color);

#ifdef __cplusplus
}
#endif

#endif /* __HALO_N64_H */
