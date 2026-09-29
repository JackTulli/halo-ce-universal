/*
PLATFORM.H

Shared internals of the Xbox platform layer (port/xbox/src), which
implements the parts of the Xbox SDK the game calls that nxdk does not
provide: Direct3D 8, DirectSound, XNet, the XAPI's memory, saves, input
and system calls. Files including this header are compiled with the
game's ABI and see the XDK declarations (port/include/xdk), so every
definition is type-checked against the SDK prototype it replaces; the
kernel and nxdk's own libraries are reached through the bridge in nx.h,
whose units (nx_*.c) are compiled against nxdk's headers instead, since
those and the XDK's declare the same names.

port/linux/src/platform.h defers to this header under HALO_XBOX, so the
Linux platform units the Xbox build compiles again see it too.
*/

#ifndef __HALO_XBOX_PLATFORM_H
#define __HALO_XBOX_PLATFORM_H

/* as cseries_windows.h does, so xtl.h also declares the debug keyboard;
the XDK's Winsock names are read under the private names game code sees
them by (port/linux/include/halo_linux_winsock_names.h), which the network
layer (port/linux/src/xnet.c) defines */
#define DEBUG_KEYBOARD
#include "../../linux/include/halo_linux_winsock_names.h"
#include <xtl.h>
#include <xbdm.h>
#include <d3d8perf.h>
#define HALO_LINUX_WINSOCK_NAMES_UNDEFINE
#include "../../linux/include/halo_linux_winsock_names.h"
#undef HALO_LINUX_WINSOCK_NAMES_UNDEFINE

#include "nx.h"

/* ---------- logging */

/* prints "halo-xbox: <message>" to the kernel's debug output and the
screen's text console */
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* performance mode (xbox_display.c): a performance.txt on the disc */
int halo_performance_mode(void);

/* the device (d3d8_nv2a.c): columns everything the game draws on the
screen shifts right by (the menus, laid out for 640 columns, centred on a
wider screen) */
void d3d8_nv2a_set_ui_offset(long columns);

/* reports an SDK entry point the Xbox build does not implement, once per
name; the caller then behaves as a failing or empty call */
void platform_unimplemented(const char *name);

#define PLATFORM_UNIMPLEMENTED() platform_unimplemented(__func__)

/* the last SDK entry point the game called, which the watchdog thread
(xbox_kernel.c) reports every few seconds while the console is brought up;
what is a string literal that stays */
void platform_trace(const char *what);
extern const char *volatile platform_last_call_name;
extern volatile unsigned long platform_call_count;
#define PLATFORM_TRACE() (platform_last_call_name = __func__, platform_call_count++)
void platform_watchdog_start(void);

/* ---------- paths

The January build ran from a development kit, where D: was a writable
folder; on a console D: is the disc. Paths on D: that name something the
disc carries (its maps, its XBE) stay; the rest -- the checkpoints the game
keeps in d:\core, its text logs, its saved core files -- go to the cache
partition, Z:, reads and writes alike. Returns path, or buffer (MAX_PATH)
holding the translation. */
const char *platform_redirect_path(const char *path, char *buffer);

/* ---------- contiguous ("physical") memory

The Xbox maps physical memory at virtual 0x80000000 + P; here that is the
real thing, and the kernel hands out the blocks (MmAllocateContiguousMemoryEx,
nx_kernel.c). The arithmetic the Linux units use keeps working. */

#define PLATFORM_CONTIGUOUS_BASE 0x80000000UL
#define PLATFORM_CONTIGUOUS_SIZE 0x04000000UL /* the retail console's 64 MB */
#define PLATFORM_ANY_PHYSICAL_ADDRESS 0xffffffffUL

/* returns NULL on failure; physical_address places the block exactly */
void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
	unsigned long physical_address, DWORD protect);
void platform_contiguous_free(void *address);
BOOL platform_is_contiguous(const void *address);
#define PLATFORM_PHYSICAL_TO_VIRTUAL(physical) ((void *)((unsigned long)(physical) | PLATFORM_CONTIGUOUS_BASE))
#define PLATFORM_VIRTUAL_TO_PHYSICAL(address) ((unsigned long)(address) & ~PLATFORM_CONTIGUOUS_BASE)

/* ---------- the device's surfaces (port/linux/src/d3d8_resources.c) */

void d3d8_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);

#endif
