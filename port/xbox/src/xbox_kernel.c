/*
XBOX_KERNEL.C

The kernel and XAPI services of the Xbox platform layer that nxdk's Win32
subset does not provide: physical memory, asynchronous file I/O, the
global memory functions, interlocked operations, and the few Win32
functions whose nxdk versions need more than a calling-convention thunk
(build/xbox/win32_thunks.c has the rest).
*/

#include "platform.h"
#include "nx_win32.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- logging */

void platform_log(const char *format, ...)
{
	char text[512];
	va_list arguments;
	size_t length;

	strcpy(text, "halo-xbox: ");
	va_start(arguments, format);
	vsnprintf(text + 11, sizeof(text) - 13, format, arguments);
	va_end(arguments);
	length = strlen(text);
	text[length] = '\n';
	text[length + 1] = 0;
	nx_print(text);
}

void platform_unimplemented(const char *name)
{
	static const char *reported[256];
	static long reported_count = 0;
	long index;

	for (index = 0; index < reported_count; index++)
	{
		if (reported[index] == name)
			return;
	}
	if (reported_count < 256)
		reported[reported_count++] = name;
	platform_log("%s is not implemented on the Xbox", name);
}

/* ---------- the trace and the watchdog */

const char *volatile platform_last_call_name = "(nothing yet)";
volatile unsigned long platform_call_count;

void platform_trace(const char *what)
{
	platform_last_call_name = what;
	platform_call_count++;
}

const char *platform_last_call(void)
{
	return platform_last_call_name;
}

/* ---------- the clocks, which the game reads millions of times a second
(win32_names.txt: bridge-only, so no trace): nxdk's performance counter is
the processor's time stamp counter at a fixed 733,333,333 Hz */

DWORD WINAPI GetTickCount(void)
{
	return nxb_GetTickCount();
}

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *counter)
{
	counter->QuadPart = (LONGLONG)__builtin_ia32_rdtsc();
	return TRUE;
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
	frequency->QuadPart = 733333333;
	return TRUE;
}

static DWORD WINAPI watchdog_thread(LPVOID unused)
{
	unsigned long seconds = 0;

	(void)unused;
	for (;;)
	{
		nxb_Sleep(5000);
		seconds += 5;
		if (seconds <= 30)
		{
			/* the network's start: the link, then the address DHCP gave */
			extern int posix_network_link_up(void);
			extern unsigned long posix_local_ipv4_address(void);
			extern void nx_net_describe(char *text, unsigned long size);
			unsigned long ip = posix_local_ipv4_address();
			char netif_text[96];

			nx_net_describe(netif_text, sizeof(netif_text));
			platform_log("alive %lus: %lu calls, last %s; net link %d addr %lu.%lu.%lu.%lu %s", seconds, platform_call_count, platform_last_call_name,
				posix_network_link_up(), ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, (ip >> 24) & 0xff, netif_text);
		}
		else
			platform_log("alive %lus: %lu calls, last %s", seconds, platform_call_count, platform_last_call_name);
	}
	return 0;
}

void platform_watchdog_start(void)
{
	static BOOL started;

	if (!started)
	{
		started = TRUE;
		if (!CreateThread(NULL, 16384, watchdog_thread, NULL, 0, NULL))
			platform_log("cannot start the watchdog thread");
	}
}

/* ---------- paths

The disc's root entries, read once: a path on D: whose first component is
one of them is the disc's; any other is the cache partition's. */

#define MAXIMUM_DISC_ENTRIES 64

static char disc_entries[MAXIMUM_DISC_ENTRIES][32];
static int disc_entry_count = -1;

static void read_disc_entries(void)
{
	WIN32_FIND_DATAA entry;
	HANDLE find;

	disc_entry_count = 0;
	find = nxb_FindFirstFileA("D:\\*", &entry);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (disc_entry_count < MAXIMUM_DISC_ENTRIES && strlen(entry.cFileName) < sizeof(disc_entries[0]))
			strcpy(disc_entries[disc_entry_count++], entry.cFileName);
	} while (nxb_FindNextFileA(find, &entry));
	nx_find_close(find);
}

const char *platform_redirect_path(const char *path, char *buffer)
{
	const char *rest;
	unsigned long length;
	int index;

	if (!path)
		return path;
	/* the game's copies of the disc's compressed maps (z:\cacheNNN.map:
	up to six, each up to 300 MB) outgrow the 750 MB utility partition and
	leave no room for the saves next to them; they go to the data
	partition instead (E:\halo, made at start-up) */
	if ((path[0] == 'z' || path[0] == 'Z') && path[1] == ':' && (path[2] == '\\' || path[2] == '/') &&
		!_strnicmp(path + 3, "cache", 5) && strlen(path) >= 12 && !_stricmp(path + strlen(path) - 4, ".map") &&
		!strchr(path + 3, '\\') && !strchr(path + 3, '/'))
	{
		snprintf(buffer, MAX_PATH, "E:\\halo\\%s", path + 3);
		return buffer;
	}
	if ((path[0] != 'd' && path[0] != 'D') || path[1] != ':' || (path[2] != '\\' && path[2] != '/'))
		return path;
	if (disc_entry_count < 0)
		read_disc_entries();
	rest = path + 3;
	for (length = 0; rest[length] && rest[length] != '\\' && rest[length] != '/'; length++)
		;
	for (index = 0; index < disc_entry_count; index++)
	{
		if (strlen(disc_entries[index]) == length && !_strnicmp(disc_entries[index], rest, length))
			return path;
	}
	/* wildcards in the first component (the root listing itself) stay */
	if (memchr(rest, '*', length) || memchr(rest, '?', length))
		return path;
	snprintf(buffer, MAX_PATH, "Z:\\%s", rest);
	return buffer;
}

/* ---------- contiguous memory */

void *platform_contiguous_alloc(unsigned long size, unsigned long alignment, unsigned long physical_address,
	DWORD protect)
{
	return nx_physical_alloc(size, physical_address, alignment, protect);
}

void platform_contiguous_free(void *address)
{
	nx_physical_free(address);
}

BOOL platform_is_contiguous(const void *address)
{
	return (unsigned long)address >= PLATFORM_CONTIGUOUS_BASE &&
		(unsigned long)address < PLATFORM_CONTIGUOUS_BASE + PLATFORM_CONTIGUOUS_SIZE;
}

LPVOID WINAPI XPhysicalAlloc(SIZE_T size, ULONG_PTR physical_address, ULONG_PTR alignment, DWORD protect)
{
	PLATFORM_TRACE();
	platform_watchdog_start();
	void *memory = nx_physical_alloc(size, physical_address, alignment, protect);

	if (!memory)
		platform_log("XPhysicalAlloc(%lu bytes at %08lx) failed", size, physical_address);
	return memory;
}

VOID WINAPI XPhysicalFree(LPVOID address)
{
	nx_physical_free(address);
}

VOID WINAPI XPhysicalProtect(LPVOID address, SIZE_T size, DWORD new_protect)
{
	nx_set_protect(address, size, new_protect);
}

DWORD WINAPI XQueryMemoryProtect(LPVOID address)
{
	return nx_query_protect(address);
}

BOOL WINAPI VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
	return nx_virtual_protect(address, size, new_protect, old_protect);
}

/* ---------- handles

nxdk's FindFirstFileA returns a pointer to a record of its own, which
FindClose frees: CloseHandle must tell those apart from the kernel's
handles. */

#define MAXIMUM_FIND_HANDLES 32

static HANDLE find_handles[MAXIMUM_FIND_HANDLES];

HANDLE WINAPI FindFirstFileA(LPCSTR file_name, LPWIN32_FIND_DATAA data)
{
	PLATFORM_TRACE();
	char redirected[MAX_PATH];
	HANDLE handle = nxb_FindFirstFileA(platform_redirect_path(file_name, redirected), data);
	int index;

	if (handle != INVALID_HANDLE_VALUE)
	{
		for (index = 0; index < MAXIMUM_FIND_HANDLES; index++)
		{
			if (!find_handles[index])
			{
				find_handles[index] = handle;
				break;
			}
		}
	}
	return handle;
}

/* ---------- files

The saves (U:, T:) are written through to the disk when their handles
close: the emulator's disk image keeps newly written clusters unlinked
until the guest flushes, and an emulator crash would lose them. */

#define MAXIMUM_SAVE_HANDLES 16

static HANDLE save_handles[MAXIMUM_SAVE_HANDLES];

static BOOL is_save_path(const char *path)
{
	if (!path || path[1] != ':')
		return FALSE;
	if (path[0] == 'u' || path[0] == 'U' || path[0] == 't' || path[0] == 'T')
		return TRUE;
	/* the profiles' map file and the default profiles on the utility partition */
	return (path[0] == 'z' || path[0] == 'Z') && (path[2] == '\\' || path[2] == '/') && !_strnicmp(path + 3, "saved", 5);
}

static BOOL is_save_handle(HANDLE handle)
{
	int index;

	for (index = 0; index < MAXIMUM_SAVE_HANDLES; index++)
		if (save_handles[index] == handle && handle)
			return TRUE;
	return FALSE;
}

HANDLE WINAPI CreateFileA(LPCSTR file_name, DWORD desired_access, DWORD share_mode, LPSECURITY_ATTRIBUTES security,
	DWORD creation_disposition, DWORD flags_and_attributes, HANDLE template_file)
{
	PLATFORM_TRACE();
	char redirected[MAX_PATH];
	HANDLE handle;

	file_name = platform_redirect_path(file_name, redirected);
	handle = nxb_CreateFileA(file_name, desired_access, share_mode, security, creation_disposition,
		flags_and_attributes, template_file);
	if (handle != INVALID_HANDLE_VALUE && (desired_access & GENERIC_WRITE) && is_save_path(file_name))
	{
		int index;

		for (index = 0; index < MAXIMUM_SAVE_HANDLES; index++)
		{
			if (!save_handles[index])
			{
				save_handles[index] = handle;
				break;
			}
		}
	}
	return handle;
}

BOOL WINAPI CloseHandle(HANDLE handle)
{
	PLATFORM_TRACE();
	int index;

	for (index = 0; index < MAXIMUM_FIND_HANDLES; index++)
	{
		if (find_handles[index] == handle && handle)
		{
			find_handles[index] = NULL;
			return nx_find_close(handle);
		}
	}
	for (index = 0; index < MAXIMUM_SAVE_HANDLES; index++)
	{
		if (save_handles[index] == handle && handle)
		{
			save_handles[index] = NULL;
			nx_flush_file(handle);
			break;
		}
	}
	return nxb_CloseHandle(handle);
}

/* a save reaches the disk image with every write, not only when its handle
closes: the game keeps a profile open while it plays, and the console's
cache held a profile the user had just made until a reset of the emulated
console threw it away */
BOOL WINAPI WriteFile(HANDLE handle, LPCVOID buffer, DWORD count, LPDWORD written, LPOVERLAPPED overlapped)
{
	PLATFORM_TRACE();
	BOOL success = nxb_WriteFile(handle, buffer, count, written, overlapped);

	if (success && !overlapped && is_save_handle(handle))
		nx_flush_file(handle);
	return success;
}

/* ---------- threads */

HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES attributes, DWORD stack_size, LPTHREAD_START_ROUTINE routine,
	LPVOID parameter, DWORD flags, LPDWORD thread_id)
{
	PLATFORM_TRACE();
	(void)attributes;
	return nx_create_thread(stack_size, routine, parameter, flags, thread_id);
}

/* ---------- alertable waits, which run the completion routines of the
thread's asynchronous reads and writes (nx_kernel.c) */

DWORD WINAPI SleepEx(DWORD milliseconds, BOOL alertable)
{
	PLATFORM_TRACE();
	return nx_sleep_ex(milliseconds, alertable);
}

DWORD WINAPI WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable)
{
	PLATFORM_TRACE();
	return nx_wait_for_single_object_ex(handle, milliseconds, alertable);
}

/* ---------- asynchronous file I/O */

/* A buffer the console cannot have: above its 64 MB physical window and
below the kernel's own regions. The kernel's drivers touch a request's buffer
at dispatch level, where a bad one is a bugcheck with no report, so such a
request fails here instead, with its callers. */
static BOOL buffer_is_impossible(const char *what, const void *buffer, DWORD count)
{
	unsigned long start = (unsigned long)buffer, end = start + count;

	if ((start >= 0x84000000UL && start < 0xD0000000UL) || (end > 0x84000000UL && end <= 0xD0000000UL) ||
		end < start)
	{
		platform_log("%s: impossible buffer %p (%lu bytes) from %p %p %p", what, buffer, count,
			__builtin_return_address(1), __builtin_return_address(2), __builtin_return_address(3));
		SetLastError(ERROR_INVALID_PARAMETER);
		return TRUE;
	}
	return FALSE;
}

BOOL WINAPI ReadFileEx(HANDLE handle, LPVOID buffer, DWORD count, LPOVERLAPPED overlapped,
	LPOVERLAPPED_COMPLETION_ROUTINE completion)
{
	PLATFORM_TRACE();
	{
		static int reports;

		if (reports < 40)
		{
			reports++;
			platform_log("ReadFileEx(%p, %lu bytes at %lu into %p)", handle, count,
				overlapped ? overlapped->Offset : 0, buffer);
		}
	}
	if (buffer_is_impossible("ReadFileEx", buffer, count))
		return FALSE;
	return nx_read_file_ex(handle, buffer, count, overlapped, (void *)completion);
}

BOOL WINAPI WriteFileEx(HANDLE handle, LPCVOID buffer, DWORD count, LPOVERLAPPED overlapped,
	LPOVERLAPPED_COMPLETION_ROUTINE completion)
{
	PLATFORM_TRACE();
	if (buffer_is_impossible("WriteFileEx", buffer, count))
		return FALSE;
	return nx_write_file_ex(handle, buffer, count, overlapped, (void *)completion, is_save_handle(handle));
}

/* ---------- interlocked operations */

LONG WINAPI halo_linux_InterlockedIncrement(LPLONG addend)
{
	return __atomic_add_fetch(addend, 1, __ATOMIC_SEQ_CST);
}

LONG WINAPI halo_linux_InterlockedDecrement(LPLONG addend)
{
	return __atomic_sub_fetch(addend, 1, __ATOMIC_SEQ_CST);
}

LONG WINAPI halo_linux_InterlockedExchange(LPLONG target, LONG value)
{
	return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

LONG WINAPI halo_linux_InterlockedExchangeAdd(LPLONG addend, LONG value)
{
	return __atomic_fetch_add(addend, value, __ATOMIC_SEQ_CST);
}

LONG WINAPI halo_linux_InterlockedCompareExchange(LPLONG destination, LONG exchange, LONG comparand)
{
	__atomic_compare_exchange_n(destination, &comparand, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return comparand;
}

/* ---------- global memory (the game's system_calloc and system_realloc) */

struct global_block
{
	SIZE_T size;
	SIZE_T padding[3];
};

HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T size)
{
	struct global_block *block = malloc(sizeof(*block) + size);

	/* the console's memory is scarce: the large allocations and who made
	them (tools/xbox_symbolize.py names the address) */
	if (size >= 0x40000)
	{
		/* the callers behind system_malloc and debug_malloc */
		platform_log("GlobalAlloc(%lu KB) from %p %p %p", size / 1024, __builtin_return_address(0),
			__builtin_return_address(1), __builtin_return_address(2));
	}

	if (!block)
	{
		platform_log("GlobalAlloc(%lu bytes) failed", size);
		nx_report_memory("at the failure");
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	block->size = size;
	if (flags & GMEM_ZEROINIT)
		memset(block + 1, 0, size);
	return block + 1;
}

static struct global_block *global_block_from_pointer(void *memory)
{
	return memory ? (struct global_block *)memory - 1 : NULL;
}

HGLOBAL WINAPI GlobalReAlloc(HGLOBAL memory, SIZE_T size, UINT flags)
{
	struct global_block *block = global_block_from_pointer(memory);
	SIZE_T old_size = block ? block->size : 0;

	block = realloc(block, sizeof(*block) + size);
	if (!block)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	block->size = size;
	if ((flags & GMEM_ZEROINIT) && size > old_size)
		memset((char *)(block + 1) + old_size, 0, size - old_size);
	return block + 1;
}

HLOCAL WINAPI LocalFree(HLOCAL memory)
{
	free(global_block_from_pointer(memory));
	return NULL;
}

SIZE_T WINAPI LocalSize(HLOCAL memory)
{
	struct global_block *block = global_block_from_pointer(memory);

	return block ? block->size : 0;
}

VOID WINAPI GlobalMemoryStatus(LPMEMORYSTATUS status)
{
	unsigned long total, available;

	nx_memory_status(&total, &available);
	memset(status, 0, sizeof(*status));
	status->dwLength = sizeof(*status);
	status->dwTotalPhys = total;
	status->dwAvailPhys = available;
	status->dwMemoryLoad = total ? (DWORD)(100 - available * 100 / total) : 0;
	status->dwTotalVirtual = 0x7ffe0000;
	status->dwAvailVirtual = available;
}

/* ---------- the system */

DWORD WINAPI XGetLaunchInfo(PDWORD launch_data_type, PLAUNCH_DATA launch_data)
{
	PLATFORM_TRACE();
	unsigned long type;
	unsigned char data[3072];

	nx_launch_info(&type, data);
	if (launch_data_type)
		*launch_data_type = type;
	if (launch_data)
		memcpy(launch_data, data, sizeof(data));
	return ERROR_SUCCESS;
}

DWORD WINAPI XLaunchNewImageA(LPCSTR image_path, PLAUNCH_DATA launch_data)
{
	(void)launch_data;
	platform_log("XLaunchNewImage(\"%s\"): rebooting", image_path ? image_path : "(dashboard)");
	nx_launch_dashboard();
	return ERROR_SUCCESS;
}

DWORD WINAPI XGetLanguage(void)
{
	PLATFORM_TRACE();
	return nx_language();
}
