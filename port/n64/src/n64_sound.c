/*
N64_SOUND.C

Sound on the N64: libdragon's audio and mixer at 22 kHz. So far only the
main menu's music (Halo's title1 looping sound, tools/n64/music.py): its
start ("in", the monks) once, then one of its three loop versions after
another, chosen at random as Halo's looping sounds do, each streamed from
the cartridge (rom:/music/title_in.wav64, title_loop0..2.wav64).

The mixer is fed at the end of every frame (n64_sound_poll, from
n64_render_frame_end), in the menu and in games alike, so the audio never
runs dry; with nothing playing it plays silence.
*/

#include <libdragon.h>
#include <stdio.h>

#include "n64.h"

#define SOUND_RATE 22050
#define MUSIC_CHANNEL 0
#define LOOP_VERSIONS 3

static struct
{
	int ready;
	/* the music: playing at all, past its start, the track streaming */
	int music_on;
	int looping;
	wav64_t *track;
	unsigned long random;
	int last_version;
} sound;

void n64_sound_initialize(void)
{
	if (sound.ready)
		return;
	/* (the latency covers a slow game frame: the mixer is fed once a frame) */
	audio_init(SOUND_RATE, AUDIO_INIT_LATENCY_MS(200));
	mixer_init(4);
	sound.random = TICKS_READ();
	sound.ready = 1;
}

static void music_play(const char *path)
{
	if (sound.track)
	{
		mixer_ch_stop(MUSIC_CHANNEL);
		wav64_close(sound.track);
		sound.track = NULL;
	}
	sound.track = wav64_load(path, &(wav64_loadparms_t){ .streaming_mode = WAV64_STREAMING_FULL });
	if (!sound.track)
	{
		debugf("n64: no music %s\n", path);
		return;
	}
	mixer_ch_set_vol(MUSIC_CHANNEL, 0.8f, 0.8f);
	wav64_play(sound.track, MUSIC_CHANNEL);
	debugf("n64: music %s, %d kbit/s\n", path, wav64_get_bitrate(sound.track) / 1000);
}

/* the title music, from its start (nothing if it is already playing) */
void n64_music_start(void)
{
	n64_sound_initialize();
	if (sound.music_on)
		return;
	sound.music_on = 1;
	sound.looping = 0;
	sound.last_version = -1;
	music_play("rom:/music/title_in.wav64");
}

void n64_music_stop(void)
{
	if (!sound.music_on)
		return;
	sound.music_on = 0;
	if (sound.track)
	{
		mixer_ch_stop(MUSIC_CHANNEL);
		wav64_close(sound.track);
		sound.track = NULL;
	}
}

void n64_sound_poll(void)
{
	if (!sound.ready)
		return;
	/* a track done: the next loop version (a random one, not the one just
	played) */
	if (sound.music_on && !mixer_ch_playing(MUSIC_CHANNEL))
	{
		char path[48];
		int version;

		do
		{
			sound.random = sound.random * 1664525UL + 1013904223UL;
			version = (int)((sound.random >> 16) % LOOP_VERSIONS);
		} while (version == sound.last_version);
		sound.last_version = version;
		snprintf(path, sizeof(path), "rom:/music/title_loop%d.wav64", version);
		sound.looping = 1;
		music_play(path);
	}
	mixer_try_play();
}
