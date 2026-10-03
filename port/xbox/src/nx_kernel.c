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
console (QEMU's user network, whose addresses are 10.0.2.x, reaches the
host at 10.0.2.2), where a listener keeps the log (tools/xbox_log.py). A
real console on any other network broadcasts it instead, so the listener
keeps it on any computer of that network. */

#define LOG_HOST "10.0.2.2"
#define LOG_PORT 9999

static int log_socket = -1;
static struct sockaddr_in log_address;
static struct sockaddr_in log_broadcast_address;

unsigned long posix_local_ipv4_address(void); /* nx_net.c, network byte order */

/* where the log goes now, or NULL before the console has an address */
static const struct sockaddr_in *log_destination(void)
{
	unsigned long address = ntohl(posix_local_ipv4_address());

	if (!address)
		return NULL;
	if ((address & 0xFFFFFF00UL) == 0x0A000200UL)
		return &log_address;
	return &log_broadcast_address;
}

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
/* debug.network_log off (nx_log_network): the ring keeps the lines (the
exit screen's), nothing is sent */
static volatile int log_network_off;

void nx_log_network(int on)
{
	log_network_off = !on;
}

static void log_send_pending(void)
{
	const unsigned long size = LOG_RING_BYTES - LOG_RING_HEADER;
	unsigned long used = *(volatile unsigned long *)(log_ring + 4);
	const struct sockaddr_in *destination = log_destination();

	if (log_network_off)
	{
		log_sent = used;
		return;
	}
	/* (the ring keeps the lines until there is an address to send from) */
	if (!destination)
		return;
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
		lwip_sendto(log_socket, chunk, count, 0, (const struct sockaddr *)destination, sizeof(*destination));
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
	int network;

	log_ring_start();
	/* -1: no network at all; -2: DHCP has not answered yet, but the
	interface is up and DHCP (and AutoIP after it) carries on, and the ring
	holds the lines until there is an address to send from */
	network = nxNetInit(NULL);
	if (network == -1)
	{
		debugPrint("halo-xbox: no network; the log stays on the screen\n");
		return;
	}
	if (network != 0)
		debugPrint("halo-xbox: no DHCP address yet; the log goes out once the console has one (DHCP or link-local)\n");
	log_socket = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (log_socket < 0)
		return;
	memset(&log_address, 0, sizeof(log_address));
	log_address.sin_family = AF_INET;
	log_address.sin_port = htons(LOG_PORT);
	log_address.sin_addr.s_addr = inet_addr(LOG_HOST);
	log_broadcast_address = log_address;
	log_broadcast_address.sin_addr.s_addr = htonl(0xFFFFFFFFUL);
	debugPrint("halo-xbox: logging to " LOG_HOST ":%d (in xemu), or by broadcast to port %d\n", LOG_PORT, LOG_PORT);
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

static void screen_print(const char *text);
static void print_exit_summary(void);

/* nxdk reboots the console when the game exits (a failed start-up, or the
game's own exit): what went wrong stays on the screen a while first. A
failed start-up ends in a second assertion (halt_and_catch_fire draws with a
device that is not there) and pages of stack walks, so the screen is
cleared for the first errors of the run and its last lines */
static void exit_notice(void)
{
	nx_gpu_show_console();
	debugClearScreen();
	screen_print("halo-xbox: the game has exited; the console restarts in 30 seconds\n");
	print_exit_summary();
	nx_report_memory("at exit");
	/* then the whole log to debug.txt (on the cache partition, Partition3),
	in case the console is switched off during the wait */
	fflush(NULL);
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

/* ---------- the map cache's places

The game copies the disc's maps into six files of fixed size, z:\cache000.map
to z:\cache005.map (cache_files_windows.c: two of 278 MB for the campaign,
one of 35 MB for the main menu, three of 47 MB for multiplayer; 732 MB in
all), which platform_redirect_path sends to the data partition, E:\halo. A
console whose E: holds less free space than that (games copied to it) keeps
the files that do not fit on the two other cache partitions instead
(Partition4 and Partition5, 750 MB each, the scratch space the console
gives titles), mounted here as Y: and W:. The files start anew each boot
(clear_cached_maps), so the places are chosen once, from the free space at
start-up. */

#define MAP_CACHE_FILES 6
#define MAP_CACHE_PLACES 3

static const unsigned long map_cache_file_megabytes[MAP_CACHE_FILES] = { 278, 278, 35, 47, 47, 47 };
static const char *const map_cache_places[MAP_CACHE_PLACES] = { "E:\\halo\\", "Y:\\halo\\", "W:\\halo\\" };
static const char *map_cache_directories[MAP_CACHE_FILES];

/* the free space of a directory's drive in MB, 0 if it cannot be read (a
partition that was never formatted) */
static unsigned long free_megabytes(const char *directory)
{
	char root[4] = { directory[0], ':', '\\', 0 };
	ULARGE_INTEGER available, total, free;

	if (!GetDiskFreeSpaceExA(root, &available, &total, &free))
		return 0;
	return (unsigned long)(free.QuadPart >> 20);
}

static void place_map_cache(void)
{
	unsigned long free[MAP_CACHE_PLACES];
	char text[160];
	int file, place;

	for (place = 0; place < MAP_CACHE_PLACES; place++)
		free[place] = free_megabytes(map_cache_places[place]);
	strcpy(text, "halo-xbox: map cache on");
	for (file = 0; file < MAP_CACHE_FILES; file++)
	{
		/* a margin for the file system's own clusters */
		const unsigned long needed = map_cache_file_megabytes[file] + 2;

		map_cache_directories[file] = map_cache_places[0];
		for (place = 0; place < MAP_CACHE_PLACES; place++)
		{
			if (free[place] >= needed)
			{
				map_cache_directories[file] = map_cache_places[place];
				free[place] -= needed;
				if (place)
					CreateDirectoryA(map_cache_places[place], NULL);
				break;
			}
		}
		snprintf(text + strlen(text), sizeof(text) - strlen(text), " %.2s%s", map_cache_directories[file],
			place == MAP_CACHE_PLACES ? "(no room)" : "");
	}
	strcat(text, "\n");
	nx_print(text);
}

const char *nx_map_cache_directory(unsigned long file)
{
	return file < MAP_CACHE_FILES && map_cache_directories[file] ? map_cache_directories[file] : map_cache_places[0];
}

/* where the XBE was started from (the disc drive, or a folder of the hard
disk) and the free space of the partitions the game writes: the map cache
(above) and the saves and logs on Z: */
static void report_start_place(void)
{
	char text[160];

	snprintf(text, sizeof(text), "halo-xbox: started from %.*s\n", (int)XeImageFileName->Length,
		XeImageFileName->Buffer ? XeImageFileName->Buffer : "");
	nx_print(text);
}

static void report_free_space(char letter)
{
	char root[4] = { letter, ':', '\\', 0 };
	ULARGE_INTEGER available, total, free;
	char text[128];

	if (GetDiskFreeSpaceExA(root, &available, &total, &free))
		snprintf(text, sizeof(text), "halo-xbox: %c: has %lu MB free of %lu MB\n", letter,
			(unsigned long)(free.QuadPart >> 20), (unsigned long)(total.QuadPart >> 20));
	else
		snprintf(text, sizeof(text), "halo-xbox: %c: free space unknown (%lu)\n", letter, (unsigned long)GetLastError());
	nx_print(text);
}

static void reserve_fixed_blocks(void);
static void report_fixed_blocks(void);
static void pending_lock_initialize(void);

void nx_startup(void)
{
	if (started)
		return;
	started = 1;
	/* the game's fixed memory before anything else takes some of it */
	reserve_fixed_blocks();
	/* (the I/O lock before any thread can race to make it) */
	pending_lock_initialize();
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
	/* the other two cache partitions, for the map cache when E: is short
	of room (place_map_cache) */
	mount('Y', "\\Device\\Harddisk0\\Partition4");
	mount('W', "\\Device\\Harddisk0\\Partition5");
	/* the game's copies of the maps on the cache partition: one that a
	stopped run left half-written would fail the game's checks, so they
	start over each boot; the saved games and profiles there stay */
	log_start();
	report_fixed_blocks();
	clear_cached_maps("Z:\\"); /* copies earlier builds left on the utility partition */
	clear_cached_maps("E:\\halo\\");
	clear_cached_maps("Y:\\halo\\");
	clear_cached_maps("W:\\halo\\");
	file_self_test("U:\\selftest.bin");
	file_self_test("Z:\\selftest.bin");
	report_start_place();
	report_free_space('E');
	report_free_space('Z');
	nx_high_memory_available();
	place_map_cache();
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
		/* an empty directory, or none (the cache partitions' until used) */
		if (last_error != ERROR_FILE_NOT_FOUND && last_error != ERROR_NO_MORE_FILES &&
			last_error != ERROR_PATH_NOT_FOUND)
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

/* The hidden screen starts a new page: nxdk's text screen clears itself
when a page is full, and pbkit's report of a GPU error (drawn on it from its
interrupt handler, which then stops the console, so nothing here runs after
it) began wherever start-up had left the cursor. On a real console only
its last two lines showed. */
void nx_print_screen_hidden(void)
{
	screen_hidden = 1;
	debugClearScreen();
	debugPrint("halo-xbox: the GPU shows the game. A GPU error (pbkit's report below) stops the console;\n"
		"the log's last lines are on the network (tools/xbox_log.py) and in debug.txt.\n");
}

/* nxdk's debugPrint formats into 512 bytes and drops the rest: longer text
goes to the screen in pieces, ending at a line's end where one is near */
static void screen_print(const char *text)
{
	char piece[400];
	size_t length = strlen(text);

	while (length)
	{
		size_t count = length < sizeof(piece) - 1 ? length : sizeof(piece) - 1;

		if (count < length)
		{
			size_t end = count;

			while (end > 0 && text[end - 1] != '\n')
				end--;
			if (end > 0)
				count = end;
		}
		memcpy(piece, text, count);
		piece[count] = 0;
		debugPrint("%s", piece);
		text += count;
		length -= count;
	}
}

/* the ring's text, oldest first, for the screen (a crash report, the exit) */
static char ring_copy[LOG_RING_BYTES];

/* the ring's text, oldest first, into text (LOG_RING_BYTES); its length */
static unsigned long ring_text(char *text)
{
	const unsigned long size = LOG_RING_BYTES - LOG_RING_HEADER;
	unsigned long used, start, index, length = 0;

	if (!log_ring)
		return 0;
	used = *(volatile unsigned long *)(log_ring + 4);
	start = used > size ? used - size : 0;
	for (index = start; index < used; index++)
		text[length++] = log_ring[LOG_RING_HEADER + index % size];
	text[length] = 0;
	return length;
}

void nx_print_screen_shown(void)
{
	enum { REPLAY_BYTES = 1600 };
	char *text = ring_copy;
	unsigned long length;
	char *first;

	screen_hidden = 0;
	/* (at 720p in the low 64 MB the screen was the depth buffer meanwhile,
	nx_gpu_depth_buffer) */
	debugClearScreen();
	length = ring_text(text);
	/* the last lines, from the first whole one */
	first = length > REPLAY_BYTES ? strchr(text + length - REPLAY_BYTES, '\n') : NULL;
	screen_print(first ? first + 1 : text + (length > REPLAY_BYTES ? length - REPLAY_BYTES : 0));
}

/* prints a line of the ring (without its line end), cut to two lines of the
screen */
static void print_ring_line(const char *line, size_t length)
{
	char text[144];

	if (length > sizeof(text) - 2)
		length = sizeof(text) - 2;
	memcpy(text, line, length);
	text[length] = '\n';
	text[length + 1] = 0;
	screen_print(text);
}

static int line_contains(const char *line, size_t length, const char *what)
{
	size_t what_length = strlen(what), index;

	for (index = 0; index + what_length <= length; index++)
	{
		if (!memcmp(line + index, what, what_length))
			return 1;
	}
	return 0;
}

static int line_matches(const char *line, size_t length, const char *const *patterns)
{
	for (; *patterns; patterns++)
	{
		if (line_contains(line, length, *patterns))
			return 1;
	}
	return 0;
}

/* prints the first lines (up to maximum) that match one of patterns and
none of excluded; returns how many */
static int print_first_matching(const char *text, const char *end, const char *const *patterns,
	const char *const *excluded, int maximum)
{
	const char *cursor;
	int found = 0;

	for (cursor = text; cursor < end && found < maximum;)
	{
		const char *line_end = memchr(cursor, '\n', end - cursor);
		size_t line_length = (line_end ? line_end : end) - cursor;

		if (line_matches(cursor, line_length, patterns) &&
			(!excluded || !line_matches(cursor, line_length, excluded)))
		{
			print_ring_line(cursor, line_length);
			found++;
		}
		cursor = line_end ? line_end + 1 : end;
	}
	return found;
}

/* the first lines of the run that name a failure (the game's fatal
assertions and nxdk's first, then the platform layer's reports of failed
calls), then the last lines */
static void print_exit_summary(void)
{
	static const char *const fatal[] = { "EXCEPTION halt", "ASSERTION", "halo-xbox: EXCEPTION", NULL };
	static const char *const failures[] = {
		"### ERROR", "failed", "cannot ", "impossible", "couldn't", "EXCEPTION warn", NULL,
	};
	enum { MAXIMUM_FATAL = 3, MAXIMUM_FAILURES = 5, LAST_LINES = 7 };
	char *text = ring_copy;
	unsigned long length = ring_text(text);
	const char *cursor, *end = text + length;
	int found, lines = 0;

	if (!length)
		return;
	screen_print("halo-xbox: the first errors:\n");
	found = print_first_matching(text, end, fatal, NULL, MAXIMUM_FATAL);
	found += print_first_matching(text, end, failures, fatal, MAXIMUM_FAILURES - found);
	if (!found)
		screen_print("  (none in the log)\n");
	/* the last lines: back from the end to the start of the LAST_LINESth */
	screen_print("halo-xbox: the last lines:\n");
	for (cursor = end; cursor > text; cursor--)
	{
		if (cursor[-1] == '\n' && cursor != end && ++lines >= LAST_LINES)
			break;
	}
	while (cursor < end)
	{
		const char *line_end = memchr(cursor, '\n', end - cursor);

		print_ring_line(cursor, (line_end ? line_end : end) - cursor);
		cursor = line_end ? line_end + 1 : end;
	}
}

void nx_print(const char *text)
{
	log_ring_append(text);
	if (!screen_hidden)
	{
		DbgPrint("%s", text);
		screen_print(text);
	}
	/* without the ring (no memory for it), directly, as before; not from
	a raised IRQL, where the stack would wait on itself */
	if (!log_ring && log_socket >= 0 && !log_network_off && KeGetCurrentIrql() == PASSIVE_LEVEL)
	{
		const struct sockaddr_in *destination = log_destination();

		if (destination)
			lwip_sendto(log_socket, text, strlen(text), 0, (const struct sockaddr *)destination, sizeof(*destination));
	}
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

/* The top 64 MB of a 128 MB console. The 720p disc's XBE leaves the
kernel's 64 MB limit off (tools/xbox_disc.py, tools/xbox_xbe_flags.py; the
others keep it), but a retail kernel may still keep the rest, and the page
count says 128 MB either way: a page there, allocated and freed, is the
answer. */
#define HIGH_MEMORY_BASE 0x04000000UL
#define HIGH_MEMORY_END 0x08000000UL
/* the top 20 KB are the GPU's (pbkit's MAXRAM leaves them out the same way) */
#define GPU_RESERVED_TOP 0x5000UL

static int high_memory_placement;

int nx_high_memory_available(void)
{
	static int probed, available;

	if (!probed)
	{
		void *page = MmAllocateContiguousMemoryEx(4096, HIGH_MEMORY_BASE, HIGH_MEMORY_END - 1, 4096, PAGE_READWRITE);
		char text[128];

		probed = 1;
		available = page != NULL;
		if (page)
			MmFreeContiguousMemory(page);
		snprintf(text, sizeof(text), "halo-xbox: memory: the top 64 MB of a 128 MB console is %s\n",
			available ? "there" : "not there");
		nx_print(text);
	}
	return available;
}

unsigned long nx_physical_top(void)
{
	return nx_high_memory_available() ? HIGH_MEMORY_END : HIGH_MEMORY_BASE;
}

void nx_high_memory_placement(int on)
{
	high_memory_placement = on && nx_high_memory_available();
}

/* whether the low 64 MB still has these blocks (contiguous, where the GPU
can reach them): each taken, then all given back */
int nx_low_memory_room(const unsigned long *sizes, int count)
{
	void *blocks[16];
	int index, room = 1;

	for (index = 0; index < count && index < 16; index++)
	{
		blocks[index] = NULL;
		if (!sizes[index])
			continue;
		blocks[index] = MmAllocateContiguousMemoryEx(sizes[index], 0, HIGH_MEMORY_BASE - GPU_RESERVED_TOP - 1, 0x4000,
			PAGE_READWRITE);
		if (!blocks[index])
		{
			room = 0;
			break;
		}
	}
	while (index-- > 0)
	{
		if (blocks[index])
			MmFreeContiguousMemory(blocks[index]);
	}
	return room;
}

/* the physical address of a byte of mapped memory */
unsigned long nx_physical_address(const void *address)
{
	return (unsigned long)MmGetPhysicalAddress((PVOID)address);
}

/* ordinary memory, for what only the processor reads: the kernel may give
it from anywhere, the top 64 MB of a 128 MB console included */
void *nx_cpu_alloc(unsigned long size)
{
	PVOID base = NULL;
	SIZE_T region = size;

	if (!NT_SUCCESS(NtAllocateVirtualMemory(&base, 0, &region, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
		return NULL;
	return base;
}

void nx_cpu_free(void *memory)
{
	PVOID base = memory;
	SIZE_T region = 0;

	if (memory)
		NtFreeVirtualMemory(&base, &region, MEM_RELEASE);
}

int nx_high_memory_placed(void)
{
	return high_memory_placement;
}

/* ---------- the game's fixed blocks

The game state (physical 0x61000, 3.3 MB) and the tag cache (0x3A6000,
22 MB; the maps' tag data points into it) must be at those addresses
(physical_memory_map.c, include/halo_xbox_limits.h). They are taken first
thing at start-up, before the video mode, the GPU, the network and the
sound allocate anything, and handed to the game when it asks for them: a
128 MB console had something in the tag cache's range by the time the game
asked (2026-09-30). If one cannot be had, the start-up says which parts of
its range are taken. */

static const struct
{
	unsigned long address, size;
	const char *name;
} fixed_blocks[] =
{
	{ 0x00061000UL, 0x00345000UL, "game state" },
	{ 0x003A6000UL, 0x01600000UL, "tag cache" },
};
#define FIXED_BLOCK_COUNT (sizeof(fixed_blocks) / sizeof(fixed_blocks[0]))
static void *fixed_reserved[FIXED_BLOCK_COUNT];
static int fixed_failed[FIXED_BLOCK_COUNT];

static void reserve_fixed_blocks(void)
{
	unsigned long index;

	for (index = 0; index < FIXED_BLOCK_COUNT; index++)
	{
		fixed_reserved[index] = MmAllocateContiguousMemoryEx(fixed_blocks[index].size, fixed_blocks[index].address,
			fixed_blocks[index].address + fixed_blocks[index].size - 1, 4096, PAGE_READWRITE);
		fixed_failed[index] = fixed_reserved[index] == NULL;
	}
}

/* which 64 KB pieces of a range are taken, as spans */
static void report_taken(unsigned long address, unsigned long size)
{
	enum { PIECE = 0x10000 };
	unsigned long piece, span_start = 0, spans = 0;
	int in_span = 0;
	char text[128];

	for (piece = address & ~(PIECE - 1UL); piece < address + size + PIECE; piece += PIECE)
	{
		int taken = 0;

		if (piece < address + size)
		{
			void *probe = MmAllocateContiguousMemoryEx(PIECE, piece, piece + PIECE - 1, 4096, PAGE_READWRITE);

			taken = probe == NULL;
			if (probe)
				MmFreeContiguousMemory(probe);
		}
		if (taken && !in_span)
		{
			span_start = piece;
			in_span = 1;
		}
		else if (!taken && in_span)
		{
			in_span = 0;
			if (spans++ < 8)
			{
				snprintf(text, sizeof(text), "halo-xbox:   taken: %08lx-%08lx (%lu KB)\n", span_start, piece - 1,
					(piece - span_start) / 1024);
				nx_print(text);
			}
		}
	}
	snprintf(text, sizeof(text), "halo-xbox:   %lu taken span(s) in %08lx-%08lx\n", spans, address, address + size - 1);
	nx_print(text);
}

/* the start-up's report: whether the blocks could be reserved */
static void report_fixed_blocks(void)
{
	unsigned long index;

	for (index = 0; index < FIXED_BLOCK_COUNT; index++)
	{
		if (fixed_failed[index])
		{
			char text[160];

			snprintf(text, sizeof(text), "halo-xbox: the %s's memory (%08lx, %lu KB) could not be reserved at start-up:\n",
				fixed_blocks[index].name, fixed_blocks[index].address, fixed_blocks[index].size / 1024);
			nx_print(text);
			report_taken(fixed_blocks[index].address, fixed_blocks[index].size);
		}
	}
}

/* a reserved block, if this is the request for it */
static void *take_fixed_block(unsigned long size, unsigned long physical_address, unsigned long protect)
{
	unsigned long index;

	for (index = 0; index < FIXED_BLOCK_COUNT; index++)
	{
		void *memory = fixed_reserved[index];

		if (memory && physical_address == fixed_blocks[index].address && size <= fixed_blocks[index].size)
		{
			fixed_reserved[index] = NULL;
			if (protect != PAGE_READWRITE)
				MmSetAddressProtect(memory, fixed_blocks[index].size, protect);
			return memory;
		}
	}
	return NULL;
}

void *nx_physical_alloc(unsigned long size, unsigned long physical_address, unsigned long alignment,
	unsigned long protect)
{
	/* (below the GPU's DMA windows' limit, pbkit's MAXRAM: on a 128 MB
	console the top of the low 64 MB is ordinary memory the kernel would
	hand out, beyond the windows) */
	ULONG_PTR lowest = 0, highest = HIGH_MEMORY_BASE - GPU_RESERVED_TOP - 1;

	void *memory = NULL;

	if (physical_address != 0xffffffffUL)
	{
		lowest = physical_address;
		highest = physical_address + size - 1;
		memory = take_fixed_block(size, physical_address, protect);
		if (memory)
			return memory;
	}
	/* the kernel allocates pages: an alignment below a page (Direct3D's 128
	bytes) is a page's, and a power of two */
	if (alignment < 4096)
		alignment = 4096;
	/* the top 64 MB first at 720p, the low 64 MB when it is full */
	if (physical_address == 0xffffffffUL && high_memory_placement)
		memory = MmAllocateContiguousMemoryEx(size, HIGH_MEMORY_BASE, HIGH_MEMORY_END - GPU_RESERVED_TOP - 1, alignment,
			protect);
	if (!memory)
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

static void pending_lock_initialize(void)
{
	if (!pending_lock_ready)
	{
		RtlInitializeCriticalSection(&pending_lock);
		pending_lock_ready = 1;
	}
}

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
		char text[80];

		io_reports++;
		snprintf(text, sizeof(text), "halo-xbox: NtReadFile/NtWriteFile: status %08lx\n", (unsigned long)status);
		nx_print(text);
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

		/* most of the 64 slots are free or another thread's: skip them
		without the lock (the answer is checked again under it) */
		if (!request->used || request->thread != thread)
			continue;
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
			char text[80];

			io_reports++;
			snprintf(text, sizeof(text), "halo-xbox: completion: status %08lx, %lu bytes\n",
				(unsigned long)overlapped->status.Status, (unsigned long)overlapped->status.Information);
			nx_print(text);
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
				char text[96];

				io_reports++;
				snprintf(text, sizeof(text), "halo-xbox: a request is still pending after 10 s (status %08lx)\n",
					(unsigned long)pending_requests[index].overlapped->status.Status);
				nx_print(text);
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
		/* (the object and 63 requests at most: the kernel waits on 64) */
		count = 1 + pending_events(events + 1, MAXIMUM_PENDING_REQUESTS - 1);
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
