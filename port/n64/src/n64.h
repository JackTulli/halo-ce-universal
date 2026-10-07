/*
N64.H

The bridge between the two halves of the N64 platform layer. Units compiled
with the game's ABI and the XDK declarations (xdk_*.c, the game itself)
cannot include libdragon's headers (both declare the same names), so the
libdragon-facing units (n64_*.c) export what the game side needs through
these plain-C declarations.
*/

#ifndef __HALO_N64_H
#define __HALO_N64_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- system (n64_main.c) */

/* debug output, one line per call (no newline needed) */
void n64_log(const char *text);
/* a fatal error: shows the message on screen and stops */
void n64_fatal(const char *text) __attribute__((noreturn));
/* prints the call stack to the debug output */
void n64_backtrace(void);
/* the CPU's count register: 46.875 MHz, wraps every 91 s */
unsigned long n64_ticks(void);
/* the same counter in 64 bits, never wrapping */
unsigned long long n64_ticks64(void);
#define N64_TICKS_PER_SECOND 46875000
/* microseconds since boot, 64 bits */
unsigned long long n64_microseconds(void);
/* busy-waits */
void n64_wait_microseconds(unsigned long microseconds);
/* memory left on the heap, and the largest free block */
unsigned long n64_heap_free(void);

/* ---------- cartridge files (n64_files.c): DragonFS, read-only */

/* returns a handle (>= 0) or -1 */
long n64_file_open(const char *path);
long n64_file_size(long handle);
long n64_file_read(long handle, void *buffer, long size);
long n64_file_seek(long handle, long offset);
void n64_file_close(long handle);

/* ---------- controllers (n64_input.c) */

struct n64_pad
{
	unsigned char connected;
	unsigned char pad[3];
	signed char stick_x, stick_y;
	unsigned short buttons; /* N64_BUTTON_* */
};

enum
{
	N64_BUTTON_A = 1 << 0,
	N64_BUTTON_B = 1 << 1,
	N64_BUTTON_Z = 1 << 2,
	N64_BUTTON_START = 1 << 3,
	N64_BUTTON_D_UP = 1 << 4,
	N64_BUTTON_D_DOWN = 1 << 5,
	N64_BUTTON_D_LEFT = 1 << 6,
	N64_BUTTON_D_RIGHT = 1 << 7,
	N64_BUTTON_L = 1 << 8,
	N64_BUTTON_R = 1 << 9,
	N64_BUTTON_C_UP = 1 << 10,
	N64_BUTTON_C_DOWN = 1 << 11,
	N64_BUTTON_C_LEFT = 1 << 12,
	N64_BUTTON_C_RIGHT = 1 << 13,
};

void n64_input_poll(void);
void n64_input_get(int port, struct n64_pad *pad);

#ifdef __cplusplus
}
#endif

#endif /* __HALO_N64_H */
