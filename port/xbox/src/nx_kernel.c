/*
NX_KERNEL.C

The nxdk side of the Xbox platform layer (nx.h): the kernel services and
nxdk's hardware library, behind plain C types. Compiled against nxdk's
headers, never the XDK declarations.
*/

#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <hal/debug.h>
#include <hal/video.h>
#include <nxdk/mount.h>
#include <nxdk/net.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx.h"

/* Halo's title identifier (MS-004): where the console keeps its saves */
#define TITLE_ID_TEXT "4d530004"

/* ---------- the log over the network

Each console line also goes by UDP to the machine beside an emulated
console (QEMU's user network reaches the host at 10.0.2.2), where a
listener keeps the log; on a real console with a network, to the same
address on its network, which nothing may answer. */

#define LOG_HOST "10.0.2.2"
#define LOG_PORT 9999

static int log_socket = -1;
static struct sockaddr_in log_address;

/* every line also goes into a ring in contiguous memory, at any IRQL and
without a lock, for reading out of a stopped emulator (pmemsave at the
physical address the start-up log gives): the last words of a crash that
the network could not carry */
#define LOG_RING_BYTES 16384
#define LOG_RING_HEADER 16

static char *log_ring;

static void log_ring_start(void)
{
	log_ring = MmAllocateContiguousMemoryEx(LOG_RING_BYTES, 0, 0x03FFFFFF, 4096, PAGE_READWRITE);
	if (!log_ring)
		return;
	memset(log_ring, 0, LOG_RING_BYTES);
	memcpy(log_ring, "NXLG", 4);
}

static void log_ring_append(const char *text)
{
	unsigned long used, position;

	if (!log_ring)
		return;
	used = *(unsigned long *)(log_ring + 4);
	position = used % (LOG_RING_BYTES - LOG_RING_HEADER);
	while (*text)
	{
		log_ring[LOG_RING_HEADER + position] = *text++;
		used++;
		if (++position == LOG_RING_BYTES - LOG_RING_HEADER)
			position = 0;
	}
	*(unsigned long *)(log_ring + 4) = used;
}

unsigned long nx_log_ring_physical(void)
{
	return log_ring ? (unsigned long)log_ring & 0x03FFFFFFUL : 0;
}

/* The network log goes out from a thread of its own, every 50 ms, from the
ring: a send in every print cost the printing thread (often the game's) the
network stack's lock, and deadlocked a thread that printed while it held
that lock (an nxdk assertion inside lwIP). Lines are sent whole where they
fit a datagram. */
static unsigned long log_sent;

static void log_send_pending(void)
{
	const unsigned long size = LOG_RING_BYTES - LOG_RING_HEADER;
	unsigned long used = *(volatile unsigned long *)(log_ring + 4);

	if (used - log_sent > size)
		log_sent = used - size; /* overwritten before it went */
	while (log_sent != used)
	{
		char chunk[1400];
		unsigned long count = 0, start = log_sent;

		while (log_sent != used && count < sizeof(chunk))
			chunk[count++] = log_ring[LOG_RING_HEADER + log_sent++ % size];
		if (count == sizeof(chunk) && log_sent != used)
		{
			/* full: end it at its last line, and start there next */
			unsigned long end = count;

			while (end > 0 && chunk[end - 1] != '\n')
				end--;
			if (end > 0)
			{
				count = end;
				log_sent = start + end;
			}
		}
		lwip_sendto(log_socket, chunk, count, 0, (struct sockaddr *)&log_address, sizeof(log_address));
	}
}

static unsigned long __stdcall log_thread(void *parameter)
{
	(void)parameter;
	for (;;)
	{
		Sleep(50);
		log_send_pending();
	}
	return 0;
}

static void log_start(void)
{
	log_ring_start();
	if (nxNetInit(NULL) != 0)
	{
		debugPrint("halo-xbox: no network; the log stays on the screen\n");
		return;
	}
	log_socket = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (log_socket < 0)
		return;
	memset(&log_address, 0, sizeof(log_address));
	log_address.sin_family = AF_INET;
	log_address.sin_port = htons(LOG_PORT);
	log_address.sin_addr.s_addr = inet_addr(LOG_HOST);
	debugPrint("halo-xbox: logging to " LOG_HOST ":%d\n", LOG_PORT);
	if (log_ring)
	{
		/* above the game, so a busy game thread does not hold the log back */
		HANDLE thread = nx_create_thread(16 * 1024, log_thread, NULL, 0, NULL);

		if (thread)
			SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
	}
	{
		char text[80];

		snprintf(text, sizeof(text), "halo-xbox: log ring at physical %08lx\n", nx_log_ring_physical());
		nx_print(text);
		snprintf(text, sizeof(text), "halo-xbox: main thread %p stack %p..%p\n", (void *)KeGetCurrentThread(),
			KeGetCurrentThread()->StackLimit, KeGetCurrentThread()->StackBase);
		nx_print(text);
	}
}

/* ---------- start-up */

static int started = 0;

/* nxdk reboots the console when the game exits (a failed start-up, or the
game's own exit): the log stays on the screen a while first */
static void exit_notice(void)
{
	nx_gpu_show_console();
	nx_print("halo-xbox: the game has exited; the console restarts in 30 seconds\n");
	nx_report_memory("at exit");
	Sleep(30000);
}

static void mount(char letter, const char *path)
{
	if (!nxIsDriveMounted(letter) && !nxMountDrive(letter, path))
		debugPrint("halo-xbox: cannot mount %c: as %s\n", letter, path);
}

static void clear_cached_maps(const char *directory);

/* writes a pattern to a file, reads it back through a fresh handle and
compares: the saves and the cache go through these calls */
static void file_self_test(const char *path)
{
	static unsigned char pattern[4096], readback[4096];
	HANDLE file;
	DWORD done = 0;
	unsigned long i;
	char text[128];
	const char *verdict = "ok";

	for (i = 0; i < sizeof(pattern); i++)
		pattern[i] = (unsigned char)(i * 7 + (i >> 8));
	file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		verdict = "cannot create";
	else
	{
		if (!WriteFile(file, pattern, sizeof(pattern), &done, NULL) || done != sizeof(pattern))
			verdict = "write failed";
		CloseHandle(file);
		file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
			verdict = "cannot reopen";
		else
		{
			memset(readback, 0, sizeof(readback));
			if (!ReadFile(file, readback, sizeof(readback), &done, NULL) || done != sizeof(readback))
				verdict = "read failed";
			else if (memcmp(pattern, readback, sizeof(pattern)) != 0)
			{
				for (i = 0; i < sizeof(pattern) && pattern[i] == readback[i]; i++)
					;
				snprintf(text, sizeof(text), "halo-xbox: file self-test %s: MISMATCH from byte %lu (%02x, read %02x)\n",
					path, i, pattern[i], readback[i]);
				nx_print(text);
				verdict = NULL;
			}
			CloseHandle(file);
		}
		DeleteFileA(path);
	}
	if (verdict)
	{
		snprintf(text, sizeof(text), "halo-xbox: file self-test %s: %s (%lu bytes)\n", path, verdict, (unsigned long)done);
		nx_print(text);
	}
}

void nx_startup(void)
{
	if (started)
		return;
	started = 1;
	/* the game's format: the display's buffer is also its back buffer (d3d8_null.c) */
	XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
	/* the dashboard's mode (widescreen: 720 columns) now, before the game
	asks how wide its screen is */
	nx_video_configure();
	debugClearScreen();
	debugPrint("halo-xbox: starting\n");
	/* the console's drives, as the XAPI mounts them for a title: the
	saves (U:) and title data (T:) under its identifier on the data
	partition, the utility partition as Z: (the game's cache) */
	mount('E', "\\Device\\Harddisk0\\Partition1");
	CreateDirectoryA("E:\\UDATA", NULL);
	CreateDirectoryA("E:\\UDATA\\" TITLE_ID_TEXT, NULL);
	CreateDirectoryA("E:\\TDATA", NULL);
	CreateDirectoryA("E:\\TDATA\\" TITLE_ID_TEXT, NULL);
	CreateDirectoryA("E:\\halo", NULL); /* the map cache (platform_redirect_path) */
	mount('U', "\\Device\\Harddisk0\\Partition1\\UDATA\\" TITLE_ID_TEXT);
	mount('T', "\\Device\\Harddisk0\\Partition1\\TDATA\\" TITLE_ID_TEXT);
	mount('Z', "\\Device\\Harddisk0\\Partition3");
	/* the game's copies of the maps on the cache partition: one that a
	stopped run left half-written would fail the game's checks, so they
	start over each boot; the saved games and profiles there stay */
	log_start();
	clear_cached_maps("Z:\\"); /* copies earlier builds left on the utility partition */
	clear_cached_maps("E:\\halo\\");
	file_self_test("U:\\selftest.bin");
	file_self_test("Z:\\selftest.bin");
	atexit(exit_notice);
}

/* deletes the cache*.map files of a directory (the path ends with a
backslash): the root is listed whole and the names matched here, since
the kernel's own name masks are not relied on */
static void clear_cached_maps(const char *directory)
{
	char pattern[MAX_PATH];
	WIN32_FIND_DATAA entry;
	HANDLE find;
	int count = 0, failed = 0;
	unsigned long last_error = 0;
	char text[128];

	snprintf(pattern, sizeof(pattern), "%s*", directory);
	find = FindFirstFileA(pattern, &entry);
	if (find == INVALID_HANDLE_VALUE)
	{
		last_error = GetLastError();
		if (last_error != ERROR_FILE_NOT_FOUND && last_error != ERROR_NO_MORE_FILES) /* an empty directory */
		{
			snprintf(text, sizeof(text), "halo-xbox: cannot list %s (%lu)\n", directory, last_error);
			nx_print(text);
		}
		return;
	}
	do
	{
		char path[MAX_PATH];
		size_t length = strlen(entry.cFileName);

		if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || length < 10 ||
			_strnicmp(entry.cFileName, "cache", 5) || _stricmp(entry.cFileName + length - 4, ".map"))
			continue;
		snprintf(path, sizeof(path), "%s%s", directory, entry.cFileName);
		SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
		if (DeleteFileA(path))
			count++;
		else
		{
			failed++;
			last_error = GetLastError();
		}
	} while (FindNextFileA(find, &entry));
	FindClose(find);
	snprintf(text, sizeof(text), "halo-xbox: %d cached map(s) cleared from %s, %d not (%lu)\n", count, directory, failed,
		last_error);
	nx_print(text);
}

/* before main: nxdk runs these from its start-up thread, after its own
(which mounts D:, the disc or the folder the XBE runs from) */
__attribute__((constructor)) static void nx_startup_constructor(void)
{
	nx_startup();
}

/* once the GPU shows the game, nxdk's text screen is hidden: drawing each
line on it glyph by glyph (and clearing all 1.2 MB of it every page) and
the kernel's debug print trap cost the game's thread for nothing. The
lines still go to the ring and the network; a crash that shows the screen
again replays the ring's last lines on it (nx_print_screen_shown) */
static volatile int screen_hidden;

void nx_print_screen_hidden(void)
{
	screen_hidden = 1;
}

void nx_print_screen_shown(void)
{
	enum { REPLAY_BYTES = 1600 };
	char text[REPLAY_BYTES + 1];
	unsigned long used, start, index, length = 0;

	screen_hidden = 0;
	if (!log_ring)
		return;
	used = *(unsigned long *)(log_ring + 4);
	start = used > REPLAY_BYTES ? used - REPLAY_BYTES : 0;
	for (index = start; index < used; index++)
		text[length++] = log_ring[LOG_RING_HEADER + index % (LOG_RING_BYTES - LOG_RING_HEADER)];
	text[length] = 0;
	/* from the first whole line */
	{
		char *first = start ? strchr(text, '\n') : text;

		debugPrint("%s", first ? first + (start ? 1 : 0) : text);
	}
}

void nx_print(const char *text)
{
	log_ring_append(text);
	if (!screen_hidden)
	{
		DbgPrint("%s", text);
		debugPrint("%s", text);
	}
	/* without the ring (no memory for it), directly, as before; not from
	a raised IRQL, where the stack would wait on itself */
	if (!log_ring && log_socket >= 0 && KeGetCurrentIrql() == PASSIVE_LEVEL)
		lwip_sendto(log_socket, text, strlen(text), 0, (struct sockaddr *)&log_address, sizeof(log_address));
}

/* ---------- memory */

void nx_report_memory(const char *when)
{
	MM_STATISTICS statistics;
	char text[256];

	memset(&statistics, 0, sizeof(statistics));
	statistics.Length = sizeof(statistics);
	MmQueryStatistics(&statistics);
	snprintf(text, sizeof(text),
		"halo-xbox: memory %s: %lu of %lu pages free; KB committed %lu, cache %lu, pool %lu, stacks %lu, image %lu\n",
		when, statistics.AvailablePages, statistics.TotalPhysicalPages,
		statistics.VirtualMemoryBytesCommitted / 1024, statistics.CachePagesCommitted * 4,
		statistics.PoolPagesCommitted * 4, statistics.StackPagesCommitted * 4, statistics.ImagePagesCommitted * 4);
	nx_print(text);
}

void *nx_physical_alloc(unsigned long size, unsigned long physical_address, unsigned long alignment,
	unsigned long protect)
{
	ULONG_PTR lowest = 0, highest = 0x03FFFFFF;

	void *memory;

	if (physical_address != 0xffffffffUL)
	{
		lowest = physical_address;
		highest = physical_address + size - 1;
	}
	/* the kernel allocates pages: an alignment below a page (Direct3D's 128
	bytes) is a page's, and a power of two */
	if (alignment < 4096)
		alignment = 4096;
	memory = MmAllocateContiguousMemoryEx(size, lowest, highest, alignment, protect);
	if (size >= 0x100000)
	{
		char text[128];

		snprintf(text, sizeof(text), "halo-xbox: %lu KB of contiguous memory at %p\n", size / 1024, memory);
		nx_print(text);
		nx_report_memory("now");
	}
	if (!memory)
	{
		char text[160];

		snprintf(text, sizeof(text), "halo-xbox: MmAllocateContiguousMemoryEx(%lu bytes, %08lx-%08lx, align %lu, protect %lx) failed\n",
			size, lowest, highest, alignment, protect);
		nx_print(text);
		nx_report_memory("now");
	}
	return memory;
}

void nx_physical_free(void *address)
{
	if (address)
		MmFreeContiguousMemory(address);
}

unsigned long nx_query_protect(const void *address)
{
	return MmQueryAddressProtect((PVOID)address);
}

void nx_set_protect(void *address, unsigned long size, unsigned long protect)
{
	MmSetAddressProtect(address, size, protect);
}

int nx_virtual_protect(void *address, unsigned long size, unsigned long protect, unsigned long *old_protect)
{
	PVOID base = address;
	SIZE_T region = size;
	ULONG old = 0;
	NTSTATUS status = NtProtectVirtualMemory(&base, &region, protect, &old);

	if (old_protect)
		*old_protect = old;
	return NT_SUCCESS(status);
}

void nx_memory_status(unsigned long *total_bytes, unsigned long *available_bytes)
{
	MM_STATISTICS statistics;

	memset(&statistics, 0, sizeof(statistics));
	statistics.Length = sizeof(statistics);
	MmQueryStatistics(&statistics);
	*total_bytes = statistics.TotalPhysicalPages * 4096UL;
	*available_bytes = statistics.AvailablePages * 4096UL;
}

/* ---------- asynchronous file I/O

An OVERLAPPED starts with the IO_STATUS_BLOCK the kernel fills (Internal,
InternalHigh), then the offset. Each request gets an event the kernel
signals when it completes; the requesting thread's next alertable wait
runs the completion routines of its finished requests, as the XAPI's did
(the kernel queues those as APCs of the caller's mode, which the wait
functions here do not rely on). */

struct overlapped_layout
{
	IO_STATUS_BLOCK status;
	unsigned long offset;
	unsigned long offset_high;
	void *event;
};

typedef void (__stdcall *completion_routine)(unsigned long error, unsigned long transferred, void *overlapped);

#define MAXIMUM_PENDING_REQUESTS 64

struct pending_request
{
	int used;
	PKTHREAD thread;
	HANDLE event;
	struct overlapped_layout *overlapped;
	completion_routine routine;
	unsigned long started;
	HANDLE flush_file; /* flushed before the completion runs, or NULL */
};

static struct pending_request pending_requests[MAXIMUM_PENDING_REQUESTS];
static RTL_CRITICAL_SECTION pending_lock;
static int pending_lock_ready;
static int io_reports;

static void pending_lock_enter(void)
{
	if (!pending_lock_ready)
	{
		RtlInitializeCriticalSection(&pending_lock);
		pending_lock_ready = 1;
	}
	RtlEnterCriticalSection(&pending_lock);
}

static void pending_lock_leave(void)
{
	RtlLeaveCriticalSection(&pending_lock);
}

static struct pending_request *pending_request_new(struct overlapped_layout *overlapped, completion_routine routine,
	HANDLE flush_file)
{
	int index;

	pending_lock_enter();
	for (index = 0; index < MAXIMUM_PENDING_REQUESTS; index++)
	{
		struct pending_request *request = &pending_requests[index];

		if (!request->used)
		{
			if (!request->event)
				request->event = CreateEventA(NULL, TRUE, FALSE, NULL);
			if (!request->event)
				break;
			ResetEvent(request->event);
			request->used = 1;
			request->thread = KeGetCurrentThread();
			request->overlapped = overlapped;
			request->routine = routine;
			request->flush_file = flush_file;
			request->started = GetTickCount();
			pending_lock_leave();
			return request;
		}
	}
	pending_lock_leave();
	return NULL;
}

static int io_started(struct pending_request *request, NTSTATUS status)
{
	if (io_reports < 4)
	{
		io_reports++;
		debugPrint("halo-xbox: NtReadFile/NtWriteFile: status %08lx\n", (unsigned long)status);
	}
	if (NT_SUCCESS(status) || status == STATUS_PENDING)
		return 1;
	pending_lock_enter();
	request->used = 0;
	pending_lock_leave();
	SetLastError(RtlNtStatusToDosError(status));
	return 0;
}

int nx_read_file_ex(void *file, void *buffer, unsigned long size, void *overlapped, void *completion)
{
	struct overlapped_layout *layout = overlapped;
	struct pending_request *request = pending_request_new(layout, (completion_routine)completion, NULL);
	LARGE_INTEGER offset;

	if (!request)
	{
		SetLastError(ERROR_NO_SYSTEM_RESOURCES);
		return 0;
	}
	layout->status.Status = STATUS_PENDING;
	layout->status.Information = 0;
	offset.LowPart = layout->offset;
	offset.HighPart = (LONG)layout->offset_high;
	return io_started(request, NtReadFile(file, request->event, NULL, NULL, &layout->status, buffer, size, &offset));
}

int nx_write_file_ex(void *file, const void *buffer, unsigned long size, void *overlapped, void *completion,
	int flush_on_completion)
{
	struct overlapped_layout *layout = overlapped;
	struct pending_request *request = pending_request_new(layout, (completion_routine)completion,
		flush_on_completion ? file : NULL);
	LARGE_INTEGER offset;

	if (!request)
	{
		SetLastError(ERROR_NO_SYSTEM_RESOURCES);
		return 0;
	}
	layout->status.Status = STATUS_PENDING;
	layout->status.Information = 0;
	offset.LowPart = layout->offset;
	offset.HighPart = (LONG)layout->offset_high;
	return io_started(request, NtWriteFile(file, request->event, NULL, NULL, &layout->status, (PVOID)buffer, size, &offset));
}

/* runs the completion routines of this thread's finished requests;
returns how many ran */
static int run_completions(void)
{
	PKTHREAD thread = KeGetCurrentThread();
	int ran = 0;
	int index;

	for (index = 0; index < MAXIMUM_PENDING_REQUESTS; index++)
	{
		struct pending_request *request = &pending_requests[index];
		struct overlapped_layout *overlapped;
		completion_routine routine;
		HANDLE flush_file;
		LARGE_INTEGER zero;

		pending_lock_enter();
		zero.QuadPart = 0;
		if (!request->used || request->thread != thread ||
			NtWaitForSingleObject(request->event, FALSE, &zero) != STATUS_SUCCESS)
		{
			pending_lock_leave();
			continue;
		}
		overlapped = request->overlapped;
		routine = request->routine;
		flush_file = request->flush_file;
		request->used = 0;
		pending_lock_leave();
		if (flush_file && NT_SUCCESS(overlapped->status.Status))
			nx_flush_file(flush_file);
		if (io_reports < 8)
		{
			io_reports++;
			debugPrint("halo-xbox: completion: status %08lx, %lu bytes\n",
				(unsigned long)overlapped->status.Status, (unsigned long)overlapped->status.Information);
		}
		routine(NT_SUCCESS(overlapped->status.Status) ? 0 : RtlNtStatusToDosError(overlapped->status.Status),
			(unsigned long)overlapped->status.Information, overlapped);
		ran++;
	}
	return ran;
}

/* this thread's pending requests' events, for a wait; returns their count */
static int pending_events(HANDLE *events, int maximum_count)
{
	PKTHREAD thread = KeGetCurrentThread();
	int count = 0;
	int index;

	pending_lock_enter();
	for (index = 0; index < MAXIMUM_PENDING_REQUESTS && count < maximum_count; index++)
	{
		if (pending_requests[index].used && pending_requests[index].thread == thread)
		{
			events[count++] = pending_requests[index].event;
			if (GetTickCount() - pending_requests[index].started > 10000 && io_reports < 12)
			{
				io_reports++;
				debugPrint("halo-xbox: a request is still pending after 10 s (status %08lx)\n",
					(unsigned long)pending_requests[index].overlapped->status.Status);
				pending_requests[index].started = GetTickCount();
			}
		}
	}
	pending_lock_leave();
	return count;
}

/* the Win32 wait results */
#define NX_WAIT_OBJECT_0 0UL
#define NX_WAIT_ABANDONED 0x80UL
#define NX_WAIT_IO_COMPLETION 0xC0UL
#define NX_WAIT_TIMEOUT 0x102UL
#define NX_WAIT_FAILED 0xFFFFFFFFUL
#define NX_INFINITE 0xFFFFFFFFUL

/* waits up to milliseconds for one of the events (the object, if any,
first): the index signaled, -1 on a timeout, -2 on failure */
static int wait_for_events(HANDLE *events, int count, unsigned long milliseconds)
{
	LARGE_INTEGER timeout;
	NTSTATUS status;

	timeout.QuadPart = -(LONGLONG)milliseconds * 10000;
	status = NtWaitForMultipleObjectsEx((ULONG)count, events, WaitAny, UserMode, FALSE,
		milliseconds == NX_INFINITE ? NULL : &timeout);
	if (status == STATUS_TIMEOUT)
		return -1;
	if (!NT_SUCCESS(status))
		return -2;
	return (int)status;
}

unsigned long nx_sleep_ex(unsigned long milliseconds, int alertable)
{
	unsigned long started = GetTickCount();

	if (!alertable)
		return SleepEx(milliseconds, FALSE);
	for (;;)
	{
		HANDLE events[MAXIMUM_PENDING_REQUESTS];
		int count;
		unsigned long elapsed, remaining;

		if (run_completions())
			return NX_WAIT_IO_COMPLETION;
		elapsed = GetTickCount() - started;
		if (milliseconds != NX_INFINITE && elapsed >= milliseconds)
		{
			/* SleepEx(0, TRUE) gives up the rest of the time slice, as
			on Windows (the cache's busy waits poll with it) */
			if (!milliseconds)
				SwitchToThread();
			return 0;
		}
		remaining = milliseconds == NX_INFINITE ? NX_INFINITE : milliseconds - elapsed;
		count = pending_events(events, MAXIMUM_PENDING_REQUESTS);
		if (!count)
		{
			/* nothing to complete: sleep it out */
			if (milliseconds != NX_INFINITE)
				SleepEx(remaining, FALSE);
			else
				SleepEx(100, FALSE);
			if (milliseconds != NX_INFINITE)
				return 0;
			continue;
		}
		if (wait_for_events(events, count, remaining > 1000 ? 1000 : remaining) == -2)
			return 0;
	}
}

unsigned long nx_wait_for_single_object_ex(void *handle, unsigned long milliseconds, int alertable)
{
	unsigned long started = GetTickCount();

	if (!alertable)
		return WaitForSingleObjectEx(handle, milliseconds, FALSE);
	for (;;)
	{
		HANDLE events[MAXIMUM_PENDING_REQUESTS + 1];
		int count, result;
		unsigned long elapsed, remaining;

		if (run_completions())
			return NX_WAIT_IO_COMPLETION;
		elapsed = GetTickCount() - started;
		if (milliseconds != NX_INFINITE && elapsed >= milliseconds)
			return NX_WAIT_TIMEOUT;
		remaining = milliseconds == NX_INFINITE ? NX_INFINITE : milliseconds - elapsed;
		events[0] = handle;
		count = 1 + pending_events(events + 1, MAXIMUM_PENDING_REQUESTS);
		result = wait_for_events(events, count, remaining > 1000 ? 1000 : remaining);
		if (result == -2)
			return NX_WAIT_FAILED;
		if (result == 0)
			return NX_WAIT_OBJECT_0;
		/* a request's event, or the second of waiting: round again */
	}
}

/* ---------- threads (nxdk's start routines are __stdcall, as the game's) */

int nx_find_close(void *handle)
{
	return FindClose(handle);
}

int nx_flush_file(void *handle)
{
	IO_STATUS_BLOCK status;

	return NT_SUCCESS(NtFlushBuffersFile(handle, &status));
}

/* ---------- the console */

unsigned long nx_language(void)
{
	ULONG type = 0, value = 0, length = 0;

	if (NT_SUCCESS(ExQueryNonVolatileSetting(XC_LANGUAGE, &type, &value, sizeof(value), &length)) &&
		value >= 1 && value <= 6)
		return value;
	return 1;
}

int nx_launch_info(unsigned long *type, void *data)
{
	if (!LaunchDataPage)
	{
		*type = 2; /* LDT_FROM_DASHBOARD */
		memset(data, 0, 3072);
		return 0;
	}
	*type = LaunchDataPage->Header.dwLaunchDataType;
	memcpy(data, LaunchDataPage->LaunchData, 3072);
	return 1;
}

void nx_launch_dashboard(void)
{
	HalReturnToFirmware(HalQuickRebootRoutine);
}

void *nx_framebuffer(void)
{
	return XVideoGetFB();
}

void nx_wait_for_vblank(void)
{
	XVideoWaitForVBlank();
}

unsigned long nx_random(void)
{
	static unsigned long state = 0;

	if (!state)
	{
		/* the cycle counter too: two emulated consoles started alike can
		read the same performance counter, and their internet play
		identifiers and tokens (p2p.c) must differ */
		unsigned long long cycles = __builtin_ia32_rdtsc();

		state = ((unsigned long)KeQueryPerformanceCounter() ^ (unsigned long)cycles ^
			(unsigned long)(cycles >> 32) ^ (GetTickCount() << 16)) | 1;
	}
	state = state * 1664525UL + 1013904223UL;
	return state;
}
