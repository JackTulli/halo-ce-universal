/*
MSVC_CRT.C

Microsoft C runtime functions the game calls that nxdk's C library (PDCLib)
does not provide under the same names (port/xbox/include/crt), the game's
low-level file functions (its libtiff) over the Win32 file functions, and
the game's debug log: d:\debug.txt is the disc, so it goes to the cache
partition and is echoed to the screen.
*/

#include "platform.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <io.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* CreateFile's sharing mode (learn.microsoft.com, CreateFileA), which the
XDK declarations leave out */
#ifndef FILE_SHARE_READ
#define FILE_SHARE_READ 0x00000001
#endif

/* ---------- strings */

char *_strlwr(char *string)
{
	char *cursor;

	for (cursor = string; *cursor; cursor++)
		*cursor = (char)tolower((unsigned char)*cursor);
	return string;
}

char *_strupr(char *string)
{
	char *cursor;

	for (cursor = string; *cursor; cursor++)
		*cursor = (char)toupper((unsigned char)*cursor);
	return string;
}

static char *unsigned_to_string(unsigned long value, char *string, int radix, int negative)
{
	char digits[40];
	int count = 0;
	char *cursor = string;

	if (radix < 2 || radix > 36)
		radix = 10;
	do
	{
		int digit = (int)(value % (unsigned long)radix);

		digits[count++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
		value /= (unsigned long)radix;
	} while (value);
	if (negative)
		*cursor++ = '-';
	while (count)
		*cursor++ = digits[--count];
	*cursor = 0;
	return string;
}

char *_ltoa(long value, char *string, int radix)
{
	if (radix == 10 && value < 0)
		return unsigned_to_string((unsigned long)(-(value + 1)) + 1, string, radix, 1);
	return unsigned_to_string((unsigned long)value, string, radix, 0);
}

char *_itoa(int value, char *string, int radix)
{
	return _ltoa(value, string, radix);
}

char *_ultoa(unsigned long value, char *string, int radix)
{
	return unsigned_to_string(value, string, radix, 0);
}

/* ---------- paths */

void _splitpath(const char *path, char *drive, char *directory, char *name, char *extension)
{
	const char *cursor = path;
	const char *directory_end;
	const char *extension_start;

	if (drive)
		drive[0] = 0;
	if (cursor[0] && cursor[1] == ':')
	{
		if (drive)
		{
			drive[0] = cursor[0];
			drive[1] = ':';
			drive[2] = 0;
		}
		cursor += 2;
	}
	directory_end = cursor;
	{
		const char *scan;

		for (scan = cursor; *scan; scan++)
		{
			if (*scan == '\\' || *scan == '/')
				directory_end = scan + 1;
		}
	}
	if (directory)
	{
		memcpy(directory, cursor, (size_t)(directory_end - cursor));
		directory[directory_end - cursor] = 0;
	}
	extension_start = strrchr(directory_end, '.');
	if (!extension_start)
		extension_start = directory_end + strlen(directory_end);
	if (name)
	{
		memcpy(name, directory_end, (size_t)(extension_start - directory_end));
		name[extension_start - directory_end] = 0;
	}
	if (extension)
		strcpy(extension, extension_start);
}

void _makepath(char *path, const char *drive, const char *directory, const char *name, const char *extension)
{
	path[0] = 0;
	if (drive && drive[0])
	{
		strcat(path, drive);
		if (drive[strlen(drive) - 1] != ':')
			strcat(path, ":");
	}
	if (directory && directory[0])
	{
		strcat(path, directory);
		if (directory[strlen(directory) - 1] != '\\' && directory[strlen(directory) - 1] != '/')
			strcat(path, "\\");
	}
	if (name)
		strcat(path, name);
	if (extension && extension[0])
	{
		if (extension[0] != '.')
			strcat(path, ".");
		strcat(path, extension);
	}
}

/* ---------- x87 floating point control (the MSVC layout of the control
and status words, translated to the x87's) */

static unsigned int control_word_to_msvc(unsigned short word)
{
	unsigned int value = 0;

	if (word & 0x01) value |= _EM_INVALID;
	if (word & 0x02) value |= _EM_DENORMAL;
	if (word & 0x04) value |= _EM_ZERODIVIDE;
	if (word & 0x08) value |= _EM_OVERFLOW;
	if (word & 0x10) value |= _EM_UNDERFLOW;
	if (word & 0x20) value |= _EM_INEXACT;
	switch ((word >> 10) & 3)
	{
	case 1: value |= _RC_DOWN; break;
	case 2: value |= _RC_UP; break;
	case 3: value |= _RC_CHOP; break;
	default: break;
	}
	switch ((word >> 8) & 3)
	{
	case 0: value |= _PC_24; break;
	case 2: value |= _PC_53; break;
	default: value |= _PC_64; break;
	}
	return value;
}

static unsigned short msvc_to_control_word(unsigned int value, unsigned short word)
{
	word &= (unsigned short)~0x0f3f;
	if (value & _EM_INVALID) word |= 0x01;
	if (value & _EM_DENORMAL) word |= 0x02;
	if (value & _EM_ZERODIVIDE) word |= 0x04;
	if (value & _EM_OVERFLOW) word |= 0x08;
	if (value & _EM_UNDERFLOW) word |= 0x10;
	if (value & _EM_INEXACT) word |= 0x20;
	switch (value & _MCW_RC)
	{
	case _RC_DOWN: word |= 1 << 10; break;
	case _RC_UP: word |= 2 << 10; break;
	case _RC_CHOP: word |= 3 << 10; break;
	default: break;
	}
	switch (value & _MCW_PC)
	{
	case _PC_24: break;
	case _PC_53: word |= 2 << 8; break;
	default: word |= 3 << 8; break;
	}
	return word;
}

static unsigned short read_control_word(void)
{
	unsigned short word;

	__asm__ __volatile__("fnstcw %0" : "=m"(word));
	return word;
}

static void write_control_word(unsigned short word)
{
	__asm__ __volatile__("fldcw %0" : : "m"(word));
}

unsigned int _control87(unsigned int new_value, unsigned int mask)
{
	unsigned short word = read_control_word();
	unsigned int current = control_word_to_msvc(word);

	if (mask)
	{
		unsigned int wanted = (current & ~mask) | (new_value & mask);

		write_control_word(msvc_to_control_word(wanted, word));
		current = control_word_to_msvc(read_control_word());
	}
	return current;
}

unsigned int _controlfp(unsigned int new_value, unsigned int mask)
{
	return _control87(new_value, mask);
}

unsigned int _statusfp(void)
{
	unsigned short word;
	unsigned int value = 0;

	__asm__ __volatile__("fnstsw %0" : "=m"(word));
	if (word & 0x01) value |= _EM_INVALID;
	if (word & 0x02) value |= _EM_DENORMAL;
	if (word & 0x04) value |= _EM_ZERODIVIDE;
	if (word & 0x08) value |= _EM_OVERFLOW;
	if (word & 0x10) value |= _EM_UNDERFLOW;
	if (word & 0x20) value |= _EM_INEXACT;
	return value;
}

unsigned int _clearfp(void)
{
	unsigned int value = _statusfp();

	__asm__ __volatile__("fnclex");
	return value;
}

int _isnan(double value)
{
	return value != value;
}

int _finite(double value)
{
	return value == value && value - value == 0.0;
}

double _hypot(double x, double y)
{
	return sqrt(x * x + y * y);
}

double _copysign(double x, double y)
{
	return __builtin_copysign(x, y);
}

/* ---------- memory */

size_t _msize(void *pointer)
{
	(void)pointer;
	return 0;
}

/* ---------- low-level files (the game's libtiff), as descriptors over
Win32 file handles */

#define MAXIMUM_DESCRIPTORS 32

static HANDLE descriptors[MAXIMUM_DESCRIPTORS];

static HANDLE handle_of(int descriptor)
{
	if (descriptor < 0 || descriptor >= MAXIMUM_DESCRIPTORS || !descriptors[descriptor])
		return INVALID_HANDLE_VALUE;
	return descriptors[descriptor];
}

int _open(const char *path, int flags, ...)
{
	DWORD access = (flags & _O_WRONLY) ? GENERIC_WRITE : (flags & _O_RDWR) ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
	DWORD disposition = OPEN_EXISTING;
	HANDLE handle;
	int descriptor;

	if (flags & _O_CREAT)
		disposition = (flags & _O_EXCL) ? CREATE_NEW : (flags & _O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
	else if (flags & _O_TRUNC)
		disposition = TRUNCATE_EXISTING;
	for (descriptor = 3; descriptor < MAXIMUM_DESCRIPTORS; descriptor++)
	{
		if (!descriptors[descriptor])
			break;
	}
	if (descriptor == MAXIMUM_DESCRIPTORS)
		return -1;
	handle = CreateFileA(path, access, FILE_SHARE_READ, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return -1;
	if (flags & _O_APPEND)
		SetFilePointer(handle, 0, NULL, FILE_END);
	descriptors[descriptor] = handle;
	return descriptor;
}

int _close(int descriptor)
{
	HANDLE handle = handle_of(descriptor);

	if (handle == INVALID_HANDLE_VALUE)
		return -1;
	descriptors[descriptor] = NULL;
	return CloseHandle(handle) ? 0 : -1;
}

int _read(int descriptor, void *buffer, unsigned int count)
{
	DWORD read = 0;

	if (!ReadFile(handle_of(descriptor), buffer, count, &read, NULL))
		return -1;
	return (int)read;
}

int _write(int descriptor, const void *buffer, unsigned int count)
{
	DWORD written = 0;

	if (!WriteFile(handle_of(descriptor), buffer, count, &written, NULL))
		return -1;
	return (int)written;
}

long _lseek(int descriptor, long offset, int origin)
{
	DWORD position = SetFilePointer(handle_of(descriptor), offset, NULL, (DWORD)origin);

	return position == INVALID_SET_FILE_POINTER ? -1L : (long)position;
}

long _filelength(int descriptor)
{
	DWORD size = GetFileSize(handle_of(descriptor), NULL);

	return size == INVALID_FILE_SIZE ? -1L : (long)size;
}

int _stat(const char *path, struct _stat *buffer)
{
	WIN32_FILE_ATTRIBUTE_DATA data;

	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data))
		return -1;
	memset(buffer, 0, sizeof(*buffer));
	buffer->st_mode = (unsigned short)((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _S_IFDIR | _S_IEXEC : _S_IFREG);
	buffer->st_mode |= _S_IREAD | ((data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0 : _S_IWRITE);
	buffer->st_nlink = 1;
	buffer->st_size = (long)data.nFileSizeLow;
	return 0;
}

int _fstat(int descriptor, struct _stat *buffer)
{
	HANDLE handle = handle_of(descriptor);

	if (handle == INVALID_HANDLE_VALUE)
		return -1;
	memset(buffer, 0, sizeof(*buffer));
	buffer->st_mode = _S_IFREG | _S_IREAD | _S_IWRITE;
	buffer->st_nlink = 1;
	buffer->st_size = (long)GetFileSize(handle, NULL);
	return 0;
}

FILE *halo_xbox_fopen(const char *path, const char *mode);

/* ---------- the C library's gaps */

FILE *fdopen(int handle, const char *mode)
{
	(void)handle;
	(void)mode;
	return NULL;
}

/* nxdk's C library has no local time (its localtime returns NULL): the
console's clock is taken as UTC */
struct tm *localtime(const time_t *timer)
{
	return gmtime(timer);
}

struct tm *localtime_r(const time_t *timer, struct tm *result)
{
	struct tm *local = localtime(timer);

	if (!local)
		return NULL;
	*result = *local;
	return result;
}

/* MSVC's errno accessor (the game's zlib) */
int *_errno(void)
{
	return &errno;
}

/* the script compiler reads real literals with atof; strtod is nx_stdlib.c's */
double atof(const char *text)
{
	return strtod(text, NULL);
}

/* the Linux build's names for the log-aware file functions, which
port/linux/src/msvc_wide.c calls */
FILE *halo_linux_fopen(const char *path, const char *mode)
{
	return halo_xbox_fopen(path, mode);
}

FILE *halo_linux_freopen(const char *path, const char *mode, FILE *stream)
{
	char redirected[MAX_PATH];

	return freopen(platform_redirect_path(path, redirected), mode, stream);
}

int halo_linux_remove(const char *path)
{
	char redirected[MAX_PATH];

	return remove(platform_redirect_path(path, redirected));
}

/* ---------- the debug log */

static FILE *debug_file;

static const char *file_name_of(const char *path)
{
	const char *name = path;
	const char *cursor;

	for (cursor = path; *cursor; cursor++)
	{
		if (*cursor == '\\' || *cursor == '/')
			name = cursor + 1;
	}
	return name;
}

FILE *halo_xbox_fopen(const char *path, const char *mode)
{
	char redirected[MAX_PATH];
	FILE *file;

	/* the development kit's writable D: is the cache partition here */
	path = platform_redirect_path(path, redirected);
	file = fopen(path, mode);
	if (!_stricmp(file_name_of(path), "debug.txt"))
	{
		/* the debug log, echoed to the screen and the network */
		if (!file)
			file = fopen(path, "wb");
		if (!debug_file)
			debug_file = file;
		if (!file)
			platform_log("cannot open the debug log; it goes to the screen only");
	}
	return file;
}

/* ---------- printf with MSVC length modifiers (as port/linux/src/msvc_crt.c)

Rewrites %I64 as %ll and drops %I32; PDCLib reads the rest. */
static const char *translate_format(const char *format, char *buffer, size_t size)
{
	const char *cursor;
	size_t length = 0;

	if (!strstr(format, "I64") && !strstr(format, "I32"))
		return format;
	for (cursor = format; *cursor && length + 3 < size; cursor++)
	{
		buffer[length++] = *cursor;
		if (*cursor != '%')
			continue;
		if (cursor[1] == '%')
		{
			buffer[length++] = *++cursor;
			continue;
		}
		/* copy flags, width and precision */
		while (cursor[1] && strchr("-+ #0123456789.*", cursor[1]) && length + 3 < size)
			buffer[length++] = *++cursor;
		if (cursor[1] == 'I' && cursor[2] == '6' && cursor[3] == '4')
		{
			buffer[length++] = 'l';
			buffer[length++] = 'l';
			cursor += 3;
		}
		else if (cursor[1] == 'I' && cursor[2] == '3' && cursor[3] == '2')
		{
			cursor += 3;
		}
	}
	buffer[length] = '\0';
	return buffer;
}

/* ---------- MSVC's printf and PDCLib's

A NULL %s argument prints as "(null)" under MSVC; PDCLib reads through
it. The arguments are copied to an array of stack words (cdecl on the x86:
what a va_list points at) with NULL strings replaced. Returns the words as
a va_list, or arguments unchanged when the format is not understood. */

#define MAXIMUM_ARGUMENT_WORDS 64

static const char null_text[] = "(null)";
static const wchar_t null_wide_text[] = L"(null)";

static va_list null_safe_arguments(const char *format, va_list arguments, unsigned long *words)
{
	const char *cursor;
	int count = 0;
	va_list in;

	va_copy(in, arguments);
	for (cursor = format; *cursor; cursor++)
	{
		int wide = 0, longs = 0;

		if (*cursor != '%')
			continue;
		cursor++;
		if (*cursor == '%')
			continue;
		while (*cursor && strchr("-+ #0'", *cursor))
			cursor++;
		if (*cursor == '*')
		{
			if (count >= MAXIMUM_ARGUMENT_WORDS)
				goto give_up;
			words[count++] = va_arg(in, unsigned long);
			cursor++;
		}
		else
		{
			while (*cursor >= '0' && *cursor <= '9')
				cursor++;
		}
		if (*cursor == '.')
		{
			cursor++;
			if (*cursor == '*')
			{
				if (count >= MAXIMUM_ARGUMENT_WORDS)
					goto give_up;
				words[count++] = va_arg(in, unsigned long);
				cursor++;
			}
			else
			{
				while (*cursor >= '0' && *cursor <= '9')
					cursor++;
			}
		}
		for (;;)
		{
			if (*cursor == 'l')
				longs++;
			else if (*cursor == 'j' || *cursor == 'L' || *cursor == 'q')
				longs = 2;
			else if (*cursor == 'w')
				wide = 1;
			else if (*cursor != 'h' && *cursor != 'z' && *cursor != 't')
				break;
			cursor++;
		}
		if (count + 2 > MAXIMUM_ARGUMENT_WORDS)
			goto give_up;
		switch (*cursor)
		{
		case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
			words[count++] = va_arg(in, unsigned long);
			if (longs >= 2)
				words[count++] = va_arg(in, unsigned long);
			break;
		case 'c': case 'C': case 'p': case 'n':
			words[count++] = va_arg(in, unsigned long);
			break;
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
			words[count++] = va_arg(in, unsigned long);
			words[count++] = va_arg(in, unsigned long);
			break;
		case 's': case 'S':
		{
			unsigned long pointer = va_arg(in, unsigned long);

			if (!pointer)
				pointer = (longs || wide || *cursor == 'S') ? (unsigned long)null_wide_text : (unsigned long)null_text;
			words[count++] = pointer;
			break;
		}
		default:
			goto give_up;
		}
	}
	va_end(in);
	return (va_list)words;
give_up:
	va_end(in);
	return arguments;
}

int halo_xbox_vsnprintf(char *buffer, size_t count, const char *format, va_list arguments)
{
	char translated[1024];
	unsigned long words[MAXIMUM_ARGUMENT_WORDS];

	format = translate_format(format, translated, sizeof(translated));
	return vsnprintf(buffer, count, format, null_safe_arguments(format, arguments, words));
}

int halo_xbox_snprintf(char *buffer, size_t count, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_xbox_vsnprintf(buffer, count, format, arguments);
	va_end(arguments);
	return result;
}

int halo_xbox_vsprintf(char *buffer, const char *format, va_list arguments)
{
	char translated[1024];

	unsigned long words[MAXIMUM_ARGUMENT_WORDS];

	format = translate_format(format, translated, sizeof(translated));
	return vsprintf(buffer, format, null_safe_arguments(format, arguments, words));
}

int halo_xbox_sprintf(char *buffer, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_xbox_vsprintf(buffer, format, arguments);
	va_end(arguments);
	return result;
}

static void echo_to_screen(const char *format, va_list arguments)
{
	char text[512];

	halo_xbox_vsnprintf(text, sizeof(text), format, arguments);
	nx_print(text);
}

int halo_xbox_vprintf(const char *format, va_list arguments)
{
	echo_to_screen(format, arguments);
	return 0;
}

int halo_xbox_printf(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	echo_to_screen(format, arguments);
	va_end(arguments);
	return 0;
}

int halo_xbox_vfprintf(FILE *stream, const char *format, va_list arguments)
{
	char translated[1024];

	if (stream == debug_file || !stream)
	{
		va_list copy;

		va_copy(copy, arguments);
		echo_to_screen(format, copy);
		va_end(copy);
		if (!stream)
			return 0;
	}
	{
		unsigned long words[MAXIMUM_ARGUMENT_WORDS];

		format = translate_format(format, translated, sizeof(translated));
		return vfprintf(stream, format, null_safe_arguments(format, arguments, words));
	}
}

int halo_xbox_fprintf(FILE *stream, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_xbox_vfprintf(stream, format, arguments);
	va_end(arguments);
	return result;
}

int halo_xbox_fputs(const char *string, FILE *stream)
{
	if (stream == debug_file || !stream)
	{
		nx_print(string);
		if (!stream)
			return 0;
	}
	return fputs(string, stream);
}
