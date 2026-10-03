/*
MSVC_CRT.C

Microsoft C runtime functions the game calls that nxdk's C library (PDCLib)
does not provide under the same names (port/xbox/include/crt), the game's
low-level file functions (its libtiff) over the Win32 file functions, and
the game's debug log: d:\debug.txt is the disc, so it goes to the cache
partition and is echoed to the screen.
*/

/* the printf family here calls PDCLib's own (crt/stdio.h) */
#define HALO_XBOX_PRINTF_IMPLEMENTATION

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

/* ---------- floating-point conversions

PDCLib's printf neither formats %f, %e, %g (or %a) nor takes their double
from the argument list, so every later argument was read out of step: a %s
after a %f took half a double as its pointer (the host's crashes of
2026-09-29 and 2026-09-30 in network_player_log_idle_action, players.c).
They are formatted here and handed to PDCLib as strings.

The method is musl's (fmt_fp, src/stdio/vfprintf.c; MIT licence, as
port/third_party/musl-math) for the double: the value's exact decimal
expansion in limbs of nine digits, rounded to the precision half to even,
by integer arithmetic alone, so the x87's precision setting (the game's, or
Direct3D's) does not enter into it. Exponents have two digits at least, as
on the other ports. %a is written as %e. */

#define FORMAT_LEFT 1u
#define FORMAT_PLUS 2u
#define FORMAT_SPACE 4u
#define FORMAT_ALTERNATE 8u
#define FORMAT_ZERO 16u

#define FORMAT_MINIMUM(a, b) ((a) < (b) ? (a) : (b))
#define FORMAT_MAXIMUM(a, b) ((a) > (b) ? (a) : (b))

struct format_output
{
	char *text;
	int size;
	int length;
};

static void format_put(struct format_output *output, const char *text, long count)
{
	for (; count > 0; count--, text++)
	{
		if (output->length < output->size - 1)
			output->text[output->length++] = *text;
	}
	if (output->size > 0)
		output->text[output->length] = 0;
}

/* pads to width with c unless left-adjusted or zero-padded (musl's pad) */
static void format_pad(struct format_output *output, char c, int width, int length, unsigned flags)
{
	if ((flags & (FORMAT_LEFT | FORMAT_ZERO)) || length >= width)
		return;
	for (; length < width; length++)
		format_put(output, &c, 1);
}

/* the decimal digits of value, written backwards ending at end (none for
zero); returns the first */
static char *format_unsigned(unsigned long value, char *end)
{
	for (; value; value /= 10)
		*--end = (char)('0' + value % 10);
	return end;
}

static void format_double(struct format_output *output, double value, int width, int precision, unsigned flags,
	int conversion)
{
	enum { MANTISSA_DIGITS = 53, MAXIMUM_EXPONENT = 1024 };
	unsigned long big[(MANTISSA_DIGITS + 28) / 29 + 1 + (MAXIMUM_EXPONENT + MANTISSA_DIGITS + 28 + 8) / 9];
	unsigned long *a, *d, *r, *z;
	int e2, e, i, j, l, p = precision, t = conversion, pl;
	char buf[9 + MANTISSA_DIGITS / 4], *s;
	const char *prefix = "-0X+0X 0X-0x+0x 0x";
	char ebuf0[3 * sizeof(int)], *ebuf = &ebuf0[3 * sizeof(int)], *estr = ebuf;
	union
	{
		double value;
		unsigned long long bits;
	} parts;
	unsigned long long mantissa;
	int biased;

	parts.value = value;
	biased = (int)((parts.bits >> 52) & 0x7FF);
	mantissa = parts.bits & 0xFFFFFFFFFFFFFULL;
	pl = 1;
	if (parts.bits >> 63)
		;
	else if (flags & FORMAT_PLUS)
		prefix += 3;
	else if (flags & FORMAT_SPACE)
		prefix += 6;
	else
		prefix++, pl = 0;

	if (biased == 0x7FF)
	{
		const char *text = mantissa ? ((t & 32) ? "nan" : "NAN") : ((t & 32) ? "inf" : "INF");

		format_pad(output, ' ', width, 3 + pl, flags & ~FORMAT_ZERO);
		format_put(output, prefix, pl);
		format_put(output, text, 3);
		format_pad(output, ' ', width, 3 + pl, flags ^ FORMAT_LEFT);
		return;
	}
	if ((t | 32) == 'a')
		t = (t & 32) ? 'e' : 'E';
	if (p < 0)
		p = 6;

	/* value = y * 2^e2 with y = m / 2^24 in [2^28, 2^29), m the mantissa
	with its 53rd bit set (musl's frexpl, * 2, * 2^28) */
	e2 = 0;
	if (biased)
	{
		mantissa |= 1ULL << 52;
		e2 = biased - 1075 + 24;
	}
	else if (mantissa)
	{
		e2 = -1074 + 24;
		while (!(mantissa & (1ULL << 52)))
		{
			mantissa <<= 1;
			e2--;
		}
	}

	if (e2 < 0)
		a = r = z = big;
	else
		a = r = z = big + sizeof(big) / sizeof(*big) - MANTISSA_DIGITS - 1;

	/* y in limbs: its integer part, then its fraction (of 24 bits, whose
	expansion in base 10^9 ends within three limbs) */
	*z++ = (unsigned long)(mantissa >> 24);
	{
		unsigned long long fraction = mantissa & 0xFFFFFFULL;

		while (fraction)
		{
			unsigned long long scaled = fraction * 1000000000ULL;

			*z++ = (unsigned long)(scaled >> 24);
			fraction = scaled & 0xFFFFFFULL;
		}
	}

	while (e2 > 0)
	{
		unsigned long carry = 0;
		int sh = FORMAT_MINIMUM(29, e2);

		for (d = z - 1; d >= a; d--)
		{
			unsigned long long x = ((unsigned long long)*d << sh) + carry;

			*d = (unsigned long)(x % 1000000000);
			carry = (unsigned long)(x / 1000000000);
		}
		if (carry)
			*--a = carry;
		while (z > a && !z[-1])
			z--;
		e2 -= sh;
	}
	while (e2 < 0)
	{
		unsigned long carry = 0, *b;
		int sh = FORMAT_MINIMUM(9, -e2), need = 1 + (p + MANTISSA_DIGITS / 3 + 8) / 9;

		for (d = a; d < z; d++)
		{
			unsigned long rm = *d & ((1UL << sh) - 1);

			*d = (*d >> sh) + carry;
			carry = (1000000000UL >> sh) * rm;
		}
		if (!*a)
			a++;
		if (carry)
			*z++ = carry;
		/* no digits past the precision */
		b = (t | 32) == 'f' ? r : a;
		if (z - b > need)
			z = b + need;
		e2 += sh;
	}

	if (a < z)
	{
		for (i = 10, e = 9 * (r - a); *a >= (unsigned long)i; i *= 10, e++)
			;
	}
	else
		e = 0;

	/* rounding, half to even: j is the precision after the point */
	j = p - ((t | 32) != 'f') * e - ((t | 32) == 'g' && p);
	if (j < 9 * (z - r - 1))
	{
		unsigned long x;

		d = r + 1 + ((j + 9 * MAXIMUM_EXPONENT) / 9 - MAXIMUM_EXPONENT);
		j += 9 * MAXIMUM_EXPONENT;
		j %= 9;
		for (i = 10, j++; j < 9; i *= 10, j++)
			;
		x = *d % i;
		if (x || d + 1 != z)
		{
			int odd = ((*d / i) & 1) || (i == 1000000000 && d > a && (d[-1] & 1));
			int up = x > (unsigned long)i / 2 || (x == (unsigned long)i / 2 && (d + 1 != z || odd));

			*d -= x;
			if (up)
			{
				*d = *d + i;
				while (*d > 999999999)
				{
					*d-- = 0;
					if (d < a)
						*--a = 0;
					(*d)++;
				}
				for (i = 10, e = 9 * (r - a); *a >= (unsigned long)i; i *= 10, e++)
					;
			}
		}
		if (z > d + 1)
			z = d + 1;
	}
	for (; z > a && !z[-1]; z--)
		;

	if ((t | 32) == 'g')
	{
		if (!p)
			p++;
		if (p > e && e >= -4)
		{
			t--;
			p -= e + 1;
		}
		else
		{
			t -= 2;
			p--;
		}
		if (!(flags & FORMAT_ALTERNATE))
		{
			/* the trailing zeros of the last limb */
			if (z > a && z[-1])
			{
				for (i = 10, j = 0; z[-1] % i == 0; i *= 10, j++)
					;
			}
			else
				j = 9;
			if ((t | 32) == 'f')
				p = FORMAT_MINIMUM(p, FORMAT_MAXIMUM(0, 9 * (z - r - 1) - j));
			else
				p = FORMAT_MINIMUM(p, FORMAT_MAXIMUM(0, 9 * (z - r - 1) + e - j));
		}
	}
	l = 1 + p + (p || (flags & FORMAT_ALTERNATE));
	if ((t | 32) == 'f')
	{
		if (e > 0)
			l += e;
	}
	else
	{
		estr = format_unsigned(e < 0 ? -e : e, ebuf);
		while (ebuf - estr < 2)
			*--estr = '0';
		*--estr = (char)(e < 0 ? '-' : '+');
		*--estr = (char)t;
		l += (int)(ebuf - estr);
	}

	format_pad(output, ' ', width, pl + l, flags);
	format_put(output, prefix, pl);
	format_pad(output, '0', width, pl + l, flags ^ FORMAT_ZERO);

	if ((t | 32) == 'f')
	{
		if (a > r)
			a = r;
		for (d = a; d <= r; d++)
		{
			s = format_unsigned(*d, buf + 9);
			if (d != a)
			{
				while (s > buf)
					*--s = '0';
			}
			else if (s == buf + 9)
				*--s = '0';
			format_put(output, s, buf + 9 - s);
		}
		if (p || (flags & FORMAT_ALTERNATE))
			format_put(output, ".", 1);
		for (; d < z && p > 0; d++, p -= 9)
		{
			s = format_unsigned(*d, buf + 9);
			while (s > buf)
				*--s = '0';
			format_put(output, s, FORMAT_MINIMUM(9, p));
		}
		format_pad(output, '0', p + 9, 9, 0);
	}
	else
	{
		if (z <= a)
			z = a + 1;
		for (d = a; d < z && p >= 0; d++)
		{
			s = format_unsigned(*d, buf + 9);
			if (s == buf + 9)
				*--s = '0';
			if (d != a)
			{
				while (s > buf)
					*--s = '0';
			}
			else
			{
				format_put(output, s++, 1);
				if (p > 0 || (flags & FORMAT_ALTERNATE))
					format_put(output, ".", 1);
			}
			format_put(output, s, FORMAT_MINIMUM(buf + 9 - s, p));
			p -= (int)(buf + 9 - s);
		}
		format_pad(output, '0', p + 18, 18, 0);
		format_put(output, estr, ebuf - estr);
	}
	format_pad(output, ' ', width, pl + l, flags ^ FORMAT_LEFT);
}

/* ---------- MSVC's printf and PDCLib's

The arguments of a printf-family call, made into what PDCLib reads right:
MSVC's %I64 as %ll and %I32 dropped, a NULL string as "(null)" (MSVC's;
PDCLib reads through it), and each floating-point conversion formatted
above and passed as a string. The arguments are copied to an array of stack
words (cdecl on the x86: what a va_list points at). Returns the words as a
va_list and the format that goes with them in *prepared_format; when the
format is not understood, the call's own arguments and its format as
translate_format leaves it. */

#define MAXIMUM_ARGUMENT_WORDS 64
#define MAXIMUM_PREPARED_FORMAT 1024
#define MAXIMUM_NUMBER_TEXT 1024
/* digits beyond these are not asked for; they bound the numbers' text */
#define MAXIMUM_NUMBER_PRECISION 350

struct printf_preparation
{
	unsigned long words[MAXIMUM_ARGUMENT_WORDS];
	char format[MAXIMUM_PREPARED_FORMAT];
	char numbers[MAXIMUM_NUMBER_TEXT];
};

static const char null_text[] = "(null)";
static const wchar_t null_wide_text[] = L"(null)";

static int prepared_append(struct printf_preparation *preparation, int *length, const char *text, int count)
{
	if (*length + count >= MAXIMUM_PREPARED_FORMAT)
		return 0;
	memcpy(preparation->format + *length, text, count);
	*length += count;
	return 1;
}

/* whether a format has anything PDCLib reads wrong: a floating-point
conversion, a string (which may be NULL) or MSVC's I64/I32; most of the
game's and the platform's formats (integers and text) go to PDCLib as they
are, without the copy */
static int format_needs_preparing(const char *format)
{
	const char *cursor;

	for (cursor = strchr(format, '%'); cursor; cursor = strchr(cursor, '%'))
	{
		cursor++;
		if (*cursor == '%')
		{
			cursor++;
			continue;
		}
		while (*cursor && strchr("-+ #0'*.0123456789hljzqtLw", *cursor))
			cursor++;
		if (*cursor == 'I' || strchr("fFeEgGaAsS", *cursor))
			return 1;
		if (!*cursor)
			break;
	}
	return 0;
}

static va_list prepare_arguments(const char *format, va_list arguments, struct printf_preparation *preparation,
	const char **prepared_format)
{
	unsigned long *words = preparation->words;
	const char *cursor;
	int count = 0, length = 0, numbers_used = 0;
	va_list in;

	if (!format_needs_preparing(format))
	{
		*prepared_format = format;
		return arguments;
	}
	va_copy(in, arguments);
	for (cursor = format; *cursor; cursor++)
	{
		const char *specification = cursor;
		int first_word = count, wide = 0, longs = 0, width = 0, precision = -1;
		unsigned flags = 0;

		if (*cursor != '%')
		{
			/* the text up to the next conversion, whole */
			const char *next = strchr(cursor, '%');
			int run = next ? (int)(next - cursor) : (int)strlen(cursor);

			if (!prepared_append(preparation, &length, cursor, run))
				goto give_up;
			cursor += run - 1;
			continue;
		}
		cursor++;
		if (*cursor == '%')
		{
			if (!prepared_append(preparation, &length, "%%", 2))
				goto give_up;
			continue;
		}
		for (; *cursor && strchr("-+ #0'", *cursor); cursor++)
		{
			flags |= *cursor == '-' ? FORMAT_LEFT : *cursor == '+' ? FORMAT_PLUS : *cursor == ' ' ? FORMAT_SPACE :
				*cursor == '#' ? FORMAT_ALTERNATE : *cursor == '0' ? FORMAT_ZERO : 0;
		}
		if (*cursor == '*')
		{
			if (count >= MAXIMUM_ARGUMENT_WORDS)
				goto give_up;
			width = (int)(words[count++] = va_arg(in, unsigned long));
			if (width < 0)
			{
				flags |= FORMAT_LEFT;
				width = -width;
			}
			cursor++;
		}
		else
		{
			for (; *cursor >= '0' && *cursor <= '9'; cursor++)
				width = width * 10 + (*cursor - '0');
		}
		if (*cursor == '.')
		{
			cursor++;
			precision = 0;
			if (*cursor == '*')
			{
				if (count >= MAXIMUM_ARGUMENT_WORDS)
					goto give_up;
				precision = (int)(words[count++] = va_arg(in, unsigned long));
				if (precision < 0)
					precision = -1;
				cursor++;
			}
			else
			{
				for (; *cursor >= '0' && *cursor <= '9'; cursor++)
					precision = precision * 10 + (*cursor - '0');
			}
		}
		for (;; cursor++)
		{
			if (*cursor == 'l')
				longs++;
			else if (*cursor == 'j' || *cursor == 'L' || *cursor == 'q')
				longs = 2;
			else if (*cursor == 'w')
				wide = 1;
			else if (cursor[0] == 'I' && cursor[1] == '6' && cursor[2] == '4')
			{
				longs = 2;
				cursor += 2;
			}
			else if (cursor[0] == 'I' && cursor[1] == '3' && cursor[2] == '2')
				cursor += 2;
			else if (*cursor != 'h' && *cursor != 'z' && *cursor != 't')
				break;
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
		case 's': case 'S':
		{
			unsigned long pointer = va_arg(in, unsigned long);

			if (!pointer)
				pointer = (longs || wide || *cursor == 'S') ? (unsigned long)null_wide_text : (unsigned long)null_text;
			words[count++] = pointer;
			break;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
		{
			/* (a long double is a double on this target) */
			double value = va_arg(in, double);
			struct format_output output;

			if (numbers_used >= MAXIMUM_NUMBER_TEXT)
				goto give_up;
			output.text = preparation->numbers + numbers_used;
			output.size = MAXIMUM_NUMBER_TEXT - numbers_used;
			output.length = 0;
			output.text[0] = 0;
			if (flags & FORMAT_LEFT)
				flags &= ~FORMAT_ZERO;
			format_double(&output, value, width, FORMAT_MINIMUM(precision, MAXIMUM_NUMBER_PRECISION), flags,
				*cursor);
			/* the text stands for the conversion, its * width and precision too */
			count = first_word;
			words[count++] = (unsigned long)output.text;
			numbers_used += output.length + 1;
			if (!prepared_append(preparation, &length, "%s", 2))
				goto give_up;
			continue;
		}
		default:
			goto give_up;
		}
		/* the conversion as written, with MSVC's length modifiers translated */
		for (; specification <= cursor; specification++)
		{
			if (specification[0] == 'I' && specification[1] == '6' && specification[2] == '4')
			{
				if (!prepared_append(preparation, &length, "ll", 2))
					goto give_up;
				specification += 2;
			}
			else if (specification[0] == 'I' && specification[1] == '3' && specification[2] == '2')
				specification += 2;
			else if (!prepared_append(preparation, &length, specification, 1))
				goto give_up;
		}
	}
	va_end(in);
	preparation->format[length] = 0;
	*prepared_format = preparation->format;
	return (va_list)words;
give_up:
	va_end(in);
	*prepared_format = translate_format(format, preparation->format, sizeof(preparation->format));
	return arguments;
}

int halo_xbox_vsnprintf(char *buffer, size_t count, const char *format, va_list arguments)
{
	struct printf_preparation preparation;
	va_list prepared = prepare_arguments(format, arguments, &preparation, &format);

	return vsnprintf(buffer, count, format, prepared);
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
	struct printf_preparation preparation;
	va_list prepared = prepare_arguments(format, arguments, &preparation, &format);

	return vsprintf(buffer, format, prepared);
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
		struct printf_preparation preparation;
		va_list prepared = prepare_arguments(format, arguments, &preparation, &format);

		return vfprintf(stream, format, prepared);
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
