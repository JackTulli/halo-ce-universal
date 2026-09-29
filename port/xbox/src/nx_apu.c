/*
NX_APU.C

Sound output through the console's audio processor (the MCPX APU), as one
hardware voice.

The retail game's DirectSound plays every sound as a voice of the APU's
voice processor, which mixes them and hands the mix to the APU's two DSPs
and on to the codec. nxdk has no driver for any of that; its XAudio
(nx_audio.c) feeds the AC97 controller's bus-master DMA straight from
memory instead, which the hardware plays but the emulator does not: xemu
produces its host sound from the voice processor alone (with DSP emulation
off, its default, it sums every audible voice straight to the speakers;
with it on, from the global processor's mix buffers), and its AC97 DMA
has no host output. So here the mixer's 48 kHz 16-bit stereo output goes
into one looping voice: a ring of frames in contiguous memory that the
voice reads by its scatter-gather table, which a thread keeps filled a
little ahead of the voice's read position (the current buffer offset the
hardware writes back into the voice's structure every frame of 32
samples).

For the emulator's DSP mode the global processor also runs a three-word
program that only ends each frame at once (its mix buffers for bins 0 and
1 are then the output); the encode processor stays in reset. What the
voice processor needs is programmed by its front end's methods, written
one at a time to the APU's method window. Register and voice-structure
layout after the NV_PAPU/NV_PAVS names of the hardware.

Not tested on a console: on real hardware the codec would need the encode
processor's program to hear this path, so nx_audio.c takes the AC97 path
there unless audio.backend says otherwise.
*/

#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "nx.h"

/* ---------- the hardware */

#define APU_BASE 0xFE800000UL
#define APU_REG(offset) (*(volatile unsigned long *)(APU_BASE + (offset)))
/* the front end's method window, the global and the encode processor */
#define VP_METHOD(offset) (*(volatile unsigned long *)(APU_BASE + 0x20000UL + (offset)))
#define GP_REG(offset) (*(volatile unsigned long *)(APU_BASE + 0x30000UL + (offset)))
#define EP_REG(offset) (*(volatile unsigned long *)(APU_BASE + 0x50000UL + (offset)))

enum
{
	NV_PAPU_ISTS = 0x1000,
	NV_PAPU_IEN = 0x1004,
	NV_PAPU_FECTL = 0x1100,
	NV_PAPU_FENADDR = 0x115C,
	NV_PAPU_FETFORCE1 = 0x1504,
	NV_PAPU_SECTL = 0x2000,
	NV_PAPU_VPVADDR = 0x202C,
	NV_PAPU_VPSGEADDR = 0x2030,
	NV_PAPU_VPSSLADDR = 0x2034,
	NV_PAPU_GPSADDR = 0x2040,
	NV_PAPU_GPFADDR = 0x2044,
	NV_PAPU_EPSADDR = 0x2048,
	NV_PAPU_EPFADDR = 0x204C,
	NV_PAPU_TVL2D = 0x2054,
	NV_PAPU_TVL3D = 0x2060,
	NV_PAPU_TVLMP = 0x206C,
	NV_PAPU_GPSMAXSGE = 0x20D4,
	NV_PAPU_GPFMAXSGE = 0x20D8,
	NV_PAPU_EPSMAXSGE = 0x20DC,
	NV_PAPU_EPFMAXSGE = 0x20E0,
	/* the sound engine's frame counter mode: off stops everything */
	NV_PAPU_SECTL_XCNTMODE_RUN = 1 << 3,
	/* the front end traps when a listed voice goes idle */
	NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE = 1 << 15,
	/* the processors' reset registers: both bits set runs one */
	NV_PAPU_GPRST = 0xFFFC,
	NV_PAPU_GPRST_RUN = 3,

	/* the front end's methods */
	NV1BA0_PIO_FREE = 0x0010,
	NV1BA0_PIO_SET_ANTECEDENT_VOICE = 0x0120,
	NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP = 1 << 16,
	NV1BA0_PIO_VOICE_ON = 0x0124,
	NV1BA0_PIO_VOICE_OFF = 0x0128,
	NV1BA0_PIO_SET_SUBMIX_HEADROOM = 0x0200,
	NV1BA0_PIO_SET_CURRENT_VOICE = 0x02F8,
	NV1BA0_PIO_SET_VOICE_CFG_VBIN = 0x0300,
	NV1BA0_PIO_SET_VOICE_CFG_FMT = 0x0304,
	NV1BA0_PIO_SET_VOICE_CFG_ENV0 = 0x0308,
	NV1BA0_PIO_SET_VOICE_CFG_ENVA = 0x030C,
	NV1BA0_PIO_SET_VOICE_CFG_ENV1 = 0x0310,
	NV1BA0_PIO_SET_VOICE_CFG_ENVF = 0x0314,
	NV1BA0_PIO_SET_VOICE_CFG_MISC = 0x0318,
	NV1BA0_PIO_SET_VOICE_TAR_VOLA = 0x0360,
	NV1BA0_PIO_SET_VOICE_TAR_VOLB = 0x0364,
	NV1BA0_PIO_SET_VOICE_TAR_VOLC = 0x0368,
	NV1BA0_PIO_SET_VOICE_LFO_ENV = 0x036C,
	NV1BA0_PIO_SET_VOICE_TAR_FCA = 0x0374,
	NV1BA0_PIO_SET_VOICE_TAR_FCB = 0x0378,
	NV1BA0_PIO_SET_VOICE_TAR_PITCH = 0x037C,
	NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE = 0x03A0,
	NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO = 0x03A4,
	NV1BA0_PIO_SET_VOICE_BUF_CBO = 0x03D8,
	NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO = 0x03DC,

	/* the voice structure: 128 bytes a voice; the read position */
	NV_PAVS_SIZE = 0x80,
	NV_PAVS_VOICE_PAR_OFFSET = 0x58,
	NV_PAVS_VOICE_PAR_OFFSET_CBO = 0x00FFFFFF,
	/* the voice's format: 16-bit stereo, two samples a block, looping */
	NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK_2 = 1 << 16,
	NV_PAVS_VOICE_CFG_FMT_LOOP = 1 << 25,
	NV_PAVS_VOICE_CFG_FMT_STEREO = 1 << 27,
	NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 = 1 << 28,
	NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 = 1 << 30,

	/* voices below 64 are the 3D ones (HRTF); this is the first plain one */
	VOICE_HANDLE = 64,
	VOICE_COUNT = 256,
	MIXBIN_COUNT = 32,
};

/* ---------- the memory

one contiguous, uncached allocation, in 4 KB pages: the hardware reads and
writes all of it by physical address */
enum
{
	PAGE = 4096,
	/* the ring: 16384 frames of 16-bit stereo, 340 ms; the writer moves in
	chunks, which divide it, so no chunk straddles its end */
	RING_FRAMES = 16384,
	RING_BYTES = RING_FRAMES * 4,
	RING_PAGES = RING_BYTES / PAGE,
	CHUNK_FRAMES = 256,
	/* how far ahead of the voice the ring is kept (64 ms), and the lead
	past which the voice must have overtaken the writer instead */
	TARGET_LEAD = 3072,
	OVERRUN_LEAD = TARGET_LEAD + 1024,
	FILL_INTERVAL_MS = 8,

	VOICES_BYTES = VOICE_COUNT * NV_PAVS_SIZE,
	/* the scatter-gather tables: 8-byte entries, the ring's pages first,
	then the global processor's scratch pages */
	TABLES_BYTES = PAGE,
	GP_SGE_OFFSET = 0x800,
	/* notifiers: 16 bytes for each of a voice's four, after two */
	NOTIFIERS_BYTES = 6 * PAGE,
	SSL_BYTES = PAGE,
	/* the global processor's scratch, which it boots its program from */
	GP_SCRATCH_PAGES = 2,
	GP_SCRATCH_BYTES = GP_SCRATCH_PAGES * PAGE,

	/* the ring first: anything written past it would land in the tables
	here, not in someone else's memory */
	RING_OFFSET = 0,
	VOICES_OFFSET = RING_OFFSET + RING_BYTES,
	TABLES_OFFSET = VOICES_OFFSET + VOICES_BYTES,
	NOTIFIERS_OFFSET = TABLES_OFFSET + TABLES_BYTES,
	SSL_OFFSET = NOTIFIERS_OFFSET + NOTIFIERS_BYTES,
	GP_SCRATCH_OFFSET = SSL_OFFSET + SSL_BYTES,
	MEMORY_BYTES = GP_SCRATCH_OFFSET + GP_SCRATCH_BYTES,
};

static void (*fill_buffer)(short *samples, unsigned long frames);
static unsigned char *memory;
static unsigned long memory_physical;
static short *ring;
static volatile unsigned long *voice;
/* the writer's position in frames, and what the voice played */
static unsigned long write_frame;
static unsigned long last_cbo;
static volatile unsigned long consumed, underruns;
static int started;

static unsigned long physical(unsigned long offset)
{
	return memory_physical + offset;
}

/* a method to the front end: wait for room in its queue first */
static void vp_method(unsigned long method, unsigned long value)
{
	unsigned long spins = 100000;

	while (VP_METHOD(NV1BA0_PIO_FREE) < 8 && spins-- > 0)
		;
	VP_METHOD(method) = value;
}

static unsigned long voice_cbo(void)
{
	return voice[NV_PAVS_VOICE_PAR_OFFSET / 4] & NV_PAVS_VOICE_PAR_OFFSET_CBO;
}

/* the global processor's program: end the frame at once, forever

	loop: movep #>1,x:<<$FFFFC4  ; the frame-done request
	      jmp loop */
static const unsigned long gp_program[] = { 0x08F484, 0x000001, 0x0C0000 };

static void program_hardware(void)
{
	unsigned long *tables = (unsigned long *)(memory + TABLES_OFFSET);
	unsigned long *scratch = (unsigned long *)(memory + GP_SCRATCH_OFFSET);
	unsigned long index;

	/* everything off while it is set up */
	APU_REG(NV_PAPU_SECTL) = 0;
	APU_REG(NV_PAPU_IEN) = 0;
	GP_REG(NV_PAPU_GPRST) = 0;
	EP_REG(NV_PAPU_GPRST) = 0;
	APU_REG(NV_PAPU_FECTL) = 0;
	Sleep(10);
	APU_REG(NV_PAPU_ISTS) = 0xFFFFFFFF;

	/* the tables: the ring's pages, then the scratch's */
	for (index = 0; index < RING_PAGES; index++)
	{
		tables[index * 2] = physical(RING_OFFSET + index * PAGE);
		tables[index * 2 + 1] = 0;
	}
	for (index = 0; index < GP_SCRATCH_PAGES; index++)
	{
		tables[GP_SGE_OFFSET / 4 + index * 2] = physical(GP_SCRATCH_OFFSET + index * PAGE);
		tables[GP_SGE_OFFSET / 4 + index * 2 + 1] = 0;
	}
	for (index = 0; index < sizeof(gp_program) / sizeof(gp_program[0]); index++)
		scratch[index] = gp_program[index];

	APU_REG(NV_PAPU_VPVADDR) = physical(VOICES_OFFSET);
	APU_REG(NV_PAPU_VPSGEADDR) = physical(TABLES_OFFSET);
	APU_REG(NV_PAPU_VPSSLADDR) = physical(SSL_OFFSET);
	APU_REG(NV_PAPU_FENADDR) = physical(NOTIFIERS_OFFSET);
	APU_REG(NV_PAPU_GPSADDR) = physical(TABLES_OFFSET + GP_SGE_OFFSET);
	APU_REG(NV_PAPU_GPSMAXSGE) = GP_SCRATCH_PAGES - 1;
	APU_REG(NV_PAPU_GPFADDR) = physical(TABLES_OFFSET + GP_SGE_OFFSET);
	APU_REG(NV_PAPU_GPFMAXSGE) = GP_SCRATCH_PAGES - 1;
	APU_REG(NV_PAPU_EPSADDR) = physical(TABLES_OFFSET + GP_SGE_OFFSET);
	APU_REG(NV_PAPU_EPSMAXSGE) = GP_SCRATCH_PAGES - 1;
	APU_REG(NV_PAPU_EPFADDR) = physical(TABLES_OFFSET + GP_SGE_OFFSET);
	APU_REG(NV_PAPU_EPFMAXSGE) = GP_SCRATCH_PAGES - 1;
	/* empty voice lists, and the trap for a listed voice going idle
	(never expected: the one voice loops) */
	APU_REG(NV_PAPU_TVL2D) = 0xFFFF;
	APU_REG(NV_PAPU_TVL3D) = 0xFFFF;
	APU_REG(NV_PAPU_TVLMP) = 0xFFFF;
	APU_REG(NV_PAPU_FETFORCE1) |= NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE;

	/* no headroom on any mix bin */
	for (index = 0; index < MIXBIN_COUNT; index++)
		vp_method(NV1BA0_PIO_SET_SUBMIX_HEADROOM + index * 4, 0);

	/* the voice: left to bin 0 and right to bin 1 at full volume, the
	other six of its eight sends muted (volume 0xFFF); 16-bit stereo
	frames in a looping buffer; no envelopes, filters or pitch shift */
	vp_method(NV1BA0_PIO_SET_CURRENT_VOICE, VOICE_HANDLE);
	vp_method(NV1BA0_PIO_SET_VOICE_CFG_VBIN, 0 | (1 << 5) | (2 << 10) | (3 << 16) | (4 << 21) | (5 << 26));
	vp_method(NV1BA0_PIO_SET_VOICE_CFG_FMT,
		6 | (7 << 5) | NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK_2 | NV_PAVS_VOICE_CFG_FMT_LOOP |
		NV_PAVS_VOICE_CFG_FMT_STEREO | NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 |
		NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16);
	vp_method(NV1BA0_PIO_SET_VOICE_CFG_ENV0, 0);
	vp_method(NV1BA0_PIO_SET_VOICE_CFG_ENVA, 0);
	vp_method(NV1BA0_PIO_SET_VOICE_CFG_ENV1, 0);
	vp_method(NV1BA0_PIO_SET_VOICE_CFG_ENVF, 0);
	vp_method(NV1BA0_PIO_SET_VOICE_CFG_MISC, 0);
	vp_method(NV1BA0_PIO_SET_VOICE_TAR_VOLA, 0x000F000F);
	vp_method(NV1BA0_PIO_SET_VOICE_TAR_VOLB, 0xFFFFFFFF);
	vp_method(NV1BA0_PIO_SET_VOICE_TAR_VOLC, 0xFFFFFFFF);
	vp_method(NV1BA0_PIO_SET_VOICE_LFO_ENV, 0);
	vp_method(NV1BA0_PIO_SET_VOICE_TAR_FCA, 0);
	vp_method(NV1BA0_PIO_SET_VOICE_TAR_FCB, 0);
	vp_method(NV1BA0_PIO_SET_VOICE_TAR_PITCH, 0);
	vp_method(NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE, 0);
	vp_method(NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO, 0);
	vp_method(NV1BA0_PIO_SET_VOICE_BUF_CBO, 0);
	vp_method(NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO, RING_FRAMES - 1);

	/* the global processor boots its program from the scratch */
	GP_REG(NV_PAPU_GPRST) = NV_PAPU_GPRST_RUN;
}

static void start_voice(void)
{
	/* the sound engine runs, then the voice joins the 2D list */
	APU_REG(NV_PAPU_SECTL) = NV_PAPU_SECTL_XCNTMODE_RUN;
	vp_method(NV1BA0_PIO_SET_ANTECEDENT_VOICE, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP);
	vp_method(NV1BA0_PIO_VOICE_ON, VOICE_HANDLE);
}

/* the mixer's counters (dsound_sdl.c, the console's part) */
void dsound_statistics(unsigned long *streams_count, unsigned long *packets_count, unsigned long *voiced_frames,
	unsigned long *peak);

/* the mixer's time, in the processor's cycles (733 MHz) */
static unsigned long long mix_cycles;

static unsigned long long cycle_count(void)
{
	return __builtin_ia32_rdtsc();
}

static unsigned long __stdcall fill_thread(void *parameter)
{
	unsigned long passes = 0;

	(void)parameter;
	for (;;)
	{
		unsigned long cbo = voice_cbo() % RING_FRAMES;
		unsigned long lead;

		if (started)
		{
			consumed += (cbo - last_cbo) % RING_FRAMES;
			last_cbo = cbo;
		}
		lead = (write_frame - cbo) % RING_FRAMES;
		if (started && lead > OVERRUN_LEAD)
		{
			/* the voice ran past the writer: stale frames went out;
			start again at the next chunk ahead of it (the writer keeps
			to whole chunks, which never straddle the ring's end) */
			underruns++;
			write_frame = (cbo / CHUNK_FRAMES + 1) * CHUNK_FRAMES % RING_FRAMES;
			lead = (write_frame - cbo) % RING_FRAMES;
		}
		while (lead < TARGET_LEAD)
		{
			unsigned long long start = cycle_count();

			if (write_frame + CHUNK_FRAMES > RING_FRAMES)
				write_frame = 0;
			fill_buffer(ring + write_frame * 2, CHUNK_FRAMES);
			mix_cycles += cycle_count() - start;
			write_frame = (write_frame + CHUNK_FRAMES) % RING_FRAMES;
			lead += CHUNK_FRAMES;
		}
		if (!started)
		{
			started = 1;
			last_cbo = 0;
			start_voice();
		}
		if (++passes % (5000 / FILL_INTERVAL_MS) == 0)
		{
			/* every five seconds: what the hardware and the mixer did */
			char text[240];
			unsigned long streams, packets, voiced, peak;

			dsound_statistics(&streams, &packets, &voiced, &peak);
			snprintf(text, sizeof(text),
				"halo-xbox: apu: played %lu frames, restarted %lu, cbo %lu write %lu, ists %08lx fectl %08lx gprst %lx; "
				"streams %lu, packets %lu, voiced frames %lu, peak %lu; mixing %lu ms a second\n",
				consumed, underruns, voice_cbo(), write_frame, APU_REG(NV_PAPU_ISTS), APU_REG(NV_PAPU_FECTL),
				GP_REG(NV_PAPU_GPRST), streams, packets, voiced, peak,
				(unsigned long)(mix_cycles / 733333ULL / 5ULL));
			mix_cycles = 0;
			nx_print(text);
		}
		Sleep(FILL_INTERVAL_MS);
	}
	return 0;
}

int nx_apu_start(void (*fill)(short *samples, unsigned long frames))
{
	char text[128];
	void *thread;

	if (memory)
		return 1;
	memory = MmAllocateContiguousMemoryEx(MEMORY_BYTES, 0, 0x03FFFFFF, PAGE, PAGE_READWRITE | PAGE_NOCACHE);
	if (!memory)
	{
		nx_print("halo-xbox: apu: no memory for the voice\n");
		return 0;
	}
	memset(memory, 0, MEMORY_BYTES);
	memory_physical = (unsigned long)MmGetPhysicalAddress(memory);
	ring = (short *)(memory + RING_OFFSET);
	voice = (volatile unsigned long *)(memory + VOICES_OFFSET + VOICE_HANDLE * NV_PAVS_SIZE);
	fill_buffer = fill;
	program_hardware();
	thread = nx_create_thread(64 * 1024, fill_thread, NULL, 0, NULL);
	if (!thread)
	{
		nx_print("halo-xbox: apu: cannot start the mixer thread\n");
		return 0;
	}
	/* ahead of the game's threads: the voice waits for no one */
	SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
	snprintf(text, sizeof(text), "halo-xbox: apu: voice %d, a ring of %d frames at physical %08lx\n", VOICE_HANDLE,
		RING_FRAMES, physical(RING_OFFSET));
	nx_print(text);
	return 1;
}

void nx_apu_statistics(unsigned long *consumed_count, unsigned long *underrun_count)
{
	*consumed_count = consumed;
	*underrun_count = underruns;
}
