/*
N64_ANIM.C

Animation frames streamed from the cartridge. The converter moves every
animation's frame data out of the map (tools/n64/convert.py) into
rom:/maps/<map>.anim; the animation's tag data keeps its size, a flag and
its offset in that file. A frame is read with the 4 KB chunk around it into
a small least-recently-used cache, so the frames that follow (the next
ticks') are already there.
*/

#include <libdragon.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "n64.h"

#define CHUNK_SIZE 4096
#define CHUNK_COUNT 48

struct chunk
{
	long start; /* file offset of the chunk's first byte, or -1 */
	unsigned long last_used;
	uint8_t *data;
};

static struct
{
	uint32_t rom_address;
	long file_size;
	struct chunk chunks[CHUNK_COUNT];
	unsigned long clock;
	unsigned long misses, requests;
} anim;

void n64_animation_open(const char *map_name)
{
	char path[64];
	FILE *file;

	snprintf(path, sizeof(path), "rom:/maps/%s.anim", map_name);
	anim.rom_address = dfs_rom_addr(path + 5);
	file = fopen(path, "rb");
	anim.file_size = 0;
	if (file)
	{
		fseek(file, 0, SEEK_END);
		anim.file_size = ftell(file);
		fclose(file);
	}
	for (int i = 0; i < CHUNK_COUNT; i++)
	{
		if (!anim.chunks[i].data)
			anim.chunks[i].data = memalign(16, CHUNK_SIZE);
		anim.chunks[i].start = -1;
	}
	debugf("n64: animation frames %s: %ld KB streamed, %d KB cache\n", path, anim.file_size / 1024,
		CHUNK_COUNT * CHUNK_SIZE / 1024);
}

/* a frame of a streamed animation: its tag data's file offset and size, the
frame and the frame size */
void *n64_animation_frame(long data_offset, long data_size, short frame_index, short frame_size)
{
	long offset = data_offset + (long)frame_index * frame_size;
	long start;
	struct chunk *chunk = NULL;
	struct chunk *oldest = &anim.chunks[0];

	anim.requests++;
	anim.clock++;
	/* a chunk that holds the whole frame */
	for (int i = 0; i < CHUNK_COUNT; i++)
	{
		struct chunk *c = &anim.chunks[i];

		if (c->start >= 0 && offset >= c->start && offset + frame_size <= c->start + CHUNK_SIZE)
		{
			chunk = c;
			break;
		}
		if (c->last_used < oldest->last_used)
			oldest = c;
	}
	if (!chunk)
	{
		long length;

		anim.misses++;
		chunk = oldest;
		/* from the frame on, as much of the animation as fits */
		start = offset & ~1L;
		length = data_offset + data_size - start;
		if (length > CHUNK_SIZE)
			length = CHUNK_SIZE;
		if (start + length > anim.file_size)
			length = anim.file_size - start;
		data_cache_hit_writeback_invalidate(chunk->data, CHUNK_SIZE);
		dma_read(chunk->data, anim.rom_address + start, (length + 1) & ~1L);
		chunk->start = start;
	}
	chunk->last_used = anim.clock;
	if ((anim.requests & 4095) == 0)
	{
		debugf("anim: %lu requests, %lu%% read from the cartridge\n", anim.requests, anim.misses * 100 / anim.requests);
		anim.misses = anim.requests = 0;
	}
	return chunk->data + (offset - chunk->start);
}
