/*
HALO_LINUX_SOURCE_FIXUPS.H

Game-only workarounds for source that MSVC accepts but clang rejects, where
editing the source itself would change the byte-matched MSVC output (see
port/linux/README.md for how each was checked).
*/

#ifndef __HALO_LINUX_SOURCE_FIXUPS_H
#define __HALO_LINUX_SOURCE_FIXUPS_H

/* rasterizer.h declares rasterizer_debug_drawing_begin(boolean opaque) while
rasterizer_xbox_debug.h declares a second `long zbias` parameter, and
rasterizer_debug.c includes both and passes two arguments. MSVC tolerates
the mismatch; the definition ignores zbias. Adding the parameter to
rasterizer.h perturbs MSVC's register allocation elsewhere, so instead every
declaration and call collapses to the one-parameter form here. */
#define rasterizer_debug_drawing_begin(opaque, ...) (rasterizer_debug_drawing_begin)(opaque)

/* frames between the 30 Hz ticks (port/linux/game/render_interpolation.c);
the platform layer reads the display.interpolation setting */
struct observer_result;
struct render_camera;
struct real_matrix4x3;
int halo_interpolation_enabled(void);
float game_time_get_tick_fraction(void);
void render_interpolation_tick(void);
void render_interpolation_frame_begin(void);
void render_interpolation_frame_end(void);
void render_interpolation_dispose_from_old_map(void);
float render_interpolation_fraction(void);
struct real_matrix4x3 *render_interpolation_object_node_matrices(long object_index);
struct observer_result const *render_interpolation_camera(short local_player_index,
	struct observer_result const *observer);
void render_interpolation_first_person(short local_player_index, struct real_matrix4x3 *node_matrices,
	short node_count, struct render_camera const *camera);
float render_interpolation_game_time_sec(long ticks);

/* the width of the screen the game draws, 480 lines tall: the device's or
the display's shape, or 640 (port/linux/src/d3d8_gl.c) */
long halo_screen_width(void);
/* how much wider than tall the display shows a pixel: 1 on a display
that shows the frame's shape, 4/3 for the console's anamorphic widescreen
mode (the view widens to match, render_cameras.c) */
float halo_pixel_aspect(void);
/* the field of view setting (display.field_of_view) as a factor on the
tangent of the view's half angle; 1 at the game's 70 degrees
(port/linux/src/port_config.c) */
float halo_field_of_view_tangent_scale(void);
/* a [graphics] setting of config.toml (graphics.<name>): whether that
effect may be drawn (rasterizer.c) */
int halo_graphics_option(const char *name);
/* display.fps_counter: show the frame rate in the top left (main.c) */
int halo_fps_counter_enabled(void);
/* debug.console: the developer console and its lines on the screen; and
debug.log_file: whether debug.txt is written (port_config.c) */
int halo_debug_console_enabled(void);
int halo_debug_log_file_enabled(void);
/* a line under the frame rate (the console's 720p: on, or why not); NULL
for none */
const char *halo_display_notice(void);
/* how many texels of the frame, taken as a texture, one column the game
draws is: 1, unless the console squeezes its wider screen into its buffer
(the screen effects address the frame in its texels) */
float halo_screen_texture_scale_x(void);
/* and one line: 1, or 1.5 at the console's 720p (480 lines scaled to 720) */
float halo_screen_texture_scale_y(void);
/* the Xbox build's texture cache: 12 MB, or 22 MB at 720p on a 128 MB
console (port/xbox/src/xbox_display.c) */
unsigned long halo_xbox_texture_cache_size(void);
/* the Xbox build's sound cache (physical_memory_map.c): ordinary memory at
720p in the low 64 MB, contiguous otherwise */
void *halo_xbox_sound_cache_alloc(unsigned long size);
void halo_xbox_sound_cache_free(void *memory);
/* performance mode: performance.txt in the game's folder (the disc on the
console); rasterizer.c turns the costliest effects off, the on-screen log
stays off (interface.c) */
int halo_performance_mode(void);
#if !defined(HALO_XBOX) && !defined(HALO_ANDROID)
/* the light pipeline, of the desktop builds (rasterizer.c): the game asks
the device only for passes a card without shaders can draw -- the
environment's lightmap and base map in one pass, no bump mapping, specular,
reflections, shadows or screen effects. On when the device's renderer is
gl1 (d3d8_gl.c), or as graphics.light_pipeline says: -1 for "auto", else 0
or 1 (port_config.c) */
#define HALO_LIGHT_PIPELINE 1
unsigned char rasterizer_light_pipeline(void);
int halo_renderer_is_light(void);
int halo_light_pipeline_setting(void);
#endif
/* takes up a new width between frames (F11); returns the width */
long halo_screen_commit(void);
/* while TRUE, drawing shifts right to center 640-column layouts */
void halo_screen_ui_offset(unsigned char centered);
#ifdef HALO_XBOX
/* the console: each main loop pass's times from the game's profiler
(profile.c), in milliseconds, summed and logged every few seconds
(port/xbox/src/xbox_display.c) */
void halo_frame_profile(float frame, float ticks, long tick_count, float render, float idle, float texture);
/* waits up to milliseconds for the next vertical blank's callback
(port/xbox/src/d3d8_nv2a.c) */
void halo_xbox_wait_vertical_blank(unsigned long milliseconds);
/* the game tick's parts since the last call (source/game/game.c: the AI,
units, effects, objects and interpolation, and the actor and object
counts), written into text, the accumulators cleared; the ticks covered */
long halo_xbox_tick_profile(char *text, long size, double milliseconds_per_cycle);
/* the device's frame beginning (port/xbox/src/d3d8_nv2a.c): the next
frame's buffer and target, which Present leaves until it is drawn */
void halo_xbox_frame_begin(void);
#endif
/* internet play (port/linux/src/p2p.c): the hosted lobby's report for the
master server (port/linux/game/network_lobby.c), and the round trip in
milliseconds to the tunnel's peer at an address (network byte order), or
-1 */
void p2p_lobby_report(const void *data, long size);
long p2p_peer_round_trip(unsigned long address);
void network_lobby_update(void);
/* whether the game hosting now takes machines from elsewhere: a
split-screen game's server does not, and is not listed on the master
server (port/linux/src/p2p.c) */
void p2p_accept_remote_connections(int accept);
/* the mouse in the menus (source/interface/ui_widget.c) */
#include "halo_ui_pointer.h"

#endif
