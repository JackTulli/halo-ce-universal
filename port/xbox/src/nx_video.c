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
- 720p and 1080i are not offered: a 1280x720 frame costs 7 MB more than
  the console has left with the game's caches (port/xbox/README.md).

Called from nx_gpu_start: the mode before pbkit starts (it sizes its
buffers from the mode), the widescreen option after.
*/

#include <hal/video.h>
#include <xboxkrnl/xboxkrnl.h>

#include <stdio.h>

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

unsigned long nx_video_physical_width(void)
{
	return physical_width ? physical_width : 640;
}

unsigned long nx_video_logical_width(void)
{
	return widescreen && physical_width == WIDESCREEN_PHYSICAL_WIDTH ? WIDESCREEN_LOGICAL_WIDTH : 640;
}

void nx_video_apply_widescreen(void)
{
	if (widescreen)
		AvSendTVEncoderOption((PVOID)VIDEO_BASE, AV_OPTION_WIDESCREEN, TRUE, NULL);
}

int nx_video_widescreen(void)
{
	return widescreen;
}
