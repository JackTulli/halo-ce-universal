/*
HALO_XBOX_PREFIX.H

Force-included ahead of every game unit and every Xbox-facing platform unit
in the Xbox build (clang -include); the nxdk-facing units
(port/xbox/src/nx_*.c) never see it. As in the Windows build, clang's
Microsoft target already gives the game the MSVC environment it was written
for (structure layout, 16-bit wchar_t, calling conventions, COMDAT inline
functions), so this is short.
*/

#ifndef __HALO_XBOX_PREFIX_H
#define __HALO_XBOX_PREFIX_H

#if !defined(_M_IX86)
#error the Xbox build targets 32-bit x86
#endif

#define HALO_XBOX 1
/* the native (non-MSVC) build of the game: the game sources use this for
the few places where clang and MSVC differ, as on Linux, Android and
Windows */
#define HALO_LINUX 1

/* ---------- XDK architecture selection */

#define _X86_ 1
#define _STDCALL_SUPPORTED 1
#define _USE_MATH_DEFINES

/* ---------- MSVC intrinsics

Clang predeclares the MSVC _Interlocked* builtins with prototypes that
conflict with the XDK's WINAPI declarations; route the XDK names to the
platform layer instead (port/xbox/src/xbox_kernel.c), as the other native
builds do. */

#define _InterlockedCompareExchange halo_linux_InterlockedCompareExchange
#define _InterlockedDecrement halo_linux_InterlockedDecrement
#define _InterlockedExchange halo_linux_InterlockedExchange
#define _InterlockedExchangeAdd halo_linux_InterlockedExchangeAdd
#define _InterlockedIncrement halo_linux_InterlockedIncrement

/* ---------- structured exception handling

Only the top-level crash handler in main() uses SEH; the guarded block
always runs and the handler is compiled out, as in the other native
builds. A fault brings the console down with the kernel's own report. */

#define __try if (1)
#define __except(filter) else if (0)
#define __finally
#define __leave

/* ---------- the console's limits (16 players, 4 machines, the retail pools) */

#include "halo_xbox_limits.h"

/* the Xbox Winsock headers' fd_set, for game units and the platform's
network layer alike */
#define FD_SETSIZE HALO_PORT_FD_SETSIZE

/* ---------- Winsock and source fixups shared with the Linux build */

#ifndef HALO_LINUX_PLATFORM_LAYER
#include "../../linux/include/halo_linux_winsock_names.h"
#include "../../linux/include/halo_linux_source_fixups.h"
#endif

#include <stddef.h>

#endif /* __HALO_XBOX_PREFIX_H */
