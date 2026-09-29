/*
XBOX_DISPLAY.C

The screen and the input helpers the game's native-port code asks the
platform layer for (port/linux/include/halo_linux_source_fixups.h): on the
console the screen is the Xbox's 640x480, the menus have no mouse pointer,
and frames are drawn one per tick as the retail game did.
*/

#include "platform.h"
#include "port_config.h"
#include "halo_ui_pointer.h"

/* 640, or 854 on a widescreen display: the device squeezes them into the
720x480 buffer (d3d8_nv2a.c), and the television stretches them back */
long halo_screen_width(void)
{
	return (long)nx_video_logical_width();
}

/* the game draws the display's shape (854 columns for 16:9): each column
is shown as wide as it is tall */
float halo_pixel_aspect(void)
{
	return 1.0f;
}

/* the frame as a texture is addressed in its texels: this many per column
the game draws */
float halo_screen_texture_scale_x(void)
{
	return (float)nx_video_physical_width() / (float)nx_video_logical_width();
}

/* where the processor's time goes: every main loop pass's times from the
game's profiler, summed, and logged every five seconds as each part's
share of a pass (the idle part is the wait for the next vertical blank;
"other" is input, the network, sound and the rest of the pass) */
void halo_frame_profile(float frame, float ticks, long tick_count, float render, float idle, float texture)
{
	static float total, total_ticks, total_render, total_idle, total_texture, worst;
	static long passes, tick_total;

	total += frame;
	total_ticks += ticks;
	total_render += render;
	total_idle += idle;
	total_texture += texture;
	tick_total += tick_count;
	passes++;
	if (frame > worst)
		worst = frame;
	if (total >= 5000.0f)
	{
		float per_pass = 1.0f / (float)passes;

		platform_log("cpu: %ld passes, %ld ticks in %.1f s; a pass %.1f ms (worst %.1f): ticks %.1f (%.1f a tick), "
			"render %.1f, texture %.1f, idle %.1f, other %.1f",
			passes, tick_total, total / 1000.0f, total * per_pass, worst, total_ticks * per_pass,
			tick_total ? total_ticks / (float)tick_total : 0.0f, total_render * per_pass, total_texture * per_pass,
			total_idle * per_pass, (total - total_ticks - total_render - total_idle) * per_pass);
		total = total_ticks = total_render = total_idle = total_texture = worst = 0.0f;
		passes = tick_total = 0;
	}
}

/* performance mode: a performance.txt on the disc (tools/xbox_disc.py
--performance); rasterizer.c turns the costliest effects off for it */
int halo_performance_mode(void)
{
	static int checked, mode;

	if (!checked)
	{
		checked = 1;
		mode = GetFileAttributesA("D:\\performance.txt") != 0xFFFFFFFFUL;
		if (mode)
			platform_log("performance mode: D:\\performance.txt is on the disc");
	}
	return mode;
}

/* the width the game draws this frame (rasterizer_screen_width_update
asks every frame): the same as above; the console's screen never changes */
long halo_screen_commit(void)
{
	return (long)nx_video_logical_width();
}

/* the menus and full-screen overlays are laid out for 640 columns: while
they draw, everything shifts right to centre them on the wider screen */
void halo_screen_ui_offset(unsigned char centered)
{
	d3d8_nv2a_set_ui_offset(centered ? (long)(nx_video_logical_width() - 640) / 2 : 0);
}

int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	(void)menus_active;
	(void)pointer;
	return 0;
}

/* frames between the 30 Hz ticks (port/linux/game/render_interpolation.c):
display.unlocked_framerate in the disc's config.toml; off draws one frame
per tick as the retail game did */
int halo_interpolation_enabled(void)
{
	static int checked, enabled;

	if (!checked)
	{
		checked = 1;
		enabled = config_boolean("display.unlocked_framerate");
		if (enabled)
			platform_log("display.unlocked_framerate: frames between the game's ticks, up to the display's rate");
	}
	return enabled;
}

/* the mouse's aim (port/linux/src/xinput_sdl.c): there is none */
int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch)
{
	(void)gamepad_index;
	*yaw = 0.0f;
	*pitch = 0.0f;
	return 0;
}
