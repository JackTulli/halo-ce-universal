/*
N64_PARTICLES.C

Halo's particles, simply: what an effect would have made (a muzzle flash,
sparks, dust, blood, an explosion's fireball) as a small pool of sprites
that fly in straight lines (gravity and drag their only physics), grow,
fade in and out and are drawn as rectangles facing the camera.

The effects' particle lists are not in the map: tools/n64/effects.py
writes each map's as records of its own in rom:/fx/<map>.fx, with a 128x64
I4 atlas of the particles' pictures (16x16 each, tinted by the particle's
colour). The index of the records and the atlas stay in RAM; a record is
read from the cartridge when its effect plays, into a small cache
(n64_fx_event, asked for by source/effects/effects.c, HALO_N64).

Drawing (n64_particles_draw, from n64_render.c): each particle is placed
on the screen by the view's camera and drawn as a textured rectangle with
the depth tiny3d's triangles would have there (rdpq's Z-override: tested,
not written), far ones first; the first-person ones (a muzzle flash on the
player's own gun) are kept in the camera's space and drawn after the
first-person weapon.
*/

#include <libdragon.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "n64.h"

#define MAXIMUM_PARTICLES 128
#define CELL 16
#define CELLS 32
#define ATLAS_COLUMNS 8
#define ATLAS_BYTES 4096
#define CACHE_ENTRIES 12
#define SPAWNS_PER_SECOND 300.f
#define SPAWN_BURST 64.f

struct particle
{
	float position[3], velocity[3];
	float age, life, fade_in, fade_out;
	float radius, end_scale;
	float gravity, drag;
	uint32_t color; /* RGBA */
	uint8_t cell, flags;
	int8_t local_player;
	uint8_t live;
};

struct fx_index
{
	uint16_t tag_index, size;
	uint32_t offset;
};

static struct
{
	struct particle particles[MAXIMUM_PARTICLES];
	int next;
	int live;
	/* the map's records: the index, the file's cartridge address, the atlas */
	struct fx_index *index;
	int index_count;
	uint32_t rom;
	surface_t atlas;
	uint8_t cell_colours[CELLS][4];
	struct
	{
		int tag_index;
		uint8_t *data;
		unsigned long used;
	} cache[CACHE_ENTRIES];
	unsigned long clock;
	/* the spawn budget, and its share (half with three or four views: each
	particle is drawn in every view) */
	float tokens, budget;
	/* statistics */
	unsigned long spawned, drawn, reads, refused;
} fx;

static void fx_free(void)
{
	for (int i = 0; i < CACHE_ENTRIES; i++)
	{
		free(fx.cache[i].data);
		fx.cache[i].data = NULL;
		fx.cache[i].tag_index = -1;
	}
	free(fx.index);
	fx.index = NULL;
	fx.index_count = 0;
	if (fx.atlas.buffer)
		surface_free(&fx.atlas);
	fx.rom = 0;
}

void n64_particles_load(const char *map_name)
{
	char path[48];
	uint8_t header[20];
	FILE *file;

	fx_free();
	memset(fx.particles, 0, sizeof(fx.particles));
	fx.live = 0;
	snprintf(path, sizeof(path), "fx/%s.fx", map_name);
	fx.rom = dfs_rom_addr(path);
	snprintf(path, sizeof(path), "rom:/fx/%s.fx", map_name);
	file = fopen(path, "rb");
	if (!file || !fx.rom)
	{
		if (file)
			fclose(file);
		debugf("n64: no particles for %s\n", map_name);
		fx.rom = 0;
		return;
	}
	if (fread(header, 1, sizeof(header), file) == sizeof(header) && !memcmp(header, "N64F", 4))
	{
		int count = header[8] << 8 | header[9];
		uint32_t index_offset = (uint32_t)header[12] << 24 | header[13] << 16 | header[14] << 8 | header[15];
		uint32_t atlas_offset = (uint32_t)header[16] << 24 | header[17] << 16 | header[18] << 8 | header[19];

		fx.index = malloc(sizeof(struct fx_index) * (count ? count : 1));
		fseek(file, index_offset, SEEK_SET);
		if (fx.index && (int)fread(fx.index, sizeof(struct fx_index), count, file) == count)
			fx.index_count = count;
		fx.atlas = surface_alloc(FMT_I4, ATLAS_COLUMNS * CELL, ATLAS_BYTES * 2 / (ATLAS_COLUMNS * CELL));
		fseek(file, atlas_offset, SEEK_SET);
		if (fx.atlas.buffer)
		{
			fread(fx.atlas.buffer, 1, ATLAS_BYTES, file);
			data_cache_hit_writeback(fx.atlas.buffer, ATLAS_BYTES);
		}
		/* each cell's colour, after the atlas */
		fread(fx.cell_colours, 4, CELLS, file);
	}
	fclose(file);
	debugf("n64: particles for %d effects of %s\n", fx.index_count, map_name);
}

/* an effect's record (by binary search of the index; read from the
cartridge into the cache), or NULL */
static const uint8_t *fx_record(int tag_index)
{
	int low = 0, high = fx.index_count - 1, slot = 0;

	fx.clock++;
	for (int i = 0; i < CACHE_ENTRIES; i++)
	{
		if (fx.cache[i].data && fx.cache[i].tag_index == tag_index)
		{
			fx.cache[i].used = fx.clock;
			return fx.cache[i].data;
		}
	}
	/* (a free entry, else the least recently used) */
	for (int i = 0; i < CACHE_ENTRIES; i++)
	{
		if (!fx.cache[i].data)
		{
			slot = i;
			break;
		}
		if (fx.cache[i].used < fx.cache[slot].used)
			slot = i;
	}
	while (low <= high)
	{
		int middle = (low + high) / 2;
		struct fx_index const *entry = &fx.index[middle];

		if (entry->tag_index == tag_index)
		{
			int size = (entry->size + 15) & ~15;

			free(fx.cache[slot].data);
			fx.cache[slot].data = memalign(16, size);
			if (!fx.cache[slot].data)
				return NULL;
			data_cache_hit_writeback_invalidate(fx.cache[slot].data, size);
			dma_read(fx.cache[slot].data, fx.rom + entry->offset, size);
			fx.cache[slot].tag_index = tag_index;
			fx.cache[slot].used = fx.clock;
			fx.reads++;
			return fx.cache[slot].data;
		}
		if (entry->tag_index < tag_index)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return NULL;
}

const struct n64_fx_emitter *n64_fx_event(int effect_tag_index, int event_index, int *count)
{
	const uint8_t *record;
	const uint16_t *head;
	int events, first, last;

	*count = 0;
	if (!fx.index_count || !(record = fx_record(effect_tag_index)))
		return NULL;
	head = (const uint16_t *)record;
	events = head[0];
	if (event_index < 0 || event_index >= events)
		return NULL;
	first = head[2 + event_index];
	last = head[3 + event_index];
	*count = last - first;
	return (const struct n64_fx_emitter *)(record + ((4 + 2 * (events + 1) + 3) & ~3)) + first;
}

void n64_particle_spawn(const struct n64_particle_spawn *spawn)
{
	struct particle *p = &fx.particles[fx.next];

	/* (a budget: ~300 a second, bursts of up to 64 -- a muzzle flash or an
	impact gets through, a stream cannot fill the pool) */
	if (fx.tokens < 1.f)
	{
		fx.refused++;
		return;
	}
	fx.tokens -= 1.f;

	/* (full: the oldest goes) */
	fx.next = (fx.next + 1) % MAXIMUM_PARTICLES;
	if (!p->live)
		fx.live++;
	memcpy(p->position, spawn->position, sizeof(p->position));
	memcpy(p->velocity, spawn->velocity, sizeof(p->velocity));
	p->age = 0.f;
	/* (a muzzle flash lives ~30 ms, less than a frame: at least 80, so that
	it is drawn at all) */
	p->life = spawn->life > 0.08f ? spawn->life : 0.08f;
	p->fade_in = spawn->fade_in;
	p->fade_out = spawn->fade_out;
	p->radius = spawn->radius;
	p->end_scale = spawn->end_scale;
	p->gravity = spawn->gravity;
	p->drag = spawn->drag;
	p->color = spawn->color;
	p->cell = spawn->cell;
	p->flags = spawn->flags;
	p->local_player = spawn->local_player;
	p->live = 1;
	fx.spawned++;
}

void n64_particles_update(float dt)
{
	static float log_time;

	/* (port.json flag "debug_particles": the counts every five seconds) */
	log_time += dt;
	if (log_time >= 5.f)
	{
		log_time = 0.f;
		if (n64_debug_flag("debug_particles"))
			debugf("particles: %lu made, %lu refused, %lu drawn, %lu records read, %d live\n", fx.spawned,
				fx.refused, fx.drawn, fx.reads, fx.live);
	}
	fx.tokens += dt * SPAWNS_PER_SECOND * fx.budget;
	if (fx.tokens > SPAWN_BURST * fx.budget)
		fx.tokens = SPAWN_BURST * fx.budget;
	if (!fx.live || dt <= 0.f)
		return;
	if (dt > 0.25f)
		dt = 0.25f;
	for (int i = 0; i < MAXIMUM_PARTICLES; i++)
	{
		struct particle *p = &fx.particles[i];
		float keep;

		if (!p->live)
			continue;
		p->age += dt;
		if (p->age >= p->life)
		{
			p->live = 0;
			fx.live--;
			continue;
		}
		if (!(p->flags & N64_PARTICLE_FIRST_PERSON))
			p->velocity[2] -= p->gravity * dt;
		keep = 1.f - p->drag * dt;
		if (keep < 0.f)
			keep = 0.f;
		for (int k = 0; k < 3; k++)
		{
			p->velocity[k] *= keep;
			p->position[k] += p->velocity[k] * dt;
		}
	}
}

struct drawn
{
	float z;
	float x0, y0, x1, y1;
	uint32_t color;
	uint16_t depth;
	uint8_t cell;
};

static int compare_drawn(const void *a, const void *b)
{
	float za = ((const struct drawn *)a)->z, zb = ((const struct drawn *)b)->z;

	return za < zb ? 1 : za > zb ? -1 : 0;
}

void n64_particles_draw(const struct n64_particle_view *view)
{
	static struct drawn drawn[MAXIMUM_PARTICLES];
	int count = 0;
	float m22, m32;

	if (!fx.live || !fx.atlas.buffer)
		return;
	/* tiny3d's depth: OpenGL's projection, then (ndc / 2 + 1/2) x 0x7FFF */
	m22 = (view->z_far + view->z_near) / (view->z_near - view->z_far);
	m32 = -2.f * view->z_far * view->z_near / (view->z_far - view->z_near);
	for (int i = 0; i < MAXIMUM_PARTICLES; i++)
	{
		struct particle const *p = &fx.particles[i];
		struct drawn *d = &drawn[count];
		float x, y, z, radius, alpha, scale, depth;
		int first_person = p->flags & N64_PARTICLE_FIRST_PERSON;

		if (!p->live || first_person != (view->first_person ? N64_PARTICLE_FIRST_PERSON : 0))
			continue;
		if (first_person ? p->local_player != view->local_player :
			(p->flags & N64_PARTICLE_THIRD_PERSON_ONLY) && p->local_player == view->local_player)
			continue;
		if (first_person)
		{
			x = p->position[0];
			y = p->position[1];
			z = p->position[2];
		}
		else
		{
			float dx = p->position[0] - view->eye[0];
			float dy = p->position[1] - view->eye[1];
			float dz = p->position[2] - view->eye[2];

			x = dx * view->right[0] + dy * view->right[1] + dz * view->right[2];
			y = dx * view->up[0] + dy * view->up[1] + dz * view->up[2];
			z = dx * view->forward[0] + dy * view->forward[1] + dz * view->forward[2];
		}
		if (z < view->z_near * 2.f || (view->fog_end > 0.f && z > view->fog_end))
			continue;
		radius = p->radius * (1.f + (p->end_scale - 1.f) * (p->age / p->life));
		scale = view->pixels_per_unit / z;
		d->x0 = view->center_x + x * scale;
		d->y0 = view->center_y - y * scale;
		radius *= scale;
		if (radius < 0.4f || d->x0 + radius < view->x0 || d->x0 - radius > view->x1 ||
			d->y0 + radius < view->y0 || d->y0 - radius > view->y1)
			continue;
		alpha = 1.f;
		if (p->fade_in > 0.f && p->age < p->fade_in)
			alpha = p->age / p->fade_in;
		if (p->fade_out > 0.f && p->life - p->age < p->fade_out)
			alpha *= (p->life - p->age) / p->fade_out;
		if (view->fog_end > 0.f && z > view->fog_start)
			alpha *= 1.f - (z - view->fog_start) / (view->fog_end - view->fog_start);
		alpha *= (p->flags & N64_PARTICLE_ADDITIVE) ? 1.f : (p->color & 0xFF) * (1.f / 255.f);
		if (alpha < 0.02f)
			continue;
		/* (a hair nearer than the surface it sits on) */
		depth = (-m22 + m32 / (z * 0.995f)) * 0.5f + 0.5f;
		if (depth < 0.f)
			depth = 0.f;
		d->depth = depth >= 1.f ? 0x7FFF : (uint16_t)(depth * 0x7FFF);
		d->x1 = d->x0 + radius;
		d->y1 = d->y0 + radius;
		d->x0 -= radius;
		d->y0 -= radius;
		if (view->chunky)
		{
			/* (the view is drawn half as wide, then widened) */
			d->x0 = view->x0 + (d->x0 - view->x0) * 0.5f;
			d->x1 = view->x0 + (d->x1 - view->x0) * 0.5f;
		}
		d->z = z;
		{
			/* the tint times the cell's own colour */
			uint8_t const *hue = fx.cell_colours[p->cell & (CELLS - 1)];
			uint32_t r = ((p->color >> 24) * hue[0]) / 255, g = (((p->color >> 16) & 0xFF) * hue[1]) / 255;
			uint32_t b = (((p->color >> 8) & 0xFF) * hue[2]) / 255;

			d->color = r << 24 | g << 16 | b << 8 | (uint32_t)(alpha * 255.f);
		}
		d->cell = p->cell;
		count++;
	}
	if (!count)
		return;
	qsort(drawn, count, sizeof(drawn[0]), compare_drawn);
	rdpq_sync_pipe();
	rdpq_set_mode_standard();
	rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (TEX0, 0, PRIM, 0)));
	rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
	rdpq_mode_zbuf(true, false);
	rdpq_mode_zoverride(true, 0.f, 0);
	rdpq_mode_filter(FILTER_BILINEAR);
	rdpq_tex_upload(TILE0, &fx.atlas, NULL);
	for (int i = 0; i < count; i++)
	{
		struct drawn const *d = &drawn[i];
		int s = (d->cell % ATLAS_COLUMNS) * CELL, t = (d->cell / ATLAS_COLUMNS) * CELL;

		rdpq_set_prim_color(RGBA32(d->color >> 24, (d->color >> 16) & 0xFF, (d->color >> 8) & 0xFF, d->color & 0xFF));
		rdpq_set_prim_depth_raw(d->depth, 0);
		rdpq_texture_rectangle_scaled(TILE0, d->x0, d->y0, d->x1, d->y1, s, t, s + CELL, t + CELL);
	}
	rdpq_mode_zoverride(false, 0.f, 0);
	fx.drawn += count;
}

void n64_particles_statistics(unsigned long *spawned, unsigned long *drawn, unsigned long *reads, int *live)
{
	*spawned = fx.spawned;
	*drawn = fx.drawn;
	*reads = fx.reads;
	*live = fx.live;
}

void n64_particles_set_view_count(int views)
{
	fx.budget = views > 2 ? 0.5f : 1.f;
}
