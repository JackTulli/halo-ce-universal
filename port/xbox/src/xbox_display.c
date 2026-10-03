/*
XBOX_DISPLAY.C

The screen and the input helpers the game's native-port code asks the
platform layer for (port/linux/include/halo_linux_source_fixups.h): on the
console the screen is the Xbox's 640x480 (854x480 on a widescreen display
or at 720p), the menus have no mouse pointer, and frames are drawn one per
tick as the retail game did.
*/

#include "platform.h"
#include "port_config.h"
#include "halo_ui_pointer.h"

/* 720p if display.hd_720p asks for it and the console can (nx_video.c):
decided on the game's first question about its screen, when the settings
can be read and the GPU has not started (Direct3D_CreateDevice asks too) */
void halo_xbox_choose_display_mode(void)
{
	static int chosen;

	if (chosen)
		return;
	chosen = 1;
	/* (the first point the settings are read: the log's too) */
	nx_log_network(config_boolean("debug.network_log"));
	nx_video_upgrade_hd(config_boolean("display.hd_720p"));
}

/* 640, or 854 on a widescreen display or at 720p: the device squeezes
them into the 720x480 buffer (d3d8_nv2a.c), and the television stretches
them back, or scales them into the 1280x720 one */
long halo_screen_width(void)
{
	halo_xbox_choose_display_mode();
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
	halo_xbox_choose_display_mode();
	return (float)nx_video_physical_width() / (float)nx_video_logical_width();
}

/* the texture cache (physical_memory_map.c, xbox_texture_cache.c): the
retail 22 MB where the game's memory is in the top 64 MB of a 128 MB
console (720p), 12 MB in a 64 MB console's. Fixed once asked: the game
allocates it after its first question about the screen, which chose the
mode */
unsigned long halo_xbox_texture_cache_size(void)
{
	static unsigned long size;

	if (!size)
	{
		halo_xbox_choose_display_mode();
		/* 22 MB in the top 64 MB; 10 at 720p in the low 64 MB (9 on a 64
		MB console), whose frame buffers need the room; 12 otherwise */
		size = nx_high_memory_placed() ? 0x1600000UL : nx_video_low_memory_hd() ? nx_video_hd_texture_cache_bytes() :
			0xC00000UL;
		platform_log("texture cache: %lu MB", size >> 20);
	}
	return size;
}

/* the sound cache (physical_memory_map.c): only the processor reads it (the
mixer, dsound_sdl.c), so at 720p in the low 64 MB it is ordinary memory,
which the kernel can give from the top 64 MB of a 128 MB console */
void *halo_xbox_sound_cache_alloc(unsigned long size)
{
	halo_xbox_choose_display_mode();
	if (nx_video_low_memory_hd())
	{
		void *memory = nx_cpu_alloc(size);

		if (memory)
		{
			/* where the kernel took it from (above 64 MB is what 720p
			counts on), for the log and the screen's line */
			unsigned long physical = nx_physical_address(memory);

			platform_log("sound cache: %lu KB of ordinary memory at %p (physical %08lx)", size / 1024, memory, physical);
			nx_video_append_notice(physical >= 0x4000000UL ? "; sound above 64 MB" : "; sound below 64 MB");
			return memory;
		}
	}
	return nx_physical_alloc(size, 0xffffffffUL, 0, PAGE_READWRITE);
}

void halo_xbox_sound_cache_free(void *memory)
{
	if ((unsigned long)memory >= PLATFORM_CONTIGUOUS_BASE)
		nx_physical_free(memory);
	else
		nx_cpu_free(memory);
}

/* the display mode's line under the frame rate (main.c): "720p", or why
not, when display.hd_720p asked for it, if display.mode_notice is on */
const char *halo_display_notice(void)
{
	static int checked, shown;

	if (!checked)
	{
		shown = config_boolean("display.mode_notice");
		checked = 1;
	}
	if (!shown)
		return NULL;
	halo_xbox_choose_display_mode();
	return *nx_video_notice() ? nx_video_notice() : NULL;
}

/* and per line: 1.5 at 720p */
float halo_screen_texture_scale_y(void)
{
	halo_xbox_choose_display_mode();
	return (float)nx_video_physical_height() / 480.0f;
}

/* (source/game/game.c) the game tick's parts since the last call */
long halo_xbox_tick_profile(char *text, long size, double milliseconds_per_cycle);

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
		{
			/* and the tick's parts (game.c), timed with the cycle counter: its
			rate measured over the same window (under emulation it is not the
			console's 733 MHz) */
			static unsigned long long window_cycles, window_microseconds;
			unsigned long long cycles = __builtin_ia32_rdtsc(), microseconds = nx_microseconds();
			char text[400];

			if (window_cycles && cycles > window_cycles)
			{
				double milliseconds_per_cycle =
					(double)(microseconds - window_microseconds) / 1000.0 / (double)(cycles - window_cycles);

				if (halo_xbox_tick_profile(text, sizeof(text), milliseconds_per_cycle) > 0)
					platform_log("ticks: %s", text);
			}
			else
			{
				halo_xbox_tick_profile(text, sizeof(text), 1.0 / 733333.333);
			}
			window_cycles = cycles;
			window_microseconds = microseconds;
		}
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
	halo_xbox_choose_display_mode();
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
