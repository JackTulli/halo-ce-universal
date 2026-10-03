/*
XBOX_MAP_REBASE_TEST.C

Host harness for port/linux/src/xbox_map_rebase.c, built and run by
tools/xbox_map_layouts.py --check. It reads a list of jobs, one per line:

  tags|bsp DATA LINKED DELTA EXPECTED LABEL [OFFSET VALUE]

DATA is the tag data or a structure BSP as read from a map, LINKED the
address it was linked to (hexadecimal), DELTA the displacement (decimal),
EXPECTED the byte offsets of the words that must move (little-endian 32-bit
words). The harness rebases a copy and checks that exactly those words
changed, each by DELTA, and that nothing outside the data was written. With
OFFSET and VALUE, it first stores VALUE at OFFSET and expects the walker to
refuse the data with a sentence instead. One line of results per job.

The tool's jobs are every region of every map as read, then copies in which
the fields no retail map uses hold pointers (the declared fields; the
pointers written are in EXPECTED too), then damaged copies of both.
*/

#include "xbox_map_rebase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define GUARD_SIZE 64
#define GUARD_BYTE 0xA5
#define TIMED_RUNS 3

typedef char unsigned_long_size_assert[sizeof(unsigned long) == 4 ? 1 : -1];

static double seconds_now(void)
{
#ifdef _WIN32
	LARGE_INTEGER counter, frequency;

	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec + now.tv_nsec / 1e9;
#endif
}

static unsigned char *read_file(const char *path, unsigned long *size)
{
	FILE *file = fopen(path, "rb");
	unsigned char *data;
	long length;

	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = (unsigned char *)malloc(length > 0 ? (size_t)length : 1);
	if (data && length > 0 && fread(data, 1, (size_t)length, file) != (size_t)length)
	{
		free(data);
		data = NULL;
	}
	fclose(file);
	*size = (unsigned long)length;
	return data;
}

static unsigned long read_word(const unsigned char *p)
{
	return p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int run_job(const char *kind, const char *data_path, unsigned long linked, long delta,
	const char *expected_path, const char *label, int poke, unsigned long poke_offset, unsigned long poke_value)
{
	unsigned long size, expected_size, k, moved = 0, expected_count;
	unsigned char *original = read_file(data_path, &size);
	unsigned char *expected_bytes = read_file(expected_path, &expected_size);
	unsigned char *buffer, *work, *wanted;
	char error[512];
	double best = 1e30;
	int run, result = 0, ok = 1;

	if (!original || !expected_bytes)
	{
		printf("%s FAIL cannot read %s\n", label, original ? expected_path : data_path);
		return 0;
	}
	expected_count = expected_size / 4;
	/* one flag per word: must move */
	wanted = (unsigned char *)calloc(size / 4 + 1, 1);
	buffer = (unsigned char *)malloc(size + 2 * GUARD_SIZE);
	if (!wanted || !buffer)
	{
		printf("%s FAIL out of memory\n", label);
		return 0;
	}
	for (k = 0; k < expected_count; k++)
	{
		unsigned long offset = read_word(expected_bytes + 4 * k);

		if (offset & 3 || offset + 4 > size)
		{
			printf("%s FAIL expected offset 0x%lX is outside the data\n", label, offset);
			return 0;
		}
		wanted[offset / 4] = 1;
	}
	if (poke)
	{
		original[poke_offset + 0] = (unsigned char)poke_value;
		original[poke_offset + 1] = (unsigned char)(poke_value >> 8);
		original[poke_offset + 2] = (unsigned char)(poke_value >> 16);
		original[poke_offset + 3] = (unsigned char)(poke_value >> 24);
	}
	work = buffer + GUARD_SIZE;
	for (run = 0; run < (poke ? 1 : TIMED_RUNS); run++)
	{
		double start;

		memset(buffer, GUARD_BYTE, size + 2 * GUARD_SIZE);
		memcpy(work, original, size);
		start = seconds_now();
		if (!strcmp(kind, "tags"))
			result = xbox_map_rebase_tag_data(work, size, linked, delta, error, sizeof(error));
		else
			result = xbox_map_rebase_structure_bsp(work, size, linked, delta, error, sizeof(error));
		start = seconds_now() - start;
		if (start < best)
			best = start;
		if (!result)
			break;
	}
	if (poke)
	{
		if (result)
			printf("%s FAIL the walker accepted the damaged data\n", label);
		else
			printf("%s refused: %s\n", label, error);
		return !result;
	}
	if (!result)
	{
		printf("%s FAIL %s\n", label, error);
		return 0;
	}
	for (k = 0; k < GUARD_SIZE; k++)
	{
		if (buffer[k] != GUARD_BYTE || work[size + k] != GUARD_BYTE)
		{
			printf("%s FAIL the walker wrote outside the data\n", label);
			return 0;
		}
	}
	for (k = 0; k + 4 <= size; k += 4)
	{
		unsigned long before = read_word(original + k);
		unsigned long after = read_word(work + k);

		if (wanted[k / 4])
		{
			moved++;
			if (after != ((before + (unsigned long)delta) & 0xFFFFFFFFUL))
			{
				if (ok)
					printf("%s FAIL the pointer at +0x%lX went from 0x%08lX to 0x%08lX\n", label, k, before, after);
				ok = 0;
			}
		}
		else if (after != before)
		{
			if (ok)
				printf("%s FAIL the word at +0x%lX went from 0x%08lX to 0x%08lX\n", label, k, before, after);
			ok = 0;
		}
	}
	for (; k < size; k++)
	{
		if (original[k] != work[k])
		{
			if (ok)
				printf("%s FAIL the byte at +0x%lX changed\n", label, k);
			ok = 0;
		}
	}
	if (ok)
		printf("%s ok %lu words moved, %.3f ms\n", label, moved, best * 1000.0);
	free(original);
	free(expected_bytes);
	free(wanted);
	free(buffer);
	return ok;
}

int main(int argc, char **argv)
{
	FILE *jobs;
	char line[2048];
	int failures = 0;

	if (argc != 2 || !(jobs = fopen(argv[1], "r")))
	{
		fprintf(stderr, "usage: xbox_map_rebase_test JOBS\n");
		return 2;
	}
	while (fgets(line, sizeof(line), jobs))
	{
		char kind[16], data_path[1024], expected_path[1024], label[256];
		unsigned long linked, poke_offset, poke_value;
		long delta;
		int fields = sscanf(line, "%15s %1023s %lx %ld %1023s %255s %lx %lx", kind, data_path, &linked, &delta,
			expected_path, label, &poke_offset, &poke_value);

		if (fields < 6)
			continue;
		if (!run_job(kind, data_path, linked, delta, expected_path, label, fields == 8, poke_offset, poke_value))
			failures++;
		fflush(stdout);
	}
	fclose(jobs);
	return failures ? 1 : 0;
}
