/*
NX_VIDEO.C

The display mode from the dashboard's video settings, as retail games
chose theirs: the console's EEPROM says whether the player enabled 480p,
720p and 1080i and whether the television is widescreen
(XVideoGetEncoderSettings; nxdk's names for the XC_VIDEO_FLAGS_* bits).

- 480p: with the HDTV pack and 480p enabled, the 640x480 mode is
  progressive instead of interlaced (nxdk's XVideoSetMode takes the first
  640x480 mode in its table, the interlaced one, so the progressive mode
  is set through its XVideoInit with the table's code for it).
- Widescreen: the game draws 854 columns (the desktop builds' widescreen:
  the view is 16:9, the menus centred, the HUD at the edges) which the
  device squeezes into a 720x480 buffer (d3d8_nv2a.c, scale_x), and the
  encoder is told the picture is 16:9 (AV_OPTION_WIDESCREEN, the WSS
  signal), so the television stretches it back.
- 720p (display.hd_720p in config.toml, nx_video_upgrade_hd): the game
  draws 854x480, as on a widescreen display, and the device scales it by
  1.5 into pbkit's 1280x720 buffers (d3d8_nv2a.c, scale_x and scale_y).
  Its frame buffers cost 9 MB more than 480's, which a 64 MB console does
  not have. Where the kernel gives contiguous memory above 64 MB (a
  development kernel), the top 64 MB of a 128 MB console holds the game's
  caches and the Direct3D resources (nx_physical_alloc). A retail kernel
  gives none there, even with 128 MB (the user's console, 2026-09-30), so
  720p fits the low 64 MB instead, when a trial says it does
  (low_memory_hd): two frame buffers instead of three and no pbkit depth
  buffer (tools/xbox_pbkit_patch.py), a 10 MB texture cache (9 on a 64 MB
  console), the sound cache in ordinary memory, which the kernel can take
  from the top 64 MB, and nxdk's text screen as the depth buffer
  (nx_gpu_depth_buffer): with that, a stock 64 MB console has as much left
  over at 720p as at 480. 1080i is not offered.

nx_video_configure runs at start-up (nx_startup), before the game asks how
wide its screen is; nx_video_upgrade_hd on that first question, when the
settings can be read and before pbkit starts (it sizes its buffers from the
mode); the widescreen option after pbkit starts.
*/

#include <hal/video.h>
#include <xboxkrnl/xboxkrnl.h>

#include <stdio.h>
#include <string.h>

#include "nx.h"

/* nxdk's, not in its header: sets the mode with this code from its table */
void XVideoInit(DWORD dwMode, int width, int height, int bpp);

enum
{
	/* AvSendTVEncoderOption's options (the XDK's av.h): the ones nxdk uses
	are 6 (the settings), 9 (blank), 11 (flicker filter), 14 (soften) */
	AV_OPTION_WIDESCREEN = 18,
	/* nxdk's video mode table: 640x480 and 720x480 progressive, NTSC, on
	the HDTV pack */
	VIDEO_MODE_640x480P_NTSCM = 0x88070701,
	VIDEO_MODE_720x480P_NTSCM = 0x88080801,
	/* and 1280x720 progressive (NTSC-M and NTSC-J alike) */
	VIDEO_MODE_1280x720P_NTSCM = 0x880B0A02,
	/* the columns the game draws on a widescreen display: 480 lines' worth
	of 16:9 */
	WIDESCREEN_LOGICAL_WIDTH = 854,
	WIDESCREEN_PHYSICAL_WIDTH = 720,
	VIDEO_REGION_NTSCM = 0x00000100,
	VIDEO_REGION_NTSCJ = 0x00000200,
};

static unsigned long settings;
static int widescreen;
static int progressive;
static unsigned long physical_width;
static int high_definition;
static int low_memory_hd;
static char notice[160];

/* a 64 MB console (stock): its ordinary memory is the same 64 MB */
static int console_has_64_mb(void)
{
	unsigned long total_bytes = 0, available_bytes = 0;

	nx_memory_status(&total_bytes, &available_bytes);
	return total_bytes <= 0x4000000UL;
}

/* the texture cache at 720p in the low 64 MB: 10 MB, 9 on a 64 MB console,
where its sound cache takes 4 MB of the same memory (xbox_display.c) */
unsigned long nx_video_hd_texture_cache_bytes(void)
{
	return console_has_64_mb() ? 0x900000UL : 0xA00000UL;
}

/* what 720p needs of the low 64 MB beyond the game's fixed blocks (taken
at start-up), without the top 64 MB: pbkit's two 1280x720 frame buffers,
nxdk's text screen at 1280x720 (also the depth buffer, nx_gpu_depth_buffer;
XVideoSetMode frees the 480 one first), the texture cache, the push buffer,
the sound cache where it comes from the same memory, and room for what the
game allocates as it runs: the Direct3D resources, the USB driver's pool,
the heap. At 480 a 64 MB console has 6.4 MB left after start-up
(2026-10-01, the emulator at 64 MB) and plays, and with this 720p leaves
about as much. The room is asked for in 1 MB pieces, as it is used, not
in one contiguous block. */
static int low_memory_fits_hd(void)
{
	enum { MB = 1024 * 1024, ROOM_MB = 6, SOUND_MB = 4 };
	unsigned long sizes[16];
	unsigned long screen_now = XVideoGetFB() ? MmQueryAllocationSize(XVideoGetFB()) : 0;
	int count = 0, index;

	sizes[count++] = 2 * 5120 * 720;  /* pbkit's frame buffers (pitch 5120) */
	sizes[count++] = 5120 * 720 > screen_now ? 5120 * 720 - screen_now : 0;
	sizes[count++] = nx_video_hd_texture_cache_bytes();
	sizes[count++] = 1400 * 1024;     /* the push buffer */
	for (index = 0; index < ROOM_MB; index++)
		sizes[count++] = MB;
	if (console_has_64_mb())
	{
		for (index = 0; index < SOUND_MB; index++)
			sizes[count++] = MB;
	}
	return nx_low_memory_room(sizes, count);
}

unsigned long nx_video_flags(void)
{
	return XVideoGetEncoderSettings();
}

void nx_video_configure(void)
{
	static int configured;
	unsigned long pack, standard;
	char text[160];

	if (configured)
		return;
	configured = 1;
	settings = XVideoGetEncoderSettings();
	pack = settings & VIDEO_ADAPTER_MASK;
	standard = settings & VIDEO_STANDARD_MASK;
	widescreen = (settings & VIDEO_WIDESCREEN) != 0;
	progressive = 0;
	physical_width = 640;
	/* the wider buffer, if the mode table has it for this pack (it sets
	the mode nxdk's XVideoGetMode reports, which pbkit sizes its buffers
	from) */
	if (widescreen && XVideoSetMode(WIDESCREEN_PHYSICAL_WIDTH, 480, 32, REFRESH_DEFAULT))
		physical_width = WIDESCREEN_PHYSICAL_WIDTH;
	if ((settings & VIDEO_MODE_480P) && pack == AV_PACK_HDTV &&
		(standard == VIDEO_REGION_NTSCM || standard == VIDEO_REGION_NTSCJ))
	{
		XVideoInit(physical_width == WIDESCREEN_PHYSICAL_WIDTH ? VIDEO_MODE_720x480P_NTSCM : VIDEO_MODE_640x480P_NTSCM,
			physical_width, 480, 32);
		progressive = 1;
	}
	snprintf(text, sizeof(text), "halo-xbox: video: dashboard flags %08lx (pack %lu, standard %lx): %lux480 %s%s\n",
		settings, pack, standard >> 8, physical_width, progressive ? "progressive" : "interlaced",
		widescreen ? (physical_width == WIDESCREEN_PHYSICAL_WIDTH ? ", widescreen (854 columns squeezed in)" :
		", widescreen (no 720-column mode: 640 stretched)") : "");
	nx_print(text);
}

/* 720p, if the setting asks for it and everything it needs is there: the
dashboard's 720p (the HDTV pack, NTSC), and the top 64 MB of a 128 MB
console for the room */
int nx_video_upgrade_hd(int wanted)
{
	static int asked;
	unsigned long pack, standard;
	const char *refusal = NULL;
	char text[192];

	if (asked || !wanted)
		return high_definition;
	asked = 1;
	nx_video_configure();
	pack = settings & VIDEO_ADAPTER_MASK;
	standard = settings & VIDEO_STANDARD_MASK;
	if (nx_gpu_started())
		refusal = "the GPU had already started";
	else if (pack != AV_PACK_HDTV)
		refusal = "it needs the HDTV (component) cable";
	else if (!(settings & VIDEO_MODE_720P))
		refusal = "720p is off in the dashboard's video settings";
	else if (standard != VIDEO_REGION_NTSCM && standard != VIDEO_REGION_NTSCJ)
		refusal = "the console is not NTSC";
	else if (!nx_high_memory_available() && !low_memory_fits_hd())
		refusal = "not enough memory for its frame buffers below 64 MB";
	if (refusal)
	{
		unsigned long total_bytes = 0, available_bytes = 0;

		snprintf(text, sizeof(text), "halo-xbox: video: display.hd_720p is on, but %s: staying at %lux480\n",
			refusal, nx_video_physical_width());
		nx_print(text);
		/* and on the screen, under the frame rate (the console's log may
		not reach a computer): the reason, the dashboard's flags and the
		memory the kernel reports */
		nx_memory_status(&total_bytes, &available_bytes);
		snprintf(notice, sizeof(notice), "480: no 720p, %s (flags %08lx, %lu MB)", refusal, settings,
			(total_bytes + 0x80000UL) >> 20);
		return 0;
	}
	/* through the mode table, not XVideoInit: only XVideoSetMode records
	the mode XVideoGetMode reports, which pbkit sizes its frame buffers
	from (with XVideoInit alone the encoder sent 720p from buffers laid out
	for 720x480: a psychedelic picture) */
	if (!XVideoSetMode(1280, 720, 32, REFRESH_DEFAULT))
	{
		/* (a refusal clears the recorded mode: the 480 one again) */
		XVideoSetMode(physical_width, 480, 32, REFRESH_DEFAULT);
		if (progressive)
			XVideoInit(physical_width == WIDESCREEN_PHYSICAL_WIDTH ? VIDEO_MODE_720x480P_NTSCM : VIDEO_MODE_640x480P_NTSCM,
				physical_width, 480, 32);
		nx_print("halo-xbox: video: the mode table has no 720p for this console: staying at 480\n");
		snprintf(notice, sizeof(notice), "480: no 720p, the video mode table refused it (flags %08lx)", settings);
		return 0;
	}
	high_definition = 1;
	physical_width = 1280;
	progressive = 1;
	if (nx_high_memory_available())
	{
		/* the game's own allocations go to the top 64 MB from now on */
		nx_high_memory_placement(1);
		snprintf(notice, sizeof(notice), "720p");
	}
	else
	{
		low_memory_hd = 1;
		snprintf(notice, sizeof(notice), "720p (in 64 MB: double buffered, %lu MB textures)",
			nx_video_hd_texture_cache_bytes() >> 20);
	}
	snprintf(text, sizeof(text), "halo-xbox: video: 1280x720 progressive, 16:9 (854x480 drawn at 1280x720); %s\n",
		low_memory_hd ? "in the low 64 MB, double buffered, nxdk's text screen the depth buffer" : "the game's memory in the top 64 MB");
	nx_print(text);
	return 1;
}

int nx_video_low_memory_hd(void)
{
	return low_memory_hd;
}

/* more for the screen's line (where the sound cache landed) */
void nx_video_append_notice(const char *text)
{
	size_t length = strlen(notice);

	snprintf(notice + length, sizeof(notice) - length, "%s", text);
}

/* the display mode's line for the screen when 720p was asked for (under
the frame rate): "720p", or why not; empty otherwise */
const char *nx_video_notice(void)
{
	return notice;
}

unsigned long nx_video_physical_width(void)
{
	return physical_width ? physical_width : 640;
}

unsigned long nx_video_physical_height(void)
{
	return high_definition ? 720 : 480;
}

unsigned long nx_video_logical_width(void)
{
	if (high_definition)
		return WIDESCREEN_LOGICAL_WIDTH;
	return widescreen && physical_width == WIDESCREEN_PHYSICAL_WIDTH ? WIDESCREEN_LOGICAL_WIDTH : 640;
}

void nx_video_apply_widescreen(void)
{
	/* (720p is 16:9 by definition: the signal is for 480-line modes) */
	if (widescreen && !high_definition)
		AvSendTVEncoderOption((PVOID)VIDEO_BASE, AV_OPTION_WIDESCREEN, TRUE, NULL);
}

int nx_video_widescreen(void)
{
	return widescreen;
}
