/*
N64_FILES.C

Read-only files on the cartridge (libdragon's DragonFS, "rom:/..."), for
the XDK-facing file layer (xdk_kernel.c) and the map loader.
*/

#include <libdragon.h>
#include <stdio.h>

#include "n64.h"

#define MAXIMUM_FILES 8

static FILE *files[MAXIMUM_FILES];

long n64_file_open(const char *path)
{
	for (int i = 0; i < MAXIMUM_FILES; i++)
	{
		if (!files[i])
		{
			files[i] = fopen(path, "rb");
			return files[i] ? i : -1;
		}
	}
	return -1;
}

long n64_file_size(long handle)
{
	FILE *file = files[handle];
	long position = ftell(file);
	long size;

	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, position, SEEK_SET);
	return size;
}

long n64_file_read(long handle, void *buffer, long size)
{
	return (long)fread(buffer, 1, size, files[handle]);
}

long n64_file_seek(long handle, long offset)
{
	return fseek(files[handle], offset, SEEK_SET) == 0 ? offset : -1;
}

void n64_file_close(long handle)
{
	if (handle >= 0 && handle < MAXIMUM_FILES && files[handle])
	{
		fclose(files[handle]);
		files[handle] = NULL;
	}
}

int n64_debug_flag(const char *name)
{
	static struct { const char *name; int value; } cache[16];
	static int count;
	char path[64];
	FILE *file;

	for (int i = 0; i < count; i++)
	{
		if (cache[i].name == name)
			return cache[i].value;
	}
	snprintf(path, sizeof(path), "rom:/%s", name);
	file = fopen(path, "rb");
	if (file)
		fclose(file);
	if (count < 16)
	{
		cache[count].name = name;
		cache[count].value = file != NULL;
		count++;
	}
	return file != NULL;
}
