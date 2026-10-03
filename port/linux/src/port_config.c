/*
PORT_CONFIG.C

The native ports' settings (port_config.h), parsed with tomlc17
(port/third_party/tomlc17). Every setting is in the table below with its
type, default, the HALO_* environment variable that overrides it and the
comment written into a new file. The file is read once, on the first
question; unknown keys and values of the wrong type are reported in the log
and the defaults used instead, and the file itself is never rewritten once
it exists, so that the player's edits and comments stay.
*/

#include "platform.h"
#include "port_config.h"
#include "tomlc17.h"

#ifdef HALO_WIN9X
/* Windows 98/ME has no SDL: the executable's folder from the platform
layer (port/win9x/platform) */
#include <pthread.h>
#include "../../win9x/platform/w9x_backend.h"
#elif !defined(HALO_XBOX)
#include <SDL3/SDL.h>
#include <pthread.h>
#endif
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HALO_WIN9X) || defined(HALO_WINDOWS)
/* the Windows builds' settings window (launcher.h), shown after the file
is read and before anything reads a setting */
#define CONFIG_LAUNCHER 1
#include "launcher.h"
#include "posix.h"
#endif

/* ---------- the settings */

enum config_type
{
	_config_boolean,
	_config_integer,
	_config_real,
	_config_string,
};

/* how the setting's environment variable sets it */
enum config_environment
{
	/* the variable's text is the value ("0", "false", "no" and "off" are
	false for a boolean) */
	_environment_value,
	/* the variable being set at all makes it true */
	_environment_set_is_true,
	/* the variable being set at all makes it false */
	_environment_set_is_false,
};

/* the builds a setting means something in, and is written for */
enum
{
	_platform_desktop = 1,
	_platform_android = 2,
	_platform_console = 4,
	_platform_all = _platform_desktop | _platform_android | _platform_console,
	/* the desktop builds and Android, not the console: the console has no
	OpenGL renderer, window, clipboard or display rate to choose, and takes
	its language from the dashboard (its config.toml lists what it reads) */
	_platform_computers = _platform_desktop | _platform_android,
	/* Windows 98/ME (HALO_WIN9X), whose settings of its own are only in its
	table; a desktop build, it reads the desktop settings too */
	_platform_win9x = 8,
};

#ifdef HALO_WIN9X
/* a frame between every two ticks is more than the Pentium IIIs of the
time draw */
#define CONFIG_INTERPOLATION_DEFAULT "false"
/* no Discord client runs on 98/ME (p2p.c leaves Discord out there) */
#define CONFIG_DISCORD_PLATFORMS 0
#else
#define CONFIG_INTERPOLATION_DEFAULT "true"
#define CONFIG_DISCORD_PLATFORMS _platform_desktop
#endif

struct config_setting
{
	const char *name;
	enum config_type type;
	/* as it is written in the file */
	const char *default_value;
	const char *environment;
	enum config_environment environment_style;
	unsigned platforms;
	const char *comment;
};

static const struct config_setting config_settings[] =
{
	{ "display.fullscreen", _config_boolean, "true", "HALO_FULLSCREEN", _environment_value, _platform_desktop,
		"Start fullscreen, drawing at the display's resolution and shape; false\n"
		"starts in a window, which draws the Xbox's 640x480. F11 switches." },
	{ "display.window_scale", _config_integer, "2", "HALO_WINDOW_SCALE", _environment_value, _platform_desktop,
		"The window's size as a multiple of 640x480 (it can be resized)." },
	{ "display.screen_width", _config_integer, "0", "HALO_SCREEN_WIDTH", _environment_value, _platform_android,
		"Columns of the 480-line picture: 0 for the display's shape, 640 for the\n"
		"Xbox's 4:3." },
	{ "display.vsync", _config_boolean, "true", "HALO_NO_VSYNC", _environment_set_is_false, _platform_computers,
		"Wait for the display between frames; false draws as fast as possible." },
	{ "display.renderer", _config_string, "\"auto\"", "HALO_RENDERER", _environment_value, _platform_desktop,
		"Which renderer draws: \"gl4\" needs OpenGL 4.5; \"gl2\" is the same picture\n"
		"on OpenGL 2.0 (GLSL 1.10), for older cards and drivers and virtual\n"
		"machines; \"gl1\" is the light renderer for cards without shaders (OpenGL\n"
		"1.1, fewer effects). \"auto\" takes the first of them the driver can run;\n"
		"the log says which and why." },
#ifndef HALO_XBOX
	{ "display.gl1_texture_bits", _config_integer, "16", "HALO_GL1_TEXTURE_BITS", _environment_value, _platform_desktop,
		"The light renderer's textures (display.renderer \"gl1\"): 16 bits a texel,\n"
		"which the cards it is for draw fastest and have the memory for, or 32." },
#endif
	{ "display.interpolation", _config_boolean, CONFIG_INTERPOLATION_DEFAULT, "HALO_INTERPOLATION", _environment_value,
		_platform_computers,
		"Draw a frame for every display refresh, blending between the game's 30\n"
		"ticks a second; false keeps the original 30 frames a second." },
	{ "display.field_of_view", _config_real, "70.0", "HALO_FIELD_OF_VIEW", _environment_value, _platform_all,
		"The field of view in degrees, across a 4:3 screen (the game's is 70);\n"
		"a widescreen display shows more at the sides as always. Zoomed views\n"
		"keep their magnification. 40 to 150." },
	{ "display.mode_notice", _config_boolean, "false", "HALO_MODE_NOTICE", _environment_value, _platform_console,
		"With display.fps_counter on, show the display mode under the frame rate:\n"
		"\"720p\", \"720p (in 64 MB: ...)\", or why 720p is not on." },
	{ "display.fps_counter", _config_boolean, "false", "HALO_FPS_COUNTER", _environment_value, _platform_all,
		"Show the frame rate in the top left during play: frames per second over\n"
		"the last half second, green at 30 or more, red below." },
	{ "display.hd_720p", _config_boolean, "false", "HALO_HD_720P", _environment_value, _platform_console,
		"The console: 720p (1280x720, 16:9) instead of 480. It needs a console\n"
		"with 128 MB of memory, the HDTV (component) cable and 720p turned on\n"
		"in the dashboard's video settings; without any of them the game stays\n"
		"at 480 and its log says why. Heavy scenes are slower at 720p." },
	{ "display.unlocked_framerate", _config_boolean, "false", "HALO_UNLOCKED_FRAMERATE", _environment_value,
		_platform_all,
		"The console: draw as many frames as the display shows (60 a second),\n"
		"with movement interpolated between the game's 30 ticks; off draws one\n"
		"frame per tick, as the retail game did. The desktop builds have\n"
		"display.interpolation and display.vsync instead." },
#ifdef HALO_WIN9X
	{ "display.mode", _config_string, "\"640x480\"", "HALO_DISPLAY_MODE", _environment_value, _platform_win9x,
		"Windows 98/ME: the display mode fullscreen changes to, \"WIDTHxHEIGHT\" at\n"
		"the most colours the card has for it, or \"WIDTHxHEIGHTxBITS\" (16, 24 or\n"
		"32); \"desktop\" keeps the desktop's. The game draws 480 lines, so 640x480\n"
		"costs the least." },
	{ "display.allow_software_gl", _config_boolean, "false", "HALO_ALLOW_SOFTWARE_GL", _environment_value,
		_platform_win9x,
		"Windows 98/ME: run on Microsoft's own OpenGL (\"GDI Generic\", no 3D\n"
		"hardware) when the display driver has none; a few frames a second at\n"
		"best." },
#endif

	/* the renderer's effects (rasterizer.c, halo_graphics_option): each
	can only turn an effect off, so performance.txt's set stays off */
	{ "graphics.shadows", _config_boolean, "true", "HALO_GRAPHICS_SHADOWS", _environment_value, _platform_all,
		"Objects' shadows on the environment." },
	{ "graphics.soft_shadows", _config_boolean, "true", "HALO_GRAPHICS_SOFT_SHADOWS", _environment_value, _platform_all,
		"Blur the shadows' edges (off: hard edges, cheaper)." },
	{ "graphics.dynamic_lights", _config_boolean, "true", "HALO_GRAPHICS_DYNAMIC_LIGHTS", _environment_value, _platform_all,
		"Dynamic lights (muzzle flashes, plasma, flashlights) on the environment." },
	{ "graphics.specular", _config_boolean, "true", "HALO_GRAPHICS_SPECULAR", _environment_value, _platform_all,
		"Specular highlights on the environment." },
	{ "graphics.reflections", _config_boolean, "true", "HALO_GRAPHICS_REFLECTIONS", _environment_value, _platform_all,
		"Reflections on the environment (cube maps and mirrors)." },
	{ "graphics.decals", _config_boolean, "true", "HALO_GRAPHICS_DECALS", _environment_value, _platform_all,
		"Decals: bullet holes, scorch marks, blood." },
	{ "graphics.detail_objects", _config_boolean, "true", "HALO_GRAPHICS_DETAIL_OBJECTS", _environment_value, _platform_all,
		"Grass and other small detail objects." },
	{ "graphics.bump_mapping", _config_boolean, "true", "HALO_GRAPHICS_BUMP_MAPPING", _environment_value, _platform_all,
		"Bump mapping on the environment." },
	{ "graphics.lens_flares", _config_boolean, "true", "HALO_GRAPHICS_LENS_FLARES", _environment_value, _platform_all,
		"Lens flares." },
	{ "graphics.lens_flare_occlusion", _config_boolean, "true", "HALO_GRAPHICS_LENS_FLARE_OCCLUSION", _environment_value,
		_platform_all,
		"Hide lens flares behind geometry by testing their visibility on the GPU\n"
		"(off: cheaper; flares show through walls)." },
	{ "graphics.sun_rays", _config_boolean, "true", "HALO_GRAPHICS_SUN_RAYS", _environment_value, _platform_all,
		"The sun's rays through the fog." },
	{ "graphics.fog", _config_boolean, "true", "HALO_GRAPHICS_FOG", _environment_value, _platform_all,
		"Atmospheric and planar fog." },
	{ "graphics.particles", _config_boolean, "true", "HALO_GRAPHICS_PARTICLES", _environment_value, _platform_all,
		"Particles and particle systems (smoke, sparks, debris)." },
	{ "graphics.weather", _config_boolean, "true", "HALO_GRAPHICS_WEATHER", _environment_value, _platform_all,
		"Weather (rain, snow)." },
	{ "graphics.screen_flashes", _config_boolean, "true", "HALO_GRAPHICS_SCREEN_FLASHES", _environment_value, _platform_all,
		"Full-screen flashes (damage, pickups)." },
	{ "graphics.screen_effects", _config_boolean, "true", "HALO_GRAPHICS_SCREEN_EFFECTS", _environment_value, _platform_all,
		"Full-screen effects (the scope's blur, night vision)." },
	{ "graphics.active_camouflage_quality", _config_boolean, "true", "HALO_GRAPHICS_CAMOUFLAGE_QUALITY", _environment_value,
		_platform_all,
		"Active camouflage's second pass (off: cheaper, flatter camouflage)." },
#ifndef HALO_XBOX
	/* (the light pipeline, rasterizer.c: not the console's) */
	{ "graphics.light_pipeline", _config_string, "\"auto\"", "HALO_GRAPHICS_LIGHT_PIPELINE", _environment_value,
		_platform_desktop,
		"The light pipeline: the game draws only what a card without shaders can\n"
		"(the environment's lightmap and texture in one pass; no bump mapping,\n"
		"specular, reflections, shadows, dynamic lights or screen effects).\n"
		"\"auto\" turns it on with the gl1 renderer; \"true\" also with gl2 and gl4,\n"
		"to see its picture on any card; \"false\" never." },
	{ "graphics.detail_maps", _config_boolean, "false", "HALO_GRAPHICS_DETAIL_MAPS", _environment_value,
		_platform_desktop,
		"With the light pipeline: the environment's primary detail maps, drawn as\n"
		"one more pass over it (the full pipeline always draws them)." },
#endif

	{ "audio.enabled", _config_boolean, "true", "HALO_NO_AUDIO", _environment_set_is_false, _platform_all,
		"Play sound." },
	{ "audio.volume", _config_real, "1.0", "HALO_VOLUME", _environment_value, _platform_all,
		"The volume of everything, 0.0 to 1.0." },
#ifdef HALO_WIN9X
	{ "audio.output", _config_string, "\"auto\"", "HALO_AUDIO_OUTPUT", _environment_value, _platform_win9x,
		"Windows 98/ME: \"dsound\" plays through DirectSound, \"waveout\" through the\n"
		"older wave mapper (more delay; for drivers whose DirectSound misbehaves);\n"
		"\"auto\" takes DirectSound where it works." },
	{ "audio.rate", _config_integer, "0", "HALO_AUDIO_RATE", _environment_value, _platform_win9x,
		"Windows 98/ME: the output's samples a second (22050, 44100 or 48000); 0\n"
		"takes the first of 44100, 48000 and 22050 the sound card plays as it is.\n"
		"22050 halves the mixer's work on a slow processor." },
#endif
	{ "audio.backend", _config_string, "\"auto\"", "HALO_AUDIO_BACKEND", _environment_value, _platform_console,
		"The console's output path: \"apu\" plays through the audio processor's\n"
		"voice processor, which the emulator's sound comes from; \"ac97\" through\n"
		"the AC97 controller's DMA, nxdk's way, which a console plays; \"auto\"\n"
		"picks by the AC97 codec's identity (the emulator's SigmaTel against the\n"
		"console's Analog Devices part)." },

	{ "input.mouse_sensitivity", _config_real, "1.0", "HALO_MOUSE_SENSITIVITY", _environment_value, _platform_desktop,
		"How far the view turns for the mouse's movement." },
	{ "input.invert_mouse", _config_boolean, "false", "HALO_MOUSE_INVERT", _environment_set_is_true, _platform_desktop,
		"Moving the mouse forward looks down." },
#ifdef HALO_WIN9X
	{ "input.gamepad_map", _config_string, "\"\"", "HALO_GAMEPAD_MAP", _environment_value, _platform_win9x,
		"Windows 98/ME: which of a gamepad's buttons and axes are the Xbox\n"
		"controller's, in SDL's mapping syntax (a Windows line of\n"
		"SDL_GameControllerDB can be pasted). Empty means \"a:b0,b:b1,x:b2,y:b3,\n"
		"leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,back:b8,\n"
		"start:b9,leftstick:b10,rightstick:b11,dpup:h0.1,dpright:h0.2,dpdown:h0.4,\n"
		"dpleft:h0.8,leftx:a0,lefty:a1,rightx:a2,righty:a5\". Axes a0 to a7 are X,\n"
		"Y, Z, Rx, Ry, Rz and the two sliders; +a2 and -a2 are halves of one, a2~\n"
		"turns one round. The shoulders are the white and black buttons." },
#endif

#ifndef HALO_XBOX
	{ "launcher.show", _config_boolean, "true", "HALO_LAUNCHER", _environment_value, _platform_desktop,
		"Show the settings window when the game starts, where the build has one\n"
		"(Windows, Windows 98/ME): the display, graphics, sound and input\n"
		"settings, and the rest of this file. -launcher on the command line\n"
		"shows it whatever this says, -nolauncher skips it." },
#endif

	{ "game.language", _config_string, "\"\"", "HALO_LANGUAGE", _environment_value, _platform_computers,
		"The language the game asks the Xbox for: \"ja\", \"de\", \"fr\", \"es\" or \"it\";\n"
		"empty for English. The game data decides what is translated." },

	{ "paths.data", _config_string, "\"\"", "HALO_DATA_ROOT", _environment_value, _platform_desktop,
		"The folder holding the game data's maps folder; empty looks in the\n"
		"working directory and its assets folder. Windows paths are easiest in\n"
		"single quotes: 'C:\\Games\\Halo'." },
	{ "paths.saves", _config_string, "\"\"", "HALO_SAVE_ROOT", _environment_value, _platform_desktop,
		"Where saved games and profiles go; empty for the usual place\n"
		"(~/.local/share/halo-linux, or %APPDATA%\\halo on Windows)." },

	{ "network.address", _config_string, "\"\"", "HALO_NET_ADDRESS", _environment_value, _platform_all,
		"This machine's IPv4 address for system link, for a machine on several\n"
		"networks; empty chooses one." },
	{ "network.broadcast", _config_string, "\"\"", "HALO_NET_BROADCAST", _environment_value, _platform_all,
		"Comma-separated IPv4 addresses system link sends its announcements to\n"
		"instead of the local network's broadcast address (for VPNs); empty for\n"
		"the local network." },
	{ "network.netcode", _config_string, "\"distributed\"", "HALO_NETCODE", _environment_value, _platform_all,
		"\"distributed\" (work in progress, port/linux/NETCODE.md) predicts each\n"
		"player's own moves and lets the host decide the rest; \"lockstep\" plays\n"
		"system link as the Xbox game did. Every machine must use the same." },
	{ "network.online", _config_boolean, "true", "HALO_NET_ONLINE", _environment_value, _platform_all,
		"Internet play: a hosted game is listed on the master server, where\n"
		"everyone's System Link screen shows it, and gets an invite link (logged,\n"
		"and put on the clipboard) that also joins it. Off keeps system link to\n"
		"the local network." },
	{ "network.join_from_clipboard", _config_boolean, "true", "HALO_NET_JOIN_FROM_CLIPBOARD", _environment_value,
		_platform_computers,
		"Join the game of an invite link found on the clipboard when the game\n"
		"comes to the front." },
	{ "network.tunnel_port", _config_integer, "0", "HALO_NET_TUNNEL_PORT", _environment_value, _platform_all,
		"The UDP port internet play uses; 0 picks one. A fixed one can be\n"
		"forwarded on the router, for networks whose NAT stops connections." },
	{ "network.master_server", _config_string, "\"172.104.30.93:27920\"", "HALO_NET_MASTER", _environment_value,
		_platform_all,
		"The master server (host:port, UDP; port/master) that lists games on the\n"
		"internet under Multiplayer, System Link, and through which machines\n"
		"find each other; empty lists nothing and answers no invite." },
	{ "network.stun_servers", _config_string, "\"stun.l.google.com:19302,stun.cloudflare.com:3478\"",
		"HALO_NET_STUN", _environment_value, _platform_all,
		"Public STUN servers that tell this machine its internet address;\n"
		"comma-separated host:port." },
	{ "discord.application_id", _config_string, "\"1553978809840050229\"", "HALO_DISCORD_APPLICATION",
		_environment_value, CONFIG_DISCORD_PLATFORMS,
		"The Discord application internet play invites go through while the\n"
		"Discord desktop client runs; empty for none." },

	{ "debug.console", _config_boolean, "true", "HALO_CONSOLE", _environment_value, _platform_all,
		"The developer console and its lines on the screen during play (the\n"
		"errors and messages the game prints there). Off hides the lines, and the\n"
		"console (the ` key on a keyboard) does not open." },
	{ "debug.log_file", _config_boolean, "false", "HALO_LOG_FILE", _environment_value, _platform_all,
		"Write the game's log, debug.txt (on the console: on the cache\n"
		"partition, X: over FTP). Off (the default) writes nothing and spares\n"
		"the disk the writes; on for reporting a problem." },
	{ "debug.network_log", _config_boolean, "false", "HALO_NETWORK_LOG", _environment_value, _platform_console,
		"Send the console's log over the network as it runs (UDP port 9999, to\n"
		"every machine of the network; tools/xbox_log.py keeps it on a computer).\n"
		"Off (the default) sends nothing once the settings are read." },

	{ "debug.network_test", _config_string, "\"\"", "HALO_NETWORK_TEST", _environment_value, _platform_all,
		"Automated system link sessions for testing (port/linux/game/network_test.c):\n"
		"\"host:<map>\" hosts a game on that map, \"join\" joins the first game found;\n"
		"empty for none." },
	{ "debug.network_test_start", _config_real, "15.0", "HALO_NETWORK_TEST_START", _environment_value, _platform_all,
		"Seconds after hosting that an automated test game starts." },
	{ "debug.network_test_kill", _config_real, "0.0", "HALO_NETWORK_TEST_KILL", _environment_value, _platform_all,
		"Every this many seconds an automated test host kills its last player; 0 never." },
	{ "debug.network_test_shoot", _config_real, "0.0", "HALO_NETWORK_TEST_SHOOT", _environment_value, _platform_all,
		"Every this many seconds each automated test player hits the next with\n"
		"their weapon; 0 never." },
	{ "debug.network_test_vehicle", _config_real, "0.0", "HALO_NETWORK_TEST_VEHICLE", _environment_value, _platform_all,
		"This many seconds into an automated test game the host seats its last\n"
		"player as a vehicle's driver (and out 15 seconds on); 0 never." },
	{ "debug.network_test_pickup", _config_real, "0.0", "HALO_NETWORK_TEST_PICKUP", _environment_value, _platform_all,
		"This many seconds into an automated test game the host stands its last\n"
		"player on a weapon, which a joining player then picks up; 0 never." },
	{ "debug.network_latency", _config_real, "0.0", "HALO_NETWORK_LATENCY", _environment_value, _platform_all,
		"Milliseconds everything received is held back (a round trip between two\n"
		"machines of twice it), to test the netcode as over the internet; 0 none." },
	{ "debug.network_loss", _config_real, "0.0", "HALO_NETWORK_LOSS", _environment_value, _platform_all,
		"Percent of datagrams received that are dropped, for the same; 0 none." },
	{ "debug.test_input", _config_string, "\"\"", "HALO_TEST_INPUT", _environment_value, _platform_computers,
		"\"bot:<seed>\" plays controller 1 with a scripted pattern (automated\n"
		"network tests); empty for none." },
	{ "debug.exit_after", _config_real, "0.0", "HALO_EXIT_AFTER", _environment_value, _platform_computers,
		"Quit this many seconds after the window opens; 0 never." },
	{ "debug.hidden_window", _config_boolean, "false", "HALO_HIDDEN_WINDOW", _environment_set_is_true, _platform_desktop,
		"Keep the window hidden (and never fullscreen)." },
	{ "debug.null_renderer", _config_boolean, "false", "HALO_NULL_RENDERER", _environment_set_is_true, _platform_computers,
		"Run without a window, drawing nothing." },
	{ "debug.gl_debug", _config_boolean, "false", "HALO_GL_DEBUG", _environment_set_is_true, _platform_computers,
		"Report OpenGL errors in the log." },
	{ "debug.gl_disable", _config_string, "\"\"", "HALO_GL_DISABLE", _environment_value, _platform_desktop,
		"Comma-separated things of OpenGL the renderer is to do without, as if\n"
		"the driver lacked them, to test what it does instead: extensions\n"
		"(\"EXT_framebuffer_object\") and the renderer's own names for what came\n"
		"after OpenGL 2.0 (clip_control, integer_attributes, bgra_attributes,\n"
		"sampler_objects, base_vertex, copy_buffer, query_buffer, debug_output,\n"
		"framebuffer_objects, depth_textures, packed_depth_stencil,\n"
		"framebuffer_blit, copy_image, s3tc, s3tc_3d, anisotropy, large_streams,\n"
		"depth_replace). Empty for none." },
#ifndef HALO_XBOX
	{ "debug.gl1_extensions", _config_string, "\"all\"", "HALO_GL1_EXTENSIONS", _environment_value, _platform_desktop,
		"The OpenGL extensions the light renderer (display.renderer \"gl1\") may\n"
		"use, comma-separated, to try on a capable card what it does without\n"
		"them: \"all\" for every one the driver has, \"none\" for plain OpenGL 1.1,\n"
		"or names (ARB_multitexture, EXT_texture_env_combine,\n"
		"ARB_texture_env_combine, EXT_texture_env_add, ARB_texture_env_dot3,\n"
		"EXT_bgra, EXT_packed_pixels, EXT_texture_compression_s3tc,\n"
		"EXT_compiled_vertex_array, EXT_blend_color, EXT_blend_minmax,\n"
		"EXT_blend_subtract, EXT_texture_edge_clamp,\n"
		"ARB_texture_mirrored_repeat, SGIS_texture_lod). \"simple\" besides draws\n"
		"everything as its first texture times the vertex colour, without the\n"
		"plans it otherwise makes of the Xbox's pixel shaders." },
#endif
	{ "debug.gpu_stats", _config_boolean, "false", "HALO_GPU_STATS", _environment_set_is_true, _platform_computers,
		"Log the renderer's draw counts once a second." },
	{ "debug.gpu_trace_frame", _config_integer, "-1", "HALO_GPU_TRACE", _environment_value, _platform_computers,
		"Log every draw of this frame; -1 none." },
	{ "debug.gpu_trace_constants", _config_boolean, "false", "HALO_GPU_TRACE_CONSTANTS", _environment_set_is_true, _platform_computers,
		"With gpu_trace_frame, also the vertex shader constants." },
	{ "debug.gpu_skip_vertex_shaders", _config_string, "\"\"", "HALO_GPU_SKIP_VS", _environment_value, _platform_computers,
		"Comma-separated ids of vertex shaders not to draw with." },
	{ "debug.gpu_dump_shaders", _config_string, "\"\"", "HALO_GPU_DUMP_SHADERS", _environment_value, _platform_computers,
		"A folder to write the generated GLSL to; empty none." },
	{ "debug.gpu_debug_expression", _config_string, "\"\"", "HALO_GPU_DEBUG_EXPR", _environment_value, _platform_computers,
		"A GLSL expression every pixel shader shows instead of its result." },
	{ "debug.gpu_debug_texture0", _config_boolean, "false", "HALO_GPU_DEBUG_T0", _environment_set_is_true, _platform_computers,
		"Pixel shaders show their first texture." },
	{ "debug.gpu_debug_flat", _config_boolean, "false", "HALO_GPU_DEBUG_FLAT", _environment_set_is_true, _platform_computers,
		"Pixel shaders show their vertex colour." },
	{ "debug.screenshot_directory", _config_string, "\"\"", "HALO_SCREENSHOT_DIR", _environment_value, _platform_computers,
		"A folder to save frames to (with screenshot_every); empty none." },
	{ "debug.screenshot_every", _config_integer, "0", "HALO_SCREENSHOT_EVERY", _environment_value, _platform_computers,
		"Save every this many frames to screenshot_directory; 0 none." },
	{ "debug.texture_dump_directory", _config_string, "\"\"", "HALO_TEXTURE_DUMP", _environment_value, _platform_computers,
		"A folder to write every texture to as it is uploaded; empty none." },
	{ "debug.texture_log", _config_boolean, "false", "HALO_TEXTURE_LOG", _environment_set_is_true, _platform_computers,
		"Log texture uploads." },
	{ "debug.texture_no_cache", _config_boolean, "false", "HALO_TEXTURE_NO_CACHE", _environment_set_is_true, _platform_computers,
		"Upload textures again every time they are used." },
	{ "debug.sample_seconds", _config_real, "0.0", "HALO_SAMPLE", _environment_value, _platform_android,
		"Log where every game thread is this often, in seconds (read by the\n"
		"app, port/android/host/host_debug.c); 0 never." },
};

#define NUMBER_OF_CONFIG_SETTINGS (sizeof(config_settings) / sizeof(config_settings[0]))

#ifdef HALO_ANDROID
#define CONFIG_PLATFORM _platform_android
#elif defined(HALO_XBOX)
#define CONFIG_PLATFORM _platform_console
#elif defined(HALO_WIN9X)
#define CONFIG_PLATFORM (_platform_desktop | _platform_win9x)
#else
#define CONFIG_PLATFORM _platform_desktop
#endif

struct config_value
{
	int boolean;
	long integer;
	double real;
	char *string;
};

static struct config_value config_values[NUMBER_OF_CONFIG_SETTINGS];
static int config_loaded = 0;
#ifdef HALO_XBOX
/* a spin lock: the settings are read once, early, and the console's
threads are few */
static volatile long config_lock;
#define pthread_mutex_lock(lock) do { while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {} } while (0)
#define pthread_mutex_unlock(lock) __atomic_store_n(lock, 0, __ATOMIC_RELEASE)
#else
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

/* ---------- the file */

static void config_path(char *path, size_t size)
{
#ifdef HALO_ANDROID
	/* the data folder, which the app names (port/android/host/host_main.c) */
	const char *root = getenv("HALO_DATA_ROOT");

	snprintf(path, size, "%s/config.toml", root && *root ? root : ".");
#elif defined(HALO_XBOX)
	/* beside the game's data: the disc, or the folder the XBE runs from */
	snprintf(path, size, "D:\\config.toml");
#elif defined(HALO_WIN9X)
	/* the executable's folder (a shortcut need not start the game in it) */
	char directory[1024];

	if (w9x_executable_directory(directory, (int)sizeof(directory)))
		snprintf(path, size, "%s\\config.toml", directory);
	else
		snprintf(path, size, "config.toml");
#else
	/* the executable's folder, with its separator */
	const char *base = SDL_GetBasePath();

	snprintf(path, size, "%sconfig.toml", base ? base : "");
#endif
}

/* the whole file, NUL terminated, or NULL; free() it */
static char *config_read_file(const char *path, size_t *size)
{
#if defined(HALO_ANDROID) || defined(HALO_XBOX) || defined(HALO_WIN9X)
	FILE *file = fopen(path, "rb");
	char *text = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		text = malloc((size_t)length + 1);
		if (text && fread(text, 1, (size_t)length, file) == (size_t)length)
		{
			text[length] = 0;
			*size = (size_t)length;
		}
		else
		{
			free(text);
			text = NULL;
		}
	}
	fclose(file);
	return text;
#else
	/* SDL's, for UTF-8 paths on Windows */
	void *data = SDL_LoadFile(path, size);
	char *text;

	if (!data)
		return NULL;
	text = malloc(*size + 1);
	if (text)
	{
		memcpy(text, data, *size);
		text[*size] = 0;
	}
	SDL_free(data);
	return text;
#endif
}

static int config_write_file(const char *path, const char *text)
{
#if defined(HALO_ANDROID) || defined(HALO_XBOX) || defined(HALO_WIN9X)
	FILE *file = fopen(path, "wb");
	int written;

	if (!file)
		return 0;
#ifdef HALO_WIN9X
	/* Windows 98's Notepad shows a file without carriage returns as one
	line: lines that end in a bare newline get one */
	{
		const char *line = text, *end;

		written = 1;
		while (written && *line)
		{
			end = strchr(line, '\n');
			if (!end)
				end = line + strlen(line);
			written = fwrite(line, 1, (size_t)(end - line), file) == (size_t)(end - line);
			if (written && *end == '\n')
			{
				if (end == line || end[-1] != '\r')
					written = fputc('\r', file) != EOF;
				if (written)
					written = fputc('\n', file) != EOF;
				end++;
			}
			line = end;
		}
	}
#else
	written = fwrite(text, 1, strlen(text), file) == strlen(text);
#endif
	return fclose(file) == 0 && written;
#else
	return SDL_SaveFile(path, text, strlen(text));
#endif
}

struct config_text
{
	char *buffer;
	size_t length, capacity;
};

static void config_append(struct config_text *text, const char *string)
{
	size_t length = strlen(string);

	if (text->length + length + 1 > text->capacity)
	{
		size_t capacity = (text->capacity ? text->capacity : 4096) * 2 + length;
		char *buffer = realloc(text->buffer, capacity);

		if (!buffer)
			return;
		text->buffer = buffer;
		text->capacity = capacity;
	}
	memcpy(text->buffer + text->length, string, length + 1);
	text->length += length;
}

/* the first length characters of text, as a string of their own */
static char *config_copy(const char *text, size_t length)
{
	char *copy = malloc(length + 1);

	if (copy)
	{
		memcpy(copy, text, length);
		copy[length] = 0;
	}
	return copy;
}

/* one setting as the file holds it: its comment, and its key at the
default */
static void config_append_setting(struct config_text *text, const struct config_setting *setting)
{
	const char *dot = strchr(setting->name, '.');
	const char *line;
	char buffer[256];

	config_append(text, "\n");
	for (line = setting->comment; *line;)
	{
		size_t length = strcspn(line, "\n");

		snprintf(buffer, sizeof(buffer), "# %.*s\n", (int)length, line);
		config_append(text, buffer);
		line += length;
		if (*line)
			line++;
	}
#if !defined(HALO_ANDROID) && !defined(HALO_XBOX)
	/* (Android apps and the console have no environment to set) */
	switch (setting->environment_style)
	{
	case _environment_value:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=<value>)\n", setting->environment);
		break;
	case _environment_set_is_true:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=1 makes it true)\n", setting->environment);
		break;
	case _environment_set_is_false:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=1 makes it false)\n", setting->environment);
		break;
	}
	config_append(text, buffer);
#endif
	snprintf(buffer, sizeof(buffer), "%s = %s\n", dot + 1, setting->default_value);
	config_append(text, buffer);
}

/* the file with every setting of this build at its default */
static char *config_default_text(void)
{
	struct config_text text = { NULL, 0, 0 };
	char section[32] = "";
	size_t index;

#if defined(HALO_ANDROID) || defined(HALO_XBOX)
	config_append(&text,
		"# Halo settings\n"
		"#\n"
		"# The game writes this file with the defaults when it is missing: delete\n"
		"# it to go back to them.\n");
#else
	config_append(&text,
		"# Halo settings\n"
		"#\n"
		"# The game writes this file with the defaults when it is missing: delete\n"
		"# it to go back to them. Each setting can also be set for one run with\n"
		"# the environment variable named with it, which wins over this file.\n");
#endif
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *dot = strchr(setting->name, '.');
		char buffer[64];

		if (!(setting->platforms & CONFIG_PLATFORM) || !dot)
			continue;
		if (strncmp(section, setting->name, (size_t)(dot - setting->name)) ||
			section[dot - setting->name] != 0)
		{
			snprintf(section, sizeof(section), "%.*s", (int)(dot - setting->name), setting->name);
			snprintf(buffer, sizeof(buffer), "\n[%s]\n", section);
			config_append(&text, buffer);
		}
		config_append_setting(&text, setting);
	}
	return text.buffer;
}

/* the settings of this build that text (the file, parsed as table) lacks,
added to it in their sections, keeping the rest as it is: a newer version's
settings appear in an older file. Returns the new text, or NULL if nothing
was missing */
static char *config_add_missing(const char *text, toml_datum_t table)
{
	char *result = NULL;
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *dot = strchr(setting->name, '.');
		const char *current = result ? result : text;
		struct config_text block = { NULL, 0, 0 };
		struct config_text updated = { NULL, 0, 0 };
		char header[40];
		const char *line;
		const char *insert = NULL;

		if (!(setting->platforms & CONFIG_PLATFORM) || !dot || toml_seek(table, setting->name).type != TOML_UNKNOWN)
			continue;
		snprintf(header, sizeof(header), "[%.*s]", (int)(dot - setting->name), setting->name);
		/* the end of the section's last line that is not blank */
		for (line = current; *line; )
		{
			const char *start = line;
			size_t length = strcspn(line, "\n");

			while (*start == ' ' || *start == '\t')
				start++;
			if (insert && *start == '[')
				break;
			if (!insert && !strncmp(start, header, strlen(header)))
				insert = line + length;
			else if (insert && start < line + length && *start != '\r')
				insert = line + length;
			line += length;
			if (*line)
				line++;
		}
		if (insert)
		{
			if (*insert)
				insert++;
			config_append_setting(&block, setting);
		}
		else
		{
			/* no such section: a new one at the end */
			insert = current + strlen(current);
			config_append(&block, current[0] && insert[-1] != '\n' ? "\n\n" : "\n");
			config_append(&block, header);
			config_append(&block, "\n");
			config_append_setting(&block, setting);
		}
		if (!block.buffer)
			continue;
		{
			char *before = config_copy(current, (size_t)(insert - current));

			if (before)
				config_append(&updated, before);
			free(before);
		}
		if (insert > current && insert[-1] != '\n')
			config_append(&updated, "\n");
		config_append(&updated, block.buffer);
		config_append(&updated, insert);
		free(block.buffer);
		if (updated.buffer)
		{
			free(result);
			result = updated.buffer;
			platform_log("settings: added %s (new in this version) at its default", setting->name);
		}
	}
	return result;
}

/* ---------- values */

static int config_text_is_false(const char *text)
{
	char lower[8];
	size_t index;

	for (index = 0; index + 1 < sizeof(lower) && text[index]; index++)
		lower[index] = (char)tolower((unsigned char)text[index]);
	lower[index] = 0;
	return !strcmp(lower, "0") || !strcmp(lower, "false") || !strcmp(lower, "no") || !strcmp(lower, "off");
}

static void config_set_from_text(struct config_value *value, enum config_type type, const char *text)
{
	switch (type)
	{
	case _config_boolean:
		value->boolean = !config_text_is_false(text);
		break;
	case _config_integer:
		value->integer = strtol(text, NULL, 10);
		break;
	case _config_real:
		value->real = strtod(text, NULL);
		break;
	case _config_string:
		free(value->string);
		value->string = strdup(text);
		break;
	}
}

/* the value in the file, if it is there and of the setting's type */
static void config_set_from_file(struct config_value *value, const struct config_setting *setting,
	toml_datum_t table)
{
	toml_datum_t datum = toml_seek(table, setting->name);
	int wrong_type = 0;

	if (datum.type == TOML_UNKNOWN)
		return;
	switch (setting->type)
	{
	case _config_boolean:
		if (datum.type == TOML_BOOLEAN)
			value->boolean = datum.u.boolean;
		else
			wrong_type = 1;
		break;
	case _config_integer:
		if (datum.type == TOML_INT64)
			value->integer = (long)datum.u.int64;
		else
			wrong_type = 1;
		break;
	case _config_real:
		if (datum.type == TOML_FP64)
			value->real = datum.u.fp64;
		else if (datum.type == TOML_INT64)
			value->real = (double)datum.u.int64;
		else
			wrong_type = 1;
		break;
	case _config_string:
		if (datum.type == TOML_STRING)
		{
			free(value->string);
			value->string = strdup(datum.u.s);
		}
		else
		{
			wrong_type = 1;
		}
		break;
	}
	if (wrong_type)
	{
		static const char *const expected[] = { "true or false", "a whole number", "a number", "a quoted string" };

		platform_log("config.toml line %d: %s should be %s; using %s", datum.lineno, setting->name,
			expected[setting->type], setting->default_value);
	}
}

static long config_setting_index(const char *name)
{
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		if (!strcmp(config_settings[index].name, name))
			return (long)index;
	}
	return -1;
}

/* keys in the file that are no setting, likely misspelt */
static void config_report_unknown_keys(toml_datum_t table)
{
	int section_index;

	for (section_index = 0; section_index < table.u.tab.size; section_index++)
	{
		toml_datum_t section = table.u.tab.value[section_index];
		int key_index;

		if (section.type != TOML_TABLE)
		{
			platform_log("config.toml line %d: unknown setting %s", section.lineno, table.u.tab.key[section_index]);
			continue;
		}
		for (key_index = 0; key_index < section.u.tab.size; key_index++)
		{
			char name[128];

			snprintf(name, sizeof(name), "%s.%s", table.u.tab.key[section_index], section.u.tab.key[key_index]);
			if (config_setting_index(name) < 0)
				platform_log("config.toml line %d: unknown setting %s", section.u.tab.value[key_index].lineno, name);
		}
	}
}

static void config_load(void)
{
	char path[1024];
	size_t size = 0;
	char *text;
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const char *default_value = config_settings[index].default_value;

		if (config_settings[index].type == _config_string)
		{
			/* written as a TOML basic string without escapes */
			size_t length = strlen(default_value);

			config_values[index].string = length >= 2 ? config_copy(default_value + 1, length - 2) : strdup("");
		}
		else
		{
			config_set_from_text(&config_values[index], config_settings[index].type, default_value);
		}
	}

	config_path(path, sizeof(path));
	text = config_read_file(path, &size);
	if (text)
	{
		toml_result_t result = toml_parse(text, (int)size);

		if (result.ok)
		{
			char *completed;

			for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
				config_set_from_file(&config_values[index], &config_settings[index], result.toptab);
			config_report_unknown_keys(result.toptab);
			platform_log("settings: %s", path);
			completed = config_add_missing(text, result.toptab);
			if (completed && !config_write_file(path, completed))
				platform_log("settings: cannot write %s", path);
			free(completed);
		}
		else
		{
			platform_log("config.toml: %s; using the defaults", result.errmsg);
		}
		toml_free(result);
		free(text);
	}
	else
	{
#ifdef HALO_XBOX
		/* the disc is read-only: nothing to write */
		platform_log("settings: no %s; using the defaults", path);
#else
		char *defaults = config_default_text();

		if (defaults && config_write_file(path, defaults))
			platform_log("settings: wrote the defaults to %s", path);
		else
			platform_log("settings: cannot write %s; using the defaults", path);
		free(defaults);
#endif
	}

#ifndef HALO_XBOX
	/* the environment; the console has none */
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *environment = getenv(setting->environment);

		if (!environment)
			continue;
		switch (setting->environment_style)
		{
		case _environment_value:
			config_set_from_text(&config_values[index], setting->type, environment);
			break;
		case _environment_set_is_true:
			config_values[index].boolean = 1;
			break;
		case _environment_set_is_false:
			config_values[index].boolean = 0;
			break;
		}
	}
#endif
}

#ifdef CONFIG_LAUNCHER
static void config_launcher(void);

/* the Windows builds' launcher, after the file and before the first value
is handed out: readers that keep their first answer see its changes */
static void config_launcher_once(void)
{
	static int launcher_done;

	/* (set first: the launcher reads and sets settings itself) */
	if (!launcher_done)
	{
		launcher_done = 1;
		config_launcher();
	}
}
#endif

static const struct config_value *config_value(const char *name, enum config_type type)
{
	static const struct config_value none = { 0, 0, 0.0, "" };
	long index;

	pthread_mutex_lock(&config_lock);
	if (!config_loaded)
	{
		config_load();
		config_loaded = 1;
	}
	pthread_mutex_unlock(&config_lock);
#ifdef CONFIG_LAUNCHER
	config_launcher_once();
#endif
	index = config_setting_index(name);
	if (index < 0 || config_settings[index].type != type)
	{
		platform_log("settings: no %s setting %s", type == _config_string ? "string" : "such", name);
		return &none;
	}
	return &config_values[index];
}

#ifndef HALO_XBOX
/* the same, for the table's own readers (config_setting_text and the rest) */
static void config_ensure_loaded(void)
{
	pthread_mutex_lock(&config_lock);
	if (!config_loaded)
	{
		config_load();
		config_loaded = 1;
	}
	pthread_mutex_unlock(&config_lock);
#ifdef CONFIG_LAUNCHER
	config_launcher_once();
#endif
}
#endif

/* ---------- public code */

int config_boolean(const char *name)
{
	return config_value(name, _config_boolean)->boolean;
}

long config_integer(const char *name)
{
	return config_value(name, _config_integer)->integer;
}

double config_real(const char *name)
{
	return config_value(name, _config_real)->real;
}

const char *config_string(const char *name)
{
	const char *string = config_value(name, _config_string)->string;

	return string ? string : "";
}

/* display.field_of_view as a factor on the tangent of the view's half
angle: 1 at the game's 70 degrees (main.c, set_window_camera_values, scales
the observer's view by it, zoomed or not, which keeps a zoom's magnification) */
float halo_field_of_view_tangent_scale(void)
{
	static float scale;
	static int computed;

	if (!computed)
	{
		double degrees = config_real("display.field_of_view");
		const double half_radians_per_degree = 3.14159265358979323846 / 360.0;

		if (!(degrees >= 40.0))
			degrees = degrees > 0.0 ? 40.0 : 70.0;
		if (degrees > 150.0)
			degrees = 150.0;
		scale = (float)(tan(degrees * half_radians_per_degree) / tan(70.0 * half_radians_per_degree));
		if (degrees != 70.0)
			platform_log("field of view %.1f degrees (x%.3f)", degrees, scale);
		computed = 1;
	}
	return scale;
}

/* debug.console: the developer console and its lines on the screen
(console.c, interface.c); read once, it is asked every frame */
int halo_debug_console_enabled(void)
{
	static int checked, enabled;

	if (!checked)
	{
		enabled = config_boolean("debug.console");
		checked = 1;
	}
	return enabled;
}

/* debug.log_file: whether the game writes debug.txt (errors.c) */
int halo_debug_log_file_enabled(void)
{
	static int checked, enabled;

	if (!checked)
	{
		enabled = config_boolean("debug.log_file");
		checked = 1;
	}
	return enabled;
}

/* display.fps_counter: the frame rate in the top left (main.c,
main_framerate_render); read once, it is asked every frame */
int halo_fps_counter_enabled(void)
{
	static int checked, enabled;

	if (!checked)
	{
		enabled = config_boolean("display.fps_counter");
		checked = 1;
	}
	return enabled;
}

/* a [graphics] setting (graphics.<name>): whether that effect may be drawn */
int halo_graphics_option(const char *name)
{
	char key[64];

	snprintf(key, sizeof(key), "graphics.%s", name);
	return config_boolean(key);
}

#ifndef HALO_XBOX
/* graphics.light_pipeline: 1 on, 0 off, -1 for "auto", which leaves it to
the renderer (rasterizer.c, rasterizer_light_pipeline) */
int halo_light_pipeline_setting(void)
{
	const char *text = config_string("graphics.light_pipeline");
	char lower[8];
	size_t index;

	for (index = 0; index + 1 < sizeof(lower) && text[index]; index++)
		lower[index] = (char)tolower((unsigned char)text[index]);
	lower[index] = 0;
	if (!lower[0] || !strcmp(lower, "auto"))
		return -1;
	return !config_text_is_false(lower);
}

/* ---------- the table itself (port_config.h; the launcher, launcher.h) */

int config_setting_count(void)
{
	return (int)NUMBER_OF_CONFIG_SETTINGS;
}

/* a string's default without its quotes (written as a TOML basic string
without escapes, config_load) */
static void config_setting_default_text(const struct config_setting *setting, char *text, size_t size)
{
	const char *default_value = setting->default_value;
	size_t length = strlen(default_value);

	if (setting->type == _config_string && length >= 2)
		snprintf(text, size, "%.*s", (int)(length - 2), default_value + 1);
	else
		snprintf(text, size, "%s", setting->type == _config_string ? "" : default_value);
}

int config_setting_info(int index, struct config_setting_info *info)
{
	const struct config_setting *setting;

	if (index < 0 || index >= (int)NUMBER_OF_CONFIG_SETTINGS)
		return 0;
	setting = &config_settings[index];
	info->name = setting->name;
	info->type = (int)setting->type;
	info->comment = setting->comment;
	info->read_here = (setting->platforms & CONFIG_PLATFORM) != 0;
	config_setting_default_text(setting, info->default_text, sizeof(info->default_text));
	return 1;
}

/* a real as the file spells one: with a point or an exponent, so that TOML
reads a float back (70.0, not 70) */
static void config_real_text(double real, char *text, size_t size)
{
	snprintf(text, size, "%.15g", real);
	if (!strpbrk(text, ".eEni") && strlen(text) + 2 < size)
		strcat(text, ".0");
}

static void config_value_text(const struct config_value *value, enum config_type type, char *text, size_t size)
{
	switch (type)
	{
	case _config_boolean:
		snprintf(text, size, "%s", value->boolean ? "true" : "false");
		break;
	case _config_integer:
		snprintf(text, size, "%ld", value->integer);
		break;
	case _config_real:
		config_real_text(value->real, text, size);
		break;
	case _config_string:
		snprintf(text, size, "%s", value->string ? value->string : "");
		break;
	}
}

int config_setting_text(int index, char *text, int size)
{
	if (index < 0 || index >= (int)NUMBER_OF_CONFIG_SETTINGS || size <= 0)
		return 0;
	config_ensure_loaded();
	config_value_text(&config_values[index], config_settings[index].type, text, (size_t)size);
	return 1;
}

int config_setting_set_text(int index, const char *text)
{
	if (index < 0 || index >= (int)NUMBER_OF_CONFIG_SETTINGS)
		return 0;
	config_ensure_loaded();
	config_set_from_text(&config_values[index], config_settings[index].type, text);
	return 1;
}

/* ---------- writing the values back (config_save)

The file as it is, each setting's line given the current value: the
comments, the order, and the keys of other builds and other versions stay
as they are. */

/* a string as a TOML basic string */
static void config_quote_string(const char *string, char *text, size_t size)
{
	size_t used = 0;

	if (size < 3)
	{
		if (size)
			text[0] = 0;
		return;
	}
	text[used++] = '"';
	for (; *string; string++)
	{
		unsigned char character = (unsigned char)*string;
		char escape[8];

		switch (character)
		{
		case '"': strcpy(escape, "\\\""); break;
		case '\\': strcpy(escape, "\\\\"); break;
		case '\n': strcpy(escape, "\\n"); break;
		case '\r': strcpy(escape, "\\r"); break;
		case '\t': strcpy(escape, "\\t"); break;
		case '\b': strcpy(escape, "\\b"); break;
		case '\f': strcpy(escape, "\\f"); break;
		default:
			if (character < 0x20 || character == 0x7f)
				snprintf(escape, sizeof(escape), "\\u%04X", character);
			else
				snprintf(escape, sizeof(escape), "%c", character);
			break;
		}
		if (used + strlen(escape) + 2 > size)
			break;
		strcpy(text + used, escape);
		used += strlen(escape);
	}
	text[used++] = '"';
	text[used] = 0;
}

/* the value of "key = value" in the section [section] of text, as the
span *start to *end of the text; FALSE when there is no such line (the
key quoted or dotted at the top level, or a multi-line string, which the
game never writes and which are left alone) */
static int config_find_value(const char *text, const char *section, const char *key, size_t *start, size_t *end)
{
	size_t section_length = strlen(section), key_length = strlen(key);
	int in_section = 0;
	const char *line;

	for (line = text; *line; )
	{
		size_t length = strcspn(line, "\n");
		const char *stop = line + length;
		const char *cursor = line;

		while (cursor < stop && (*cursor == ' ' || *cursor == '\t'))
			cursor++;
		if (cursor < stop && *cursor == '[')
		{
			const char *name = cursor + 1;

			while (name < stop && (*name == ' ' || *name == '\t'))
				name++;
			in_section = 0;
			if ((size_t)(stop - name) > section_length && !strncmp(name, section, section_length))
			{
				const char *close = name + section_length;

				while (close < stop && (*close == ' ' || *close == '\t'))
					close++;
				in_section = close < stop && *close == ']';
			}
		}
		else if (in_section && (size_t)(stop - cursor) > key_length && !strncmp(cursor, key, key_length))
		{
			const char *after = cursor + key_length;

			while (after < stop && (*after == ' ' || *after == '\t'))
				after++;
			if (after < stop && *after == '=')
			{
				const char *scan;

				after++;
				while (after < stop && (*after == ' ' || *after == '\t'))
					after++;
				*start = (size_t)(after - text);
				if (after < stop && (*after == '"' || *after == '\''))
				{
					char quote = *after;

					if (stop - after >= 3 && after[1] == quote && after[2] == quote)
						return 0;
					for (scan = after + 1; scan < stop && *scan != quote; scan++)
					{
						if (quote == '"' && *scan == '\\')
							scan++;
					}
					if (scan >= stop)
						return 0;
					*end = (size_t)(scan + 1 - text);
				}
				else
				{
					scan = after;
					while (scan < stop && *scan != '#' && *scan != '\r')
						scan++;
					while (scan > after && (scan[-1] == ' ' || scan[-1] == '\t'))
						scan--;
					*end = (size_t)(scan - text);
				}
				return 1;
			}
		}
		line += length;
		if (*line)
			line++;
	}
	return 0;
}

/* text with its span start to end replaced by value; NULL without memory */
static char *config_replace(const char *text, size_t start, size_t end, const char *value)
{
	size_t length = strlen(text), value_length = strlen(value);
	char *result = malloc(length - (end - start) + value_length + 1);

	if (!result)
		return NULL;
	memcpy(result, text, start);
	memcpy(result + start, value, value_length);
	memcpy(result + start + value_length, text + end, length - end + 1);
	return result;
}

int config_save(void)
{
	char path[1024];
	size_t size = 0;
	char *text, *completed;
	toml_result_t parsed;
	int changed = 0, written = 1;
	size_t index;

	config_ensure_loaded();
	config_path(path, sizeof(path));
	text = config_read_file(path, &size);
	if (text)
	{
		parsed = toml_parse(text, (int)size);
		if (!parsed.ok)
		{
			platform_log("config.toml: %s; writing it anew", parsed.errmsg);
			toml_free(parsed);
			free(text);
			text = NULL;
		}
	}
	if (!text)
	{
		text = config_default_text();
		if (!text)
			return 0;
		parsed = toml_parse(text, (int)strlen(text));
		changed = 1;
	}
	completed = parsed.ok ? config_add_missing(text, parsed.toptab) : NULL;
	toml_free(parsed);
	if (completed)
	{
		free(text);
		text = completed;
		changed = 1;
	}
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *dot = strchr(setting->name, '.');
		char section[32], value[1200], current[1200];
		size_t start, end;
		char *replaced;

		if (!(setting->platforms & CONFIG_PLATFORM) || !dot)
			continue;
		snprintf(section, sizeof(section), "%.*s", (int)(dot - setting->name), setting->name);
		if (setting->type == _config_string)
			config_quote_string(config_values[index].string ? config_values[index].string : "", value, sizeof(value));
		else
			config_value_text(&config_values[index], setting->type, value, sizeof(value));
		if (!config_find_value(text, section, dot + 1, &start, &end))
		{
			platform_log("settings: no line for %s in %s: not written", setting->name, path);
			continue;
		}
		snprintf(current, sizeof(current), "%.*s", (int)(end - start), text + start);
		if (!strcmp(current, value))
			continue;
		replaced = config_replace(text, start, end, value);
		if (!replaced)
			continue;
		free(text);
		text = replaced;
		changed = 1;
	}
	if (changed)
	{
		written = config_write_file(path, text);
		if (written)
			platform_log("settings: wrote %s", path);
		else
			platform_log("settings: cannot write %s", path);
	}
	free(text);
	return written;
}
#endif

#ifdef CONFIG_LAUNCHER
/* ---------- the launcher (launcher.h)

Shown once the file is read and before the first setting is handed out
(config_ensure_loaded): with launcher.show, unless the command line says
-nolauncher or carries an invite link (which a running copy takes, p2p.c);
-launcher shows it whatever they say. Play puts the chosen values in place
for this run and writes the file; Quit ends the program. */

#define CONFIG_LAUNCHER_VALUE_SIZE 1024

static void config_launcher(void)
{
	struct launcher_setting *settings;
	char **originals, **defaults;
	long *indices;
	char argument[64];
	int show, forced = 0, link = 0, count = 0, changed = 0, index;
	long show_index = config_setting_index("launcher.show");

	if (show_index < 0)
		return;
	show = config_values[show_index].boolean;
	for (index = 1; posix_command_line_argument(index, argument, sizeof(argument)); index++)
	{
		if (!strcmp(argument, "-launcher") || !strcmp(argument, "/launcher"))
		{
			show = 1;
			forced = 1;
		}
		else if (!strcmp(argument, "-nolauncher") || !strcmp(argument, "/nolauncher"))
		{
			show = 0;
			forced = 1;
		}
		else if (!strncmp(argument, "halo:", 5))
		{
			link = 1;
		}
	}
	if (!show || (link && !forced))
		return;
	settings = calloc(NUMBER_OF_CONFIG_SETTINGS, sizeof(*settings));
	originals = calloc(NUMBER_OF_CONFIG_SETTINGS, sizeof(*originals));
	defaults = calloc(NUMBER_OF_CONFIG_SETTINGS, sizeof(*defaults));
	indices = calloc(NUMBER_OF_CONFIG_SETTINGS, sizeof(*indices));
	if (!settings || !originals || !defaults || !indices)
	{
		free(settings);
		free(originals);
		free(defaults);
		free(indices);
		return;
	}
	for (index = 0; index < (int)NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		struct launcher_setting *shown = &settings[count];
		char default_text[128];

		if (!(setting->platforms & CONFIG_PLATFORM))
			continue;
		config_setting_default_text(setting, default_text, sizeof(default_text));
		defaults[count] = strdup(default_text);
		originals[count] = malloc(CONFIG_LAUNCHER_VALUE_SIZE);
		shown->value = malloc(CONFIG_LAUNCHER_VALUE_SIZE);
		if (!defaults[count] || !originals[count] || !shown->value)
		{
			/* (what there is so far is shown) */
			if (defaults[count])
				free(defaults[count]);
			if (originals[count])
				free(originals[count]);
			if (shown->value)
				free(shown->value);
			break;
		}
		shown->name = setting->name;
		shown->type = (enum launcher_setting_type)setting->type;
		shown->default_text = defaults[count];
		shown->comment = setting->comment;
		shown->value_size = CONFIG_LAUNCHER_VALUE_SIZE;
		config_value_text(&config_values[index], setting->type, shown->value, CONFIG_LAUNCHER_VALUE_SIZE);
		snprintf(originals[count], CONFIG_LAUNCHER_VALUE_SIZE, "%s", shown->value);
		indices[count] = index;
		count++;
	}
	if (!launcher_show(settings, count))
	{
		platform_log("launcher: quit");
		exit(EXIT_SUCCESS);
	}
	for (index = 0; index < count; index++)
	{
		if (strcmp(settings[index].value, originals[index]))
		{
			config_set_from_text(&config_values[indices[index]], config_settings[indices[index]].type,
				settings[index].value);
			platform_log("launcher: %s = %s", settings[index].name, settings[index].value);
			changed++;
		}
	}
	if (changed)
		config_save();
	for (index = 0; index < count; index++)
	{
		free(settings[index].value);
		free(originals[index]);
		free(defaults[index]);
	}
	free(settings);
	free(originals);
	free(defaults);
	free(indices);
}
#endif
