/*
NX_GPU.C

The GPU behind the Direct3D device (d3d8_nv2a.c): nxdk's pbkit starts the
NV2A, owns the push buffer and the display's frame buffers and flips them
at the vertical blank; this unit is its plain-C face (nx.h).

pbkit keeps three rotating frame buffers and a depth buffer, and points the
colour and depth DMA windows at the current ones; the device instead binds
a DMA context over all of memory for colour (so any surface can be the
render target, as on the console) and leaves pbkit's depth window in place
(its depth buffer, at offset 0).
*/

#include <pbkit/pbkit.h>
#include <hal/video.h>
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "nx.h"

static int started;
static void resync(void);
static void wait_until_graphics_idle(void);
static void drain_for_console(void);
static unsigned long flush_dwords; /* (below, with the ring) */
static void park_unless_console_owner(void);
static void ensure_room(unsigned long dwords);
/* the free space last read from the pusher, less what has been written
since: the pusher only frees more, so a stale count is a safe one, and the
register (a slow access under emulation) is read only when the count runs
short, every few kilobytes rather than every push */
static unsigned long free_cache;

static uint32_t *push_head;
static unsigned long push_size;
static struct s_CtxDma color_context;
static struct s_CtxDma report_context;
static void *reports;

int nx_gpu_start(unsigned long push_buffer_size)
{
	int status;

	if (started)
		return 1;
	if (push_buffer_size < 512 * 1024)
		push_buffer_size = 512 * 1024;
	pb_size(push_buffer_size);
	nx_video_configure();
	/* 720p in the low 64 MB: two frame buffers, and none of pbkit's depth
	buffer (the device has its own) (tools/xbox_pbkit_patch.py) */
	if (nx_video_low_memory_hd())
	{
		pb_set_back_buffer_count(1);
		flush_dwords = 2048;
	}
	/* none of pbkit's depth buffer: the device has its own
	(nx_gpu_depth_buffer), and nothing addressed pbkit's (1.3 MB at 480) */
	pb_set_depth_buffer(0);
	status = pb_init();
	if (status != 0)
	{
		char text[96];

		snprintf(text, sizeof(text), "halo-xbox: the GPU did not start (pb_init %d)\n", status);
		nx_print(text);
		return 0;
	}
	pb_show_front_screen();
	nx_print_screen_hidden();
	nx_video_apply_widescreen();
	push_size = push_buffer_size;
	/* pbkit's first target: its depth buffer becomes the depth DMA window */
	pb_target_back_buffer();
	/* and its last: the device binds its own window and surface at each
	frame (d3d8_nv2a.c, apply_render_target), so pb_finished need not
	re-target pbkit's, eight GPU interrupts a frame (tools/xbox_pbkit_patch.py,
	version 6) */
	pb_set_retarget_on_finish(0);
	/* all of memory: at 720p the game's surfaces, textures and vertices
	are in the top 64 MB of a 128 MB console (its last 20 KB are the
	GPU's, as MAXRAM leaves them out of the low 64 MB) */
	pb_create_dma_ctx(NX_GPU_MEMORY_CONTEXT_COLOR, DMA_CLASS_3D, 0,
		nx_high_memory_placed() ? nx_physical_top() - 0x5001 : MAXRAM, &color_context);
	pb_bind_channel(&color_context);
	/* the visibility reports: a page of their own, which their DMA context
	spans, so the report methods' 24-bit offsets are within it */
	reports = MmAllocateContiguousMemoryEx(NX_GPU_REPORT_BYTES, 0, MAXRAM, 4096, PAGE_READWRITE | PAGE_NOCACHE);
	if (reports)
	{
		memset(reports, 0, NX_GPU_REPORT_BYTES);
		pb_create_dma_ctx(NX_GPU_MEMORY_CONTEXT_REPORT, DMA_CLASS_3D, (DWORD)reports & 0x03FFFFFF,
			NX_GPU_REPORT_BYTES - 1, &report_context);
		pb_bind_channel(&report_context);
	}
	pb_reset();
	push_head = pb_begin();
	pb_end(push_head);
	resync();
	started = 1;
	return 1;
}

int nx_gpu_started(void)
{
	return started;
}

int nx_gpu_flip_pending(void)
{
	return started && pb_flip_pending();
}

/* whether the buffer the next frame is drawn into is still on the screen or
waiting to be: it was the frame's (buffers - 1) before, so the display must
have flipped to the one after that (tools/xbox_pbkit_patch.py, version 4).
pbkit's ready table counts only frames the GPU has finished, which with the
frame rate unlocked let the GPU draw into the frame being shown */
int nx_gpu_back_buffer_in_use(void)
{
	int buffers = nx_video_low_memory_hd() ? 2 : 3;

	return started && pb_frames_pending() > buffers - 2;
}

int nx_gpu_frames_pending(void)
{
	return started ? pb_frames_pending() : 0;
}

void *nx_gpu_reports(void)
{
	return reports;
}

/* the thread that showed the console (a crash report): from then on any
other thread that pushes parks at its next push (park_unless_console_owner),
so the game, still drawing on its own thread, cannot draw over the report:
at 720p in the low 64 MB the screen is the depth buffer too, which every
frame clears */
static PKTHREAD volatile console_owner;

static void park_unless_console_owner(void)
{
	PKTHREAD owner = console_owner;

	if (owner && owner != KeGetCurrentThread())
	{
		for (;;)
			Sleep(1000);
	}
}

void nx_gpu_show_console(void)
{
	if (started)
	{
		if (!console_owner)
			console_owner = KeGetCurrentThread();
		/* every other pusher to the slow path, where it parks */
		nx_gpu_room_end = nx_gpu_put;
		nx_gpu_kick_at = NULL;
		nx_gpu_flush();
		ensure_room(4096);
		/* what was queued drawn first, before the text */
		drain_for_console();
		pb_show_debug_screen();
		nx_print_screen_shown();
		resync();
		free_cache = 0;
		nx_gpu_room_end = nx_gpu_put;
		nx_gpu_kick_at = NULL;
	}
}

unsigned long nx_gpu_back_buffer(void)
{
	return (unsigned long)pb_back_buffer() & 0x03FFFFFF;
}

unsigned long nx_gpu_back_buffer_pitch(void)
{
	return pb_back_buffer_pitch();
}

unsigned long nx_gpu_back_buffer_width(void)
{
	return pb_back_buffer_width();
}

unsigned long nx_gpu_back_buffer_height(void)
{
	return pb_back_buffer_height();
}

/* The device addresses depth through the same all-memory window as colour,
so a depth surface's offset is a physical address, and the game can use
the depth buffer's memory as a colour surface (its screen effects copy the
frame there). pbkit's own depth buffer sits behind a window of its own at
offset 0 and cannot be named that way, so the device gets one here. */
unsigned long nx_gpu_depth_buffer(unsigned long bytes)
{
	static void *buffer;
	static unsigned long size;
	static int shared;

	if (!buffer || size < bytes)
	{
		if (buffer && !shared)
			MmFreeContiguousMemory(buffer);
		buffer = NULL;
		shared = 0;
		/* 720p in the low 64 MB: nxdk's text screen is the depth buffer
		too. XVideoSetMode gave it the mode's size (1280x720x4, 3.5 MB, the
		depth buffer's exactly), contiguous and write-combined, for text
		that shows only before the game does and after a crash, when the
		depth is of no more use (nx_print_screen_shown clears it first); a
		stray line during play marks a few depth values for a frame. That
		is the room a 64 MB console needs for 720p. */
		if (nx_video_low_memory_hd())
		{
			void *screen = XVideoGetFB();

			if (screen && MmQueryAllocationSize(screen) >= bytes)
			{
				buffer = screen;
				shared = 1;
			}
		}
		/* (at 720p in the top 64 MB, like the game's other surfaces) */
		if (!buffer)
			buffer = nx_physical_alloc(bytes, 0xffffffffUL, 4096, PAGE_READWRITE | PAGE_WRITECOMBINE);
		size = buffer ? bytes : 0;
		if (buffer)
			memset(buffer, 0, bytes);
	}
	return buffer ? (unsigned long)buffer & 0x7FFFFFFF : 0;
}

/* The commands go into the ring as they are written; the GPU is told
about them (pbkit's pb_end: a write-combine cache flush and the PUT
register, several register accesses the emulator turns into VM exits) only
every few kilobytes and before anything waits on it. Telling it after every
Direct3D call, as before, cost tens of thousands of exits a frame. */

#define FLUSH_DWORDS 8192
/* 2048 at 720p in the low 64 MB: double buffered, the GPU waits for the CPU
at each frame's start, and a smaller kick sets it going sooner */
static unsigned long flush_dwords = FLUSH_DWORDS;
/* the room ensure_room keeps beyond any reservation: pbkit's own pushes
(the flip, the debug screen) check nothing, and they land in it */
#define ROOM_MARGIN 1025

/* the write position (the end of what the CPU has written) and the fast
path's bounds (nx.h): a reservation that ends below nx_gpu_room_end needs
no checks, a block that ends at or past nx_gpu_kick_at is flushed. The
device's pushes are inline checks against them; everything else (the free
space, the ring's wrap, the kick, the statistics) is here, in the slow path */
unsigned long *nx_gpu_put;
unsigned long *nx_gpu_room_end;
unsigned long *nx_gpu_kick_at;
static uint32_t *flushed_ptr; /* the end the GPU has been told about (pbkit's pb_Put) */
/* where free_cache was counted from: the free space now is free_cache less
what has been written since */
static uint32_t *room_base;
static unsigned long stat_kicks, stat_dwords;
static ULONGLONG stat_wait_ticks;

static uint32_t *put_pointer(void)
{
	return (uint32_t *)nx_gpu_put;
}

static void set_kick_at(void)
{
	nx_gpu_kick_at = (unsigned long *)(flushed_ptr + flush_dwords);
}

/* no fast path until ensure_room has counted the room again */
static void fast_path_close(void)
{
	nx_gpu_room_end = nx_gpu_put;
	free_cache = 0;
}

static void resync(void)
{
	flushed_ptr = pb_begin();
	nx_gpu_put = (unsigned long *)flushed_ptr;
	set_kick_at();
	fast_path_close();
}

void nx_gpu_flush(void)
{
	uint32_t *put;

	park_unless_console_owner();
	put = put_pointer();

	if (put != flushed_ptr)
	{
		stat_dwords += (unsigned long)(put - flushed_ptr);
		pb_end(put);
		flushed_ptr = put;
		stat_kicks++;
	}
	set_kick_at();
}

unsigned long *nx_gpu_begin(void)
{
	return nx_gpu_put;
}

/* the pusher's read position (NV_PFIFO_CACHE1_DMA_GET): a physical address */
static unsigned long gpu_get(void)
{
	return *(volatile unsigned long *)0xFD003244UL & 0x03FFFFFFUL;
}

/* The dwords the pusher has left free ahead of put: it fetches from GET up
to PUT (what it has been told, flushed_ptr), following the jump at the tail
to the head. The CPU never passes it, so a GET beyond put in address is the
previous lap (free up to it), a GET behind put is this lap (free to the tail
and on to GET), and a GET at PUT has fetched everything it was told: the
ring is free but for what has been written since, from flushed_ptr to put. */
static unsigned long pusher_free_dwords(uint32_t *put)
{
	unsigned long mask = 0x03FFFFFFUL;
	unsigned long get = gpu_get();
	unsigned long put_physical = (unsigned long)put & mask;
	unsigned long flushed = (unsigned long)flushed_ptr & mask;
	unsigned long head = (unsigned long)push_head & mask, tail = head + push_size;

	if (get == flushed)
		return (push_size - (put_physical - flushed)) / 4;
	if (get > put_physical)
		return (get - put_physical) / 4;
	if (get == put_physical)
		return 0;
	return (tail - put_physical + (get - head)) / 4;
}

/* room for dwords at the write pointer: a wait while the pusher is still
fetching where the block would go (which the frame's commands tell it to
finish: a flush first), and at the ring's tail a jump to its head. PUT goes
to the head at once; the pusher fetches up to the jump and follows it.
pbkit's own pushes (the flip, the debug screen) check nothing, so they get
room first. */
static void ensure_room(unsigned long dwords)
{
	unsigned long need = dwords + 1024, spins = 0; /* a wide margin: pbkit's flip pushes */
	unsigned long known = 0, to_tail = 0, room;
	ULONGLONG waited_from = 0;
	uint32_t *put;

	for (;;)
	{
		unsigned long used;

		put = put_pointer();
		to_tail = (unsigned long)(push_head + push_size / 4 - put);
		used = (unsigned long)(put - room_base);
		known = free_cache > used ? free_cache - used : 0;
		if (known <= need + 1)
		{
			free_cache = pusher_free_dwords(put);
			room_base = put;
			known = free_cache;
		}
		if (known <= need + 1)
		{
			if (!spins)
			{
				nx_gpu_flush();
				waited_from = KeQueryPerformanceCounter();
			}
			if (++spins == 50000000UL)
			{
				nx_print("halo-xbox: the GPU stopped reading the push buffer\n");
				break;
			}
			__asm__ __volatile__("pause");
			continue;
		}
		if (to_tail < need + 1)
		{
			*put = 1 + (((unsigned long)push_head) & 0x0FFFFFFF);
			stat_dwords += (unsigned long)(put - flushed_ptr);
			pb_end(push_head);
			stat_kicks++;
			nx_gpu_put = (unsigned long *)push_head;
			flushed_ptr = push_head;
			set_kick_at();
			free_cache = 0;
			continue;
		}
		break;
	}
	if (spins)
		stat_wait_ticks += KeQueryPerformanceCounter() - waited_from;
	room = known < to_tail ? known : to_tail;
	nx_gpu_room_end = room > ROOM_MARGIN ? (unsigned long *)(put + room - ROOM_MARGIN) : (unsigned long *)put;
}

unsigned long *nx_gpu_reserve_slow(unsigned long dwords)
{
	park_unless_console_owner();
	ensure_room(dwords);
	return nx_gpu_put;
}

void nx_gpu_end_slow(unsigned long *end)
{
	park_unless_console_owner();
	nx_gpu_put = end;
	nx_gpu_flush();
}

unsigned long nx_gpu_room(void)
{
	uint32_t *tail = push_head + push_size / 4;
	uint32_t *put = put_pointer();

	return put < tail ? (unsigned long)(tail - put) : 0;
}

void nx_gpu_reset(void)
{
	nx_gpu_flush();
	pb_reset();
	push_head = pb_begin();
	pb_end(push_head);
	resync();
	free_cache = 0;
}

int nx_gpu_finish(void)
{
	int result;

	park_unless_console_owner();
	nx_gpu_flush();
	ensure_room(1024);
	result = pb_finished();
	resync();
	free_cache = 0;
	return result;
}

void nx_gpu_wait_for_vblank(void)
{
	nx_gpu_flush();
	pb_wait_for_vbl();
}

/* the same wait for another thread than the renderer's, which must not
touch the ring (the renderer flushes before its own waits) */
void nx_gpu_wait_for_vblank_only(void)
{
	pb_wait_for_vbl();
}

unsigned long nx_gpu_vblank_count(void)
{
	return pb_get_vbl_counter();
}

int nx_gpu_busy(void)
{
	nx_gpu_flush();
	return pb_busy();
}

/* the graphics engine idle, by its status register alone. pbkit's
pb_wait_until_gr_not_busy also runs the interrupt handlers itself, from the
thread, and an interrupt between its read of the pending bits and its call
had the handler run twice for one event: a flip or a frame's end counted
twice, and the counters behind Present's buffer wait (pb_frames_pending)
off for good. A trap the engine waits on is the interrupt's to serve. */
static void wait_until_graphics_idle(void)
{
	unsigned long spins = 0;

	while (*(volatile unsigned long *)0xFD400700UL != 0) /* NV_PGRAPH_STATUS: not busy */
	{
		if (++spins == 50000000UL)
		{
			nx_print("halo-xbox: the graphics engine did not go idle\n");
			break;
		}
		__asm__ __volatile__("pause");
	}
}

/* before a crash report shows: the pusher to the end of what it was told,
then the engine idle, within 200 ms. At a raised IRQL the DPC that serves
the GPU's traps (pbkit's subprograms, at every frame's end) cannot run, and
the GPU stops at the next one: no waiting then (shutdown_notification shows
the console again once it has lowered the IRQL) */
static void drain_for_console(void)
{
	ULONGLONG from = KeQueryPerformanceCounter(), limit = KeQueryPerformanceFrequency() / 5;
	unsigned long put = (unsigned long)flushed_ptr & 0x03FFFFFFUL;

	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		return;
	while (gpu_get() != put && KeQueryPerformanceCounter() - from < limit)
		__asm__ __volatile__("pause");
	while (*(volatile unsigned long *)0xFD400700UL != 0 && KeQueryPerformanceCounter() - from < limit)
		__asm__ __volatile__("pause");
}

void nx_gpu_wait_until_idle(void)
{
	nx_gpu_flush();
	wait_until_graphics_idle();
}

void nx_gpu_drain(void)
{
	unsigned long put, spins = 0;
	ULONGLONG waited_from = KeQueryPerformanceCounter();

	nx_gpu_flush();
	put = (unsigned long)flushed_ptr & 0x03FFFFFFUL;
	/* the pusher first: everything written has to be fetched before the
	engine's idle means anything */
	while (gpu_get() != put)
	{
		if (++spins == 50000000UL)
		{
			nx_print("halo-xbox: the GPU did not drain the push buffer\n");
			break;
		}
		__asm__ __volatile__("pause");
	}
	wait_until_graphics_idle();
	stat_wait_ticks += KeQueryPerformanceCounter() - waited_from;
}

unsigned long long nx_microseconds(void)
{
	ULONGLONG frequency = KeQueryPerformanceFrequency();

	return frequency ? KeQueryPerformanceCounter() * 1000000ULL / frequency : 0;
}

/* the counters since the last call, which clears them; the wait in
microseconds */
void nx_gpu_stats(unsigned long *kicks, unsigned long *dwords, unsigned long *wait_microseconds)
{
	ULONGLONG frequency = KeQueryPerformanceFrequency();

	*kicks = stat_kicks;
	*dwords = stat_dwords;
	*wait_microseconds = frequency ? (unsigned long)(stat_wait_ticks * 1000000ULL / frequency) : 0;
	stat_kicks = stat_dwords = 0;
	stat_wait_ticks = 0;
}
