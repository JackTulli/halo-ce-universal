/*
NX_AUDIO.C

Sound output: the choice between the console's two paths, and the AC97
one.

nxdk's XAudio (hal/audio.h) feeds the AC97 controller's bus-master DMA
48 kHz 16-bit stereo PCM from a ring of buffers in memory; a thread of
its own fills them with the mixer (port/linux/src/dsound_sdl.c, whose
SDL output this stands in for) and queues them to the hardware, keeping
a few ahead. It learns what the hardware has played from the controller's
current descriptor index (the AC97 bus master's CIV register), not from
nxdk's callback: nxdk raises that only once both the analogue and the
S/PDIF channel completed a buffer, and the emulator does not run the
S/PDIF one, so the callback never came (the first build played nothing).

The emulator plays nothing this way at all: xemu's host sound comes from
the audio processor's voices, so it gets nx_apu.c's path, one looping
voice of the APU. audio.backend chooses ("ac97", "apu", or "auto", which
tells the emulator by its AC97 codec's identity: QEMU's SigmaTel against
the console's Analog Devices part; the CPU's signature is no help, xemu
presents the console's own).
*/

#include <hal/audio.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "nx.h"

enum
{
	BUFFER_FRAMES = 1024,
	CHANNELS = 2,
	BUFFER_BYTES = BUFFER_FRAMES * CHANNELS * 2,
	/* the ring; queued to the hardware ahead: three, about 64 ms at 48 kHz */
	BUFFERS = 4,
	QUEUE_DEPTH = 3,
	/* the mixer thread's pace */
	FILL_INTERVAL_MS = 4,
};

/* the AC97 bus master's PCM out registers (nxdk's audio.c): the current
descriptor index, which advances as buffers play */
#define AC97_MMIO 0xFEC00000UL
#define AC97_PCM_OUT_CIV (*(volatile unsigned char *)(AC97_MMIO + 0x114))

static void (*fill_buffer)(short *samples, unsigned long frames);
static short *buffers;
/* the ring, all on the thread: the next buffer to fill, and those queued
to the hardware in order */
static unsigned long fill_index;
static unsigned long queued;
static unsigned char last_index;
static volatile unsigned long underruns, consumed;

/* nxdk's callback: unused (see above), and it must not touch the FPU */
static void consumed_callback(void *device, void *data)
{
	(void)device;
	(void)data;
}

/* the mixer's counters (dsound_sdl.c, the console's part) */
void dsound_statistics(unsigned long *streams_count, unsigned long *packets_count, unsigned long *voiced_frames,
	unsigned long *peak);

static unsigned long __stdcall fill_thread(void *parameter)
{
	int started = 0;
	unsigned long passes = 0;

	(void)parameter;
	for (;;)
	{
		if (started)
		{
			/* what played since the last look: the descriptor index moved
			on from each finished buffer */
			unsigned char index = AC97_PCM_OUT_CIV;
			unsigned long played = (unsigned char)(index - last_index) % 32;

			last_index = index;
			if (played > queued)
			{
				/* the hardware ran past everything queued: silence went
				out, and the count starts over */
				underruns += played - queued;
				played = queued;
			}
			consumed += played;
			queued -= played;
		}
		while (queued < QUEUE_DEPTH)
		{
			fill_buffer(buffers + fill_index * BUFFER_FRAMES * CHANNELS, BUFFER_FRAMES);
			XAudioProvideSamples((unsigned char *)(buffers + fill_index * BUFFER_FRAMES * CHANNELS), BUFFER_BYTES, 0);
			fill_index = (fill_index + 1) % BUFFERS;
			queued++;
		}
		if (!started)
		{
			started = 1;
			last_index = AC97_PCM_OUT_CIV;
			XAudioPlay();
		}
		if (++passes % (5000 / FILL_INTERVAL_MS) == 0)
		{
			/* every five seconds: what the hardware and the mixer did */
			char text[160];
			unsigned long streams, packets, voiced, peak;

			dsound_statistics(&streams, &packets, &voiced, &peak);
			snprintf(text, sizeof(text),
				"halo-xbox: audio: played %lu, late %lu, civ %u lvi %u sr %04x cr %02x; streams %lu, packets %lu, voiced frames %lu, peak %lu\n",
				consumed, underruns, AC97_PCM_OUT_CIV, *(volatile unsigned char *)(AC97_MMIO + 0x115),
				*(volatile unsigned short *)(AC97_MMIO + 0x116), *(volatile unsigned char *)(AC97_MMIO + 0x11B),
				streams, packets, voiced, peak);
			nx_print(text);
		}
		Sleep(FILL_INTERVAL_MS);
	}
	return 0;
}

static int ac97_start(void (*fill)(short *samples, unsigned long frames))
{
	char text[96];

	if (buffers)
		return 1;
	/* the hardware reads them by physical address: contiguous */
	buffers = MmAllocateContiguousMemoryEx(BUFFER_BYTES * BUFFERS, 0, 0x03FFFFFF, 64, PAGE_READWRITE | PAGE_NOCACHE);
	if (!buffers)
	{
		nx_print("halo-xbox: audio: no memory for the output buffers\n");
		return 0;
	}
	memset(buffers, 0, BUFFER_BYTES * BUFFERS);
	fill_buffer = fill;
	XAudioInit(16, CHANNELS, consumed_callback, NULL);
	/* the codec's master and PCM volumes: the AC97 reset state mutes both,
	and nothing else here unmutes them (the codec's registers are the
	first 128 bytes of the controller's window) */
	*(volatile unsigned short *)(AC97_MMIO + 0x02) = 0x0000;
	*(volatile unsigned short *)(AC97_MMIO + 0x18) = 0x0808;
	if (!nx_create_thread(64 * 1024, fill_thread, NULL, 0, NULL))
	{
		nx_print("halo-xbox: audio: cannot start the mixer thread\n");
		return 0;
	}
	snprintf(text, sizeof(text), "halo-xbox: audio: 48 kHz stereo, %d buffers of %d frames\n", BUFFERS, BUFFER_FRAMES);
	nx_print(text);
	return 1;
}

/* ---------- the choice */

static int apu_chosen;

static void cpuid(unsigned long leaf, unsigned long *a, unsigned long *b, unsigned long *c, unsigned long *d)
{
	__asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

/* the CPU's signature, for the log: the emulator presents the console's
own (family 6 model 8 stepping 10), so it tells nothing */
static void cpu_signature(char *signature, size_t size)
{
	unsigned long a, b, c, d, family, model;
	char vendor[13];

	cpuid(0, &a, &b, &c, &d);
	memcpy(vendor, &b, 4);
	memcpy(vendor + 4, &d, 4);
	memcpy(vendor + 8, &c, 4);
	vendor[12] = 0;
	cpuid(1, &a, &b, &c, &d);
	family = (a >> 8) & 0xF;
	model = (a >> 4) & 0xF;
	if (family == 6 || family == 15)
		model |= (a >> 12) & 0xF0;
	if (family == 15)
		family += (a >> 20) & 0xFF;
	snprintf(signature, size, "%s family %lu model %lu stepping %lu", vendor, family, model, a & 0xF);
}

/* the codec's identity: the console's is Analog Devices'; the emulator's
AC97 (QEMU's) answers as a SigmaTel STAC9700, 0x8384 0x7600 */
static int codec_is_the_emulators(unsigned long *identity)
{
	unsigned long id1 = *(volatile unsigned short *)(AC97_MMIO + 0x7C);
	unsigned long id2 = *(volatile unsigned short *)(AC97_MMIO + 0x7E);

	*identity = (id1 << 16) | id2;
	return id1 == 0x8384 && id2 == 0x7600;
}

int nx_audio_start(void (*fill)(short *samples, unsigned long frames), const char *backend)
{
	char signature[64], text[192];
	unsigned long codec;
	int emulator = codec_is_the_emulators(&codec);
	int want_apu;

	cpu_signature(signature, sizeof(signature));
	if (backend && strcmp(backend, "ac97") == 0)
		want_apu = 0;
	else if (backend && strcmp(backend, "apu") == 0)
		want_apu = 1;
	else
		want_apu = emulator;
	snprintf(text, sizeof(text), "halo-xbox: audio: codec %08lx (%s), %s; audio.backend %s: the %s path\n", codec,
		emulator ? "the emulator's" : "a console's", signature, backend && *backend ? backend : "auto",
		want_apu ? "audio processor's" : "AC97");
	nx_print(text);
	if (want_apu)
	{
		if (nx_apu_start(fill))
		{
			apu_chosen = 1;
			return 1;
		}
		nx_print("halo-xbox: audio: the audio processor did not start; the AC97 path instead\n");
	}
	return ac97_start(fill);
}

void nx_audio_statistics(unsigned long *consumed_count, unsigned long *underrun_count)
{
	if (apu_chosen)
	{
		nx_apu_statistics(consumed_count, underrun_count);
		return;
	}
	*consumed_count = consumed;
	*underrun_count = underruns;
}
