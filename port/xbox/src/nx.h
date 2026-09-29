/*
NX.H

The bridge between the Xbox-facing platform layer (compiled with the XDK
declarations) and nxdk (whose headers declare the same Win32 and kernel
names differently): plain C types only, implemented in nx_kernel.c against
nxdk's headers.
*/

#ifndef __HALO_XBOX_NX_H
#define __HALO_XBOX_NX_H

/* the video mode, the drives (U:, T:, Z:) and the screen console; once */
void nx_startup(void);

/* text to the kernel's debug output and the screen console */
void nx_print(const char *text);
/* the name of the last Win32 call the platform layer traced (xbox_kernel.c) */
const char *platform_last_call(void);

/* ---------- memory (MmAllocateContiguousMemoryEx and friends) */

/* physical_address 0xffffffff places the block anywhere; otherwise there */
void *nx_physical_alloc(unsigned long size, unsigned long physical_address, unsigned long alignment,
	unsigned long protect);
void nx_physical_free(void *address);
unsigned long nx_query_protect(const void *address);
void nx_set_protect(void *address, unsigned long size, unsigned long protect);
int nx_virtual_protect(void *address, unsigned long size, unsigned long protect, unsigned long *old_protect);
void nx_memory_status(unsigned long *total_bytes, unsigned long *available_bytes);
/* prints the free pages to the console */
void nx_report_memory(const char *when);

/* ---------- asynchronous reads and writes

The Win32 model, as the XAPI gave it: ReadFileEx and WriteFileEx start a
request whose completion routine runs on the requesting thread at its next
alertable wait (SleepEx, WaitForSingleObjectEx), which then returns
WAIT_IO_COMPLETION. Each request is an NtReadFile with an event; the
alertable waits below run the completions (nx_kernel.c). */

int nx_read_file_ex(void *file, void *buffer, unsigned long size, void *overlapped, void *completion_routine);
/* flush_on_completion: the file is flushed to the disk before the completion
routine runs (the saves) */
int nx_write_file_ex(void *file, const void *buffer, unsigned long size, void *overlapped, void *completion_routine,
	int flush_on_completion);
unsigned long nx_sleep_ex(unsigned long milliseconds, int alertable);
unsigned long nx_wait_for_single_object_ex(void *handle, unsigned long milliseconds, int alertable);

/* ---------- threads (CreateThread, whose nxdk version is cdecl)

Each thread starts under an exception handler frame (nx_crash.c), as does
the game's main; a fault prints a report and holds the thread. */

void *nx_create_thread(unsigned long stack_size, unsigned long (__stdcall *routine)(void *), void *parameter,
	unsigned long flags, unsigned long *thread_id);

/* nxdk's FindFirstFileA handles are records of its own, closed by FindClose */
int nx_find_close(void *handle);

/* writes a file's cached data through to the disk (NtFlushBuffersFile) */
int nx_flush_file(void *handle);

/* ---------- the GPU (nxdk's pbkit; nx_gpu.c)

pbkit owns three rotating frame buffers, a depth buffer and the push
buffer, and flips at the vertical blank. The colour DMA context below spans
all of memory, so a render target can be any surface; depth goes through
the same window, to a depth buffer of the device's own
(nx_gpu_depth_buffer), so the game can name that memory as a colour
surface too. */

#define NX_GPU_MEMORY_CONTEXT_COLOR 20
#define NX_GPU_MEMORY_CONTEXT_REPORT 22

/* starts the GPU with a push buffer of at least this size; 0 on failure */
int nx_gpu_start(unsigned long push_buffer_size);
int nx_gpu_started(void);
/* the visibility reports' page (NX_GPU_REPORT_BYTES, uncached), which the
report DMA context spans: a report's offset is its offset in the page */
#define NX_GPU_REPORT_BYTES 4096
void *nx_gpu_reports(void);
/* the text console back on the display, for a report after a failure */
void nx_gpu_show_console(void);
/* the frame buffer drawn next (physical), and its pitch */
unsigned long nx_gpu_back_buffer(void);
unsigned long nx_gpu_back_buffer_pitch(void);
unsigned long nx_gpu_back_buffer_width(void);
unsigned long nx_gpu_back_buffer_height(void);
/* a depth buffer of this many bytes in contiguous memory, allocated once;
its physical address, or 0 if there is no memory for it */
unsigned long nx_gpu_depth_buffer(unsigned long bytes);

/* the text screen (nx_kernel.c): hidden once the GPU shows the game, so
lines are not drawn on it; shown again for a crash report, with the last
lines replayed */
void nx_print_screen_hidden(void);
void nx_print_screen_shown(void);

/* ---------- sound output (nx_audio.c, nx_apu.c)

fill is called from a thread of its own for the next frames of 48 kHz
16-bit stereo PCM; backend is the audio.backend setting ("auto", "ac97"
or "apu"); returns 0 if the output could not start */
int nx_audio_start(void (*fill)(short *samples, unsigned long frames), const char *backend);
/* what the hardware played (AC97: buffers; the audio processor: frames),
and how often the mixer was late for it */
void nx_audio_statistics(unsigned long *consumed_count, unsigned long *underrun_count);
/* the audio processor's path (nx_apu.c): one looping voice of the voice
processor, which the emulator plays; returns 0 if it could not start */
int nx_apu_start(void (*fill)(short *samples, unsigned long frames));
void nx_apu_statistics(unsigned long *consumed_count, unsigned long *underrun_count);

/* ---------- the display mode from the dashboard's settings (nx_video.c) */

/* the dashboard's video flags (nxdk's VIDEO_* bits) */
unsigned long nx_video_flags(void);
/* sets the mode the flags ask for (480p); before the GPU starts */
void nx_video_configure(void);
/* tells the encoder the picture is 16:9 if the flags say widescreen; after
the GPU starts */
void nx_video_apply_widescreen(void);
/* whether the display is widescreen */
int nx_video_widescreen(void);
/* the buffer's columns (640, or 720 on a widescreen display) and the
columns the game draws (640, or 854 squeezed into the 720) */
unsigned long nx_video_physical_width(void);
unsigned long nx_video_logical_width(void);
/* the push buffer: begin returns where to write, end submits what was
written; room is what is left before a reset is needed, and reset jumps
back to the head (waiting for the GPU to arrive there) */
unsigned long *nx_gpu_begin(void);
/* the write position and the fast path's bounds (nx_gpu.c): a reservation
that ends below room_end needs no checks, a block that ends at or past
kick_at is flushed; the slow paths count the room again, wrap the ring,
wait for the GPU and tell it about what was written */
extern unsigned long *nx_gpu_put;
extern unsigned long *nx_gpu_room_end;
extern unsigned long *nx_gpu_kick_at;
unsigned long *nx_gpu_reserve_slow(unsigned long dwords);
void nx_gpu_end_slow(unsigned long *end);
/* room for dwords words at the write position, after the GPU is past it */
static inline unsigned long *nx_gpu_reserve(unsigned long dwords)
{
	if (nx_gpu_put + dwords < nx_gpu_room_end)
		return nx_gpu_put;
	return nx_gpu_reserve_slow(dwords);
}
/* waits until the GPU has fetched and finished everything written */
void nx_gpu_drain(void);
/* the performance counter, in microseconds */
unsigned long long nx_microseconds(void);
/* tells the GPU about everything written so far */
void nx_gpu_flush(void);
/* the counters since the last call, which clears them */
void nx_gpu_stats(unsigned long *kicks, unsigned long *dwords, unsigned long *wait_microseconds);
/* what was written ends at end */
static inline void nx_gpu_end(unsigned long *end)
{
	if (end >= nx_gpu_kick_at)
		nx_gpu_end_slow(end);
	else
		nx_gpu_put = end;
}
unsigned long nx_gpu_room(void);
void nx_gpu_reset(void);
/* queues the flip to the frame just drawn at the next vertical blank and
moves on to the next buffer; nonzero when the queue is full (try again) */
int nx_gpu_finish(void);
void nx_gpu_wait_for_vblank(void);
/* the same wait without the flush, for a thread other than the renderer */
void nx_gpu_wait_for_vblank_only(void);
unsigned long nx_gpu_vblank_count(void);
int nx_gpu_busy(void);
void nx_gpu_wait_until_idle(void);

/* ---------- controllers (nxdk's USB stack; nx_input.c)

The four ports' gamepads. The stack needs polling (from the game's device
change check, every frame); a gamepad's state is its last report, in the
XAPI's layout: the digital buttons, the eight analog ones (A, B, X, Y,
black, white, the triggers) and the sticks. */

#define NX_INPUT_PORTS 4

struct nx_gamepad_state
{
	unsigned short buttons;
	unsigned char analog_buttons[8];
	short thumb_left_x, thumb_left_y, thumb_right_x, thumb_right_y;
};

int nx_input_start(void);
void nx_input_poll(void);
/* nonzero with the state of the gamepad on the port (0 to 3), and the
count of its reports so far */
int nx_input_gamepad(unsigned long port, struct nx_gamepad_state *state, unsigned long *packet);
void nx_input_rumble(unsigned long port, unsigned short left, unsigned short right);

/* ---------- the console */

/* XC_LANGUAGE from the EEPROM: 1 English ... 6 Italian */
unsigned long nx_language(void);
/* the launch data page: returns nonzero when there is one, with its type
and 3072 bytes of data */
int nx_launch_info(unsigned long *type, void *data);
/* reboots to the dashboard; never returns */
void nx_launch_dashboard(void);
/* the display's buffer (640x480, A8R8G8B8), or NULL */
void *nx_framebuffer(void);
/* the display's vertical blank */
void nx_wait_for_vblank(void);
/* a pseudo-random word, seeded from the clock */
unsigned long nx_random(void);

#endif
