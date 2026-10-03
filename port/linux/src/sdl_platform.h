/*
SDL_PLATFORM.H

Window, OpenGL context and input state shared by the renderer and the
controller emulation (see sdl_platform.c).
*/

#ifndef __HALO_LINUX_SDL_PLATFORM_H
#define __HALO_LINUX_SDL_PLATFORM_H

#ifdef HALO_WIN9X
/* Windows 98/ME has no SDL: SDL's numbering all the same, and the platform
layer behind it (port/win9x/platform/w9x_platform.c) */
#include "../../win9x/platform/w9x_scancodes.h"
#include "../../win9x/platform/w9x_backend.h"
#else
#include <SDL3/SDL_scancode.h>
#endif

#define PLATFORM_MOUSE_BUTTON_COUNT 8

struct platform_input_state
{
	unsigned char keys[SDL_SCANCODE_COUNT];
	unsigned char mouse_buttons[PLATFORM_MOUSE_BUTTON_COUNT]; /* SDL_BUTTON_* */
	float mouse_dx, mouse_dy;
	float mouse_wheel;
	BOOL focused;
	BOOL mouse_released;
	/* the mouse drives the menus' pointer (platform_ui_pointer_set_active)
	instead of the controller */
	BOOL ui_pointer;
};

struct platform_keystroke
{
	BYTE virtual_key;
	CHAR ascii;
	BYTE flags;
};

BOOL platform_sdl_initialize(void);
/* creates the window and makes its OpenGL context current on this thread */
BOOL platform_video_initialize(unsigned long width, unsigned long height);
#ifndef HALO_ANDROID
BOOL platform_screen_mode(long *width, long *height);
#endif
void platform_video_drawable_size(int *width, int *height);
void platform_video_swap(void);
#if !defined(HALO_ANDROID) && !defined(HALO_WIN9X)
/* whether the context is the OpenGL 4.5 core one, which is asked for first
unless display.renderer names an older renderer; otherwise it is a
compatibility or plain context of whatever version the driver has
(sdl_platform.c). Windows 98/ME only has the latter: platform_video_info */
BOOL platform_video_core_context(void);
/* the depth and stencil bits of the window's own pixel format */
void platform_video_window_bits(int *depth, int *stencil);
#endif
/* frames between the 30 Hz ticks at the display's refresh rate, unless
display.interpolation is false (port/linux/game/render_interpolation.c) */
int halo_interpolation_enabled(void);
void platform_mouse_capture(BOOL capture);

/* main thread only; a no-op elsewhere */
void platform_pump_events(void);
/* a snapshot of the input state; consume_motion resets the mouse deltas */
void platform_input_read(struct platform_input_state *state, BOOL consume_motion);
#ifndef HALO_ANDROID
/* the pointer in the menus (d3d8_gl.c, halo_ui_pointer_update) */
struct platform_ui_pointer
{
	/* in window coordinates, as SDL reports them */
	float x, y;
	float click_x, click_y;
	BOOL moved;
	int left_clicks, right_clicks;
	int wheel_steps;
};
void platform_ui_pointer_set_active(BOOL active);
BOOL platform_ui_pointer_read(struct platform_ui_pointer *pointer);
void platform_video_window_size(int *width, int *height);
#endif
BOOL platform_next_keystroke(struct platform_keystroke *keystroke);
#ifdef HALO_WIN9X
/* the window, the display mode and the OpenGL context as they turned out:
the pixel format's bits, the driver's strings and extensions, and whether
it is accelerated (zeroes before the window opens) */
const struct w9x_video_result *platform_video_info(void);
#endif

#endif
