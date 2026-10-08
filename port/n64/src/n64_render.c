/*
N64_RENDER.C

The N64 renderer (n64.h): tiny3d on the RSP, libdragon's rdpq for the RDP.

The level is drawn from its converted render geometry (tools/n64/level.py:
rom:/levels/<map>.lvl): batches of at most 70 vertices with their
triangles, grouped by BSP cluster; a cluster outside a view's frustum is
skipped. Vertices are stored in the game's world units * 64 (the "N64
units" everything here is drawn in), already lit.
*/

#include <libdragon.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#include "n64.h"

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define MAXIMUM_VIEWS 4
#define WORLD_SCALE 64.0f
/* an object smaller than this on screen is drawn in its far version */
#define FAR_PIXELS 24.0f
#ifndef GUARD_BAND_SCALE
#define GUARD_BAND_SCALE 4
#endif

enum
{
	MATERIAL_ALPHA_TEST = 1,
	MATERIAL_TWO_SIDED = 2,
	MATERIAL_TRANSPARENT = 4,
	MATERIAL_UNTEXTURED = 8,
	/* a model texture whose palette's upper half is a player's colour
	(tools/n64/level.py split_palette) */
	MATERIAL_CHANGE_COLOR = 16,
	/* a transparent model shader drawn as a cut-out glow, unlit */
	MATERIAL_GLOW = 32,
};

/* ---------- the level file */

struct level_header
{
	char magic[4];
	uint32_t version;
	float scale;
	uint16_t texture_count, material_count, cluster_count, batch_count;
	uint32_t textures_offset, materials_offset;
	uint32_t clusters_offset, batches_offset;
	uint32_t vertices_offset, vertices_size;
	uint32_t indices_offset, indices_size;
};

struct level_texture
{
	char name[24];
	uint16_t width, height;
};

struct level_material
{
	int16_t texture;
	uint16_t flags;
};

struct level_cluster
{
	float minimum[3], maximum[3];
	uint16_t first_batch, batch_count;
};

struct level_batch
{
	uint16_t material;
	uint8_t vertex_count, triangle_count;
	uint32_t vertex_pair_offset;
	uint32_t index_offset;
	int16_t minimum[3], maximum[3];
} __attribute__((packed));

static struct
{
	uint8_t *data;
	struct level_header *header;
	struct level_texture *textures;
	struct level_material *materials;
	struct level_cluster *clusters;
	struct level_batch *batches;
	T3DVertPacked *vertices;
	uint8_t *indices;
	sprite_t **sprites;
	/* each batch's vertex load and triangles, recorded once */
	rspq_block_t **blocks;
} level;

/* ---------- the models file (tools/n64/models.py) */

#define MODEL_SCALE 1024.0f
#define MATRICES_PER_FRAME 640
#define FRAMES_IN_FLIGHT 3

struct models_header
{
	char magic[4];
	uint32_t version;
	uint16_t texture_count, model_count, region_count, permutation_count;
	uint16_t geometry_count, batch_count, segment_count, pad;
	uint32_t textures_offset, models_offset, regions_offset, permutations_offset;
	uint32_t geometries_offset, batches_offset, segments_offset;
	uint32_t vertices_offset, indices_offset;
};

struct model_record
{
	uint16_t tag_index;
	uint8_t node_count, region_count;
	uint16_t first_region, pad;
	uint16_t first_batch, batch_count;
	/* the model's vertices, then its indices, in the file */
	uint32_t data_offset, vertex_bytes, index_bytes;
	/* its far version's batches (after first_batch), or none */
	uint16_t far_first, far_count;
};
struct region_record { uint16_t permutation_count, first_permutation; };
struct geometry_record { uint16_t first_batch, batch_count; };
struct model_batch
{
	int16_t texture;
	uint16_t flags;
	uint8_t segment_count, triangle_count;
	uint16_t first_segment;
	uint32_t index_offset;
};
struct model_segment { uint8_t node, slot, count, pad; uint32_t vertex_pair_offset; };

/* Models and their textures are read from the cartridge when first drawn
and dropped, least recently drawn first, past this size (a model drawn in
the last frames in flight stays). A texture stays while a loaded model
uses it: each batch's texture and render mode are recorded with it. */
#define MODEL_CACHE_BYTES (448 * 1024)

struct loaded_model
{
	/* its vertices and indices; each batch's setup (render mode, texture)
	and its loads (node matrices from the segment the object sets) and
	triangles, recorded once */
	uint8_t *data;
	rspq_block_t **setups, **blocks;
	unsigned long last_drawn;
};

static struct
{
	uint8_t *data; /* the tables */
	struct models_header *header;
	struct level_texture *textures;
	struct model_record *models;
	struct region_record *regions;
	int16_t *permutations;
	struct geometry_record *geometries;
	struct model_batch *batches;
	struct model_segment *segments;
	char path[64], map_name[32];
	struct loaded_model *loaded;
	unsigned long loaded_bytes;
	sprite_t **sprites;
	unsigned short *texture_users;
	unsigned long *texture_size;
	unsigned long texture_bytes;
	/* tag index -> model record, or NULL */
	struct model_record **by_tag;
	int tag_count;
	unsigned long reads, texture_reads;
} models;

#define SEGMENT_NODE_MATRICES T3D_SEGMENT_1

/* debug switches: files on the cartridge (port.json "flags") */
static int debug_overview;
/* debugging: wait for the RSP and RDP after each part of a view, so that
the parts' times include the hardware's (and the frame is slower) */
static int debug_gpu_time;

static void gpu_time_sync(void)
{
	if (debug_gpu_time)
	{
		rdpq_fence();
		rspq_wait();
	}
}

/* the parts of a view, timed: the time since the last change goes to the
part that was under way (with debug_gpu_time, once the RSP and RDP are
done with it) */
enum { PHASE_NONE, PHASE_LEVEL, PHASE_OBJECTS, PHASE_FIRST_PERSON, PHASE_HUD };
static void render_phase(int phase);
static int debug_untextured;

static struct
{
	T3DViewport viewports[MAXIMUM_VIEWS];
	T3DMat4FP *identity;
	int view_count;
	surface_t *surface;
	unsigned long frame_index;
	unsigned long triangles, batches, clusters, objects;
	/* microseconds of CPU in the level, objects and first-person models,
	and frames, over the statistics window */
	unsigned long long level_ticks, object_ticks, first_person_ticks, draw_ticks, window_start;
	unsigned long long hud_ticks;
	int phase;
	unsigned long phase_start;
	int first_person;
	/* the view's eye (world units) and how many pixels a unit at distance
	one covers, for leaving out objects too small to see */
	float eye[3], pixels_per_unit;
	T3DMat4FP *matrices[FRAMES_IN_FLIGHT];
	int matrix_count;
	T3DViewport *viewport;
	int current_texture;
	float fov;
	/* each view's rectangle, for the crosshairs drawn after the 3D */
	short rects[MAXIMUM_VIEWS][4];
	/* the FPS counter: frames shown in the last second */
	unsigned long long fps_window_start;
	int fps_frames;
	int fps;
} renderer;

/* the game's last tick, in microseconds (n64_game.c), for the counter */
unsigned long n64_last_tick_us;
/* the CPU's time in the last frame's drawing, from the frame's start to its
end, and the RSP/RDP queue's wait inside n64_render_frame_end */
static unsigned long render_start_ticks, render_cpu_us, render_wait_us;

void n64_render_initialize(void)
{
	display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE_ANTIALIAS);
	rdpq_init();
	t3d_init((T3DInitParams){});
	for (int i = 0; i < MAXIMUM_VIEWS; i++)
	{
		renderer.viewports[i] = t3d_viewport_create();
		/* triangles reaching past four times the view are clipped (the
		RDP's scissor trims the rest): Halo's big floors and walls near the
		eye made the RSP clip a lot with tiny3d's default of two */
		renderer.viewports[i].guardBandScale = GUARD_BAND_SCALE;
	}
	renderer.identity = malloc_uncached(sizeof(T3DMat4FP));
	for (int i = 0; i < FRAMES_IN_FLIGHT; i++)
		renderer.matrices[i] = malloc_uncached(sizeof(T3DMat4FP) * MATRICES_PER_FRAME);
	rdpq_text_register_font(FONT_BUILTIN_DEBUG_MONO, rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));
	{
		FILE *flag;

		if ((flag = fopen("rom:/debug_overview", "rb"))) { debug_overview = 1; fclose(flag); }
		debug_gpu_time = n64_debug_flag("debug_gpu_time");
		if ((flag = fopen("rom:/debug_untextured", "rb"))) { debug_untextured = 1; fclose(flag); }
	}
	{
		T3DMat4 identity;

		t3d_mat4_identity(&identity);
		t3d_mat4_to_fixed(renderer.identity, &identity);
	}
}

/* a model texture: read when a model that uses it is loaded, freed (once
the RSP is done with it) when the last such model is dropped */
static sprite_t *acquire_texture(int texture)
{
	char path[64];

	if (texture < 0 || texture >= models.header->texture_count)
		return NULL;
	if (!models.sprites[texture])
	{
		snprintf(path, sizeof(path), "rom:/tex/%s/%s.sprite", models.map_name, models.textures[texture].name);
		models.sprites[texture] = sprite_load(path);
		if (!models.sprites[texture])
			return NULL;
		surface_t pixels = sprite_get_pixels(models.sprites[texture]);

		models.texture_size[texture] = pixels.stride * pixels.height + 512;
		models.texture_bytes += models.texture_size[texture];
		models.texture_reads++;
	}
	models.texture_users[texture]++;
	return models.sprites[texture];
}

static void release_texture(int texture)
{
	if (texture < 0 || texture >= models.header->texture_count || !models.sprites[texture] ||
		--models.texture_users[texture])
		return;
	rdpq_call_deferred((void (*)(void *))sprite_free, models.sprites[texture]);
	models.sprites[texture] = NULL;
	models.texture_bytes -= models.texture_size[texture];
}

/* a model's data and recordings, freed once the RSP is done with them */
struct model_release
{
	uint8_t *data;
	rspq_block_t **setups, **blocks;
	int count;
};

static void release_model(void *argument)
{
	struct model_release *release = argument;

	for (int i = 0; i < release->count; i++)
	{
		if (release->setups && release->setups[i])
			rspq_block_free(release->setups[i]);
		if (release->blocks && release->blocks[i])
			rspq_block_free(release->blocks[i]);
	}
	free(release->setups);
	free(release->blocks);
	free(release->data);
	free(release);
}

static void drop_model(int index)
{
	struct loaded_model *loaded = &models.loaded[index];
	struct model_record *model = &models.models[index];
	struct model_release *release;

	if (!loaded->data)
		return;
	release = malloc(sizeof(*release));
	release->data = loaded->data;
	release->setups = loaded->setups;
	release->blocks = loaded->blocks;
	release->count = model->far_first + model->far_count;
	rdpq_call_deferred(release_model, release);
	for (int b = 0; b < model->far_first + model->far_count; b++)
		release_texture(models.batches[model->first_batch + b].texture);
	models.loaded_bytes -= model->vertex_bytes + model->index_bytes;
	loaded->data = NULL;
	loaded->setups = NULL;
	loaded->blocks = NULL;
}

/* the render modes of model batches, recorded once each: textured (the
doubling is in the texture), shaded by the baked sky light, one RDP cycle;
glows unlit and cut out; untextured */
enum { MODEL_MODE_TEXTURED, MODEL_MODE_GLOW, MODEL_MODE_UNTEXTURED, NUMBER_OF_MODEL_MODES };
static rspq_block_t *model_modes[NUMBER_OF_MODEL_MODES];

static int model_mode(sprite_t *sprite, int flags)
{
	return !sprite ? MODEL_MODE_UNTEXTURED : (flags & MATERIAL_GLOW) ? MODEL_MODE_GLOW : MODEL_MODE_TEXTURED;
}

static void record_model_modes(void)
{
	for (int mode = 0; mode < NUMBER_OF_MODEL_MODES; mode++)
	{
		if (model_modes[mode])
			continue;
		rspq_block_begin();
		rdpq_mode_begin();
		rdpq_set_mode_standard();
		rdpq_mode_zbuf(true, true);
		rdpq_mode_persp(true);
		rdpq_mode_filter(FILTER_BILINEAR);
		if (mode == MODEL_MODE_GLOW)
		{
			rdpq_mode_combiner(RDPQ_COMBINER_TEX);
			rdpq_mode_alphacompare(128);
		}
		else if (mode == MODEL_MODE_TEXTURED)
			rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
		else
			rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
		rdpq_mode_end();
		model_modes[mode] = rspq_block_end();
	}
}

/* a batch's texture (and the RSP's culling: Halo's two-sided shaders are
drawn without back-face culling), recorded */
static rspq_block_t *record_setup(sprite_t *sprite, int flags)
{
	int two_sided = (flags & MATERIAL_TWO_SIDED) != 0;

	rspq_block_begin();
	if (sprite)
	{
		rdpq_sprite_upload(TILE0, sprite, &(rdpq_texparms_t){
			.s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE,
		});
	}
	t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | (two_sided ? 0 : T3D_FLAG_CULL_BACK) |
		(sprite ? T3D_FLAG_TEXTURED : 0));
	return rspq_block_end();
}

/* a model's data in memory (read from the cartridge, its textures read and
its batches recorded the first time), or 0 */
static int load_model(int index)
{
	struct model_record *model = &models.models[index];
	struct loaded_model *loaded = &models.loaded[index];
	unsigned long size = model->vertex_bytes + model->index_bytes;
	FILE *file;

	loaded->last_drawn = renderer.frame_index;
	if (loaded->data)
		return 1;
	/* room: drop the models drawn longest ago, none drawn in the frames
	still in flight (their textures go with the last model using them) */
	while (models.loaded_bytes + models.texture_bytes + size > MODEL_CACHE_BYTES)
	{
		int oldest = -1;

		for (int i = 0; i < models.header->model_count; i++)
		{
			if (models.loaded[i].data && models.loaded[i].last_drawn + FRAMES_IN_FLIGHT < renderer.frame_index &&
				(oldest < 0 || models.loaded[i].last_drawn < models.loaded[oldest].last_drawn))
				oldest = i;
		}
		if (oldest < 0)
			break;
		drop_model(oldest);
	}
	loaded->data = memalign(16, size);
	loaded->setups = calloc(model->far_first + model->far_count, sizeof(rspq_block_t *));
	loaded->blocks = calloc(model->far_first + model->far_count, sizeof(rspq_block_t *));
	file = loaded->data && loaded->setups && loaded->blocks ? fopen(models.path, "rb") : NULL;
	if (!file || fseek(file, model->data_offset, SEEK_SET) || fread(loaded->data, 1, size, file) != size)
	{
		if (file)
			fclose(file);
		free(loaded->data);
		free(loaded->setups);
		free(loaded->blocks);
		loaded->data = NULL;
		loaded->setups = NULL;
		loaded->blocks = NULL;
		return 0;
	}
	fclose(file);
	data_cache_hit_writeback_invalidate(loaded->data, size);
	models.loaded_bytes += size;
	models.reads++;
	for (int b = 0; b < model->far_first + model->far_count; b++)
	{
		struct model_batch *batch = &models.batches[model->first_batch + b];
		uint8_t *index = loaded->data + model->vertex_bytes + batch->index_offset;
		sprite_t *sprite = acquire_texture(batch->texture);

		record_model_modes();
		loaded->setups[b] = record_setup(debug_untextured ? NULL : sprite, batch->flags);
		rspq_block_begin();
		for (int g = 0; g < batch->segment_count; g++)
		{
			struct model_segment *segment = &models.segments[batch->first_segment + g];

			t3d_matrix_push(t3d_segment_address(SEGMENT_NODE_MATRICES, (void *)(segment->node * sizeof(T3DMat4FP))));
			t3d_vert_load((T3DVertPacked *)loaded->data + segment->vertex_pair_offset, segment->slot, segment->count);
			t3d_matrix_pop(1);
		}
		for (int t = 0; t < batch->triangle_count; t++, index += 3)
			t3d_tri_draw(index[0], index[1], index[2]);
		t3d_tri_sync();
		loaded->blocks[b] = rspq_block_end();
	}
	return 1;
}

static void load_models(const char *map_name)
{
	struct models_header header;
	FILE *file;

	if (models.data)
	{
		rspq_wait();
		for (int i = 0; i < models.header->model_count; i++)
			drop_model(i);
		rspq_wait();
		free(models.sprites);
		free(models.texture_users);
		free(models.texture_size);
		free(models.loaded);
		free(models.by_tag);
		free(models.data);
		memset(&models, 0, sizeof(models));
	}
	snprintf(models.path, sizeof(models.path), "rom:/levels/%s.mdl", map_name);
	snprintf(models.map_name, sizeof(models.map_name), "%s", map_name);
	file = fopen(models.path, "rb");
	if (!file)
	{
		debugf("n64: no models %s\n", models.path);
		return;
	}
	/* the tables stay in memory; vertices_offset is where they end */
	if (fread(&header, 1, sizeof(header), file) != sizeof(header) || memcmp(header.magic, "H64O", 4) || header.version != 3)
	{
		fclose(file);
		assertf(0, "%s was converted for another build", models.path);
		return;
	}
	models.data = memalign(16, header.vertices_offset);
	fseek(file, 0, SEEK_SET);
	fread(models.data, 1, header.vertices_offset, file);
	fclose(file);
	models.header = (struct models_header *)models.data;
	models.textures = (struct level_texture *)(models.data + models.header->textures_offset);
	models.models = (struct model_record *)(models.data + models.header->models_offset);
	models.regions = (struct region_record *)(models.data + models.header->regions_offset);
	models.permutations = (int16_t *)(models.data + models.header->permutations_offset);
	models.geometries = (struct geometry_record *)(models.data + models.header->geometries_offset);
	models.batches = (struct model_batch *)(models.data + models.header->batches_offset);
	models.segments = (struct model_segment *)(models.data + models.header->segments_offset);
	models.sprites = calloc(models.header->texture_count, sizeof(sprite_t *));
	models.texture_users = calloc(models.header->texture_count, sizeof(unsigned short));
	models.texture_size = calloc(models.header->texture_count, sizeof(unsigned long));
	models.loaded = calloc(models.header->model_count, sizeof(struct loaded_model));
	models.tag_count = 0;
	for (int i = 0; i < models.header->model_count; i++)
	{
		if (models.models[i].tag_index + 1 > models.tag_count)
			models.tag_count = models.models[i].tag_index + 1;
	}
	models.by_tag = calloc(models.tag_count, sizeof(struct model_record *));
	for (int i = 0; i < models.header->model_count; i++)
		models.by_tag[models.models[i].tag_index] = &models.models[i];
	debugf("n64: %d models, %ld KB of tables\n", models.header->model_count, (long)header.vertices_offset / 1024);
}

int n64_render_load_level(const char *map_name)
{
	char path[64];
	FILE *file;
	long size;

	if (level.data)
	{
		rspq_wait();
		for (int i = 0; i < level.header->batch_count; i++)
			rspq_block_free(level.blocks[i]);
		free(level.blocks);
		for (int i = 0; i < level.header->texture_count; i++)
		{
			if (level.sprites[i])
				sprite_free(level.sprites[i]);
		}
		free(level.sprites);
		free(level.data);
		memset(&level, 0, sizeof(level));
	}
	snprintf(path, sizeof(path), "rom:/levels/%s.lvl", map_name);
	file = fopen(path, "rb");
	if (!file)
	{
		debugf("n64: no level geometry %s\n", path);
		return 0;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	level.data = memalign(16, size);
	fread(level.data, 1, size, file);
	fclose(file);
	/* the RSP reads the vertices by DMA */
	data_cache_hit_writeback_invalidate(level.data, size);

	level.header = (struct level_header *)level.data;
	assertf(!memcmp(level.header->magic, "H64L", 4) && level.header->version == 2,
		"%s was converted for another build", path);
	level.textures = (struct level_texture *)(level.data + level.header->textures_offset);
	level.materials = (struct level_material *)(level.data + level.header->materials_offset);
	level.clusters = (struct level_cluster *)(level.data + level.header->clusters_offset);
	level.batches = (struct level_batch *)(level.data + level.header->batches_offset);
	level.vertices = (T3DVertPacked *)(level.data + level.header->vertices_offset);
	level.indices = level.data + level.header->indices_offset;
	level.blocks = calloc(level.header->batch_count, sizeof(rspq_block_t *));
	for (int b = 0; b < level.header->batch_count; b++)
	{
		struct level_batch *batch = &level.batches[b];
		uint8_t *index = level.indices + batch->index_offset;

		rspq_block_begin();
		t3d_vert_load(level.vertices + batch->vertex_pair_offset, 0, batch->vertex_count);
		for (int t = 0; t < batch->triangle_count; t++, index += 3)
			t3d_tri_draw(index[0], index[1], index[2]);
		t3d_tri_sync();
		level.blocks[b] = rspq_block_end();
	}
	level.sprites = calloc(level.header->texture_count, sizeof(sprite_t *));
	for (int i = 0; i < level.header->texture_count; i++)
	{
		snprintf(path, sizeof(path), "rom:/tex/%s/%s.sprite", map_name, level.textures[i].name);
		level.sprites[i] = sprite_load(path);
	}
	load_models(map_name);
	debugf("n64: level %s: %d clusters, %d batches, %lu KB, %lu KB of heap left\n", map_name,
		level.header->cluster_count, level.header->batch_count, (unsigned long)size / 1024,
		n64_heap_free() / 1024);
	return 1;
}

/* the tinted palettes made this frame (use_setup) */
static int palette_count;

void n64_render_frame_begin(void)
{
	unsigned long before_display = TICKS_READ();

	renderer.surface = display_get();
	render_wait_us = TICKS_TO_US(TICKS_READ() - before_display);
	render_start_ticks = TICKS_READ();
	rdpq_attach(renderer.surface, display_get_zbuf());
	t3d_frame_start();
	t3d_screen_clear_color(RGBA32(96, 112, 140, 0xFF));
	t3d_screen_clear_depth();
	renderer.view_count = 0;
	renderer.frame_index++;
	renderer.matrix_count = 0;
	palette_count = 0;
}

static void set_material(int index)
{
	struct level_material *material = &level.materials[index];
	int textured = material->texture >= 0 && level.sprites[material->texture] && !debug_untextured;

	rdpq_sync_pipe();
	rdpq_sync_tile();
	rdpq_mode_begin();
	rdpq_set_mode_standard();
	rdpq_mode_zbuf(true, true);
	rdpq_mode_persp(true);
	rdpq_mode_filter(FILTER_BILINEAR);
	rdpq_mode_dithering(DITHER_SQUARE_SQUARE);
	/* Halo's environment shader doubles texture x light; the converter
	doubled the textures, so one combiner cycle does */
	if (textured)
		rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
	else
		rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
	if (material->flags & MATERIAL_TRANSPARENT)
	{
		rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
		rdpq_mode_zbuf(true, false);
	}
	else if (material->flags & MATERIAL_ALPHA_TEST)
	{
		rdpq_mode_alphacompare(128);
	}
	rdpq_mode_end();
	if (textured)
	{
		rdpq_sprite_upload(TILE0, level.sprites[material->texture], &(rdpq_texparms_t){
			.s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE,
		});
	}
	t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH |
		(textured ? T3D_FLAG_TEXTURED : 0) |
		((material->flags & MATERIAL_TWO_SIDED) ? 0 : T3D_FLAG_CULL_BACK));
}

static void draw_level(T3DViewport *viewport)
{
	int current_material = -1;

	if (!level.data || n64_debug_flag("debug_no_level"))
		return;
	t3d_matrix_push(renderer.identity);
	for (int c = 0; c < level.header->cluster_count; c++)
	{
		struct level_cluster *cluster = &level.clusters[c];
		T3DVec3 minimum = {{cluster->minimum[0] * WORLD_SCALE, cluster->minimum[1] * WORLD_SCALE, cluster->minimum[2] * WORLD_SCALE}};
		T3DVec3 maximum = {{cluster->maximum[0] * WORLD_SCALE, cluster->maximum[1] * WORLD_SCALE, cluster->maximum[2] * WORLD_SCALE}};

		if (!cluster->batch_count || !t3d_frustum_vs_aabb(&viewport->viewFrustum, &minimum, &maximum))
			continue;
		renderer.clusters++;
		for (int b = cluster->first_batch; b < cluster->first_batch + cluster->batch_count; b++)
		{
			struct level_batch *batch = &level.batches[b];
			uint8_t *index = level.indices + batch->index_offset;

			if (!t3d_frustum_vs_aabb_s16(&viewport->viewFrustum, batch->minimum, batch->maximum))
				continue;
			if (batch->material != current_material)
			{
				current_material = batch->material;
				set_material(current_material);
			}
			rspq_block_run(level.blocks[b]);
			renderer.triangles += batch->triangle_count;
			renderer.batches++;
		}
	}
	t3d_matrix_pop(1);
}

void n64_render_view(const struct n64_view *view)
{
	T3DViewport *viewport;
	T3DVec3 eye, target, up;
	uint8_t white[4] = {0xFF, 0xFF, 0xFF, 0xFF};

	if (renderer.view_count >= MAXIMUM_VIEWS)
		return;
	renderer.rects[renderer.view_count][0] = view->x0;
	renderer.rects[renderer.view_count][1] = view->y0;
	renderer.rects[renderer.view_count][2] = view->x1;
	renderer.rects[renderer.view_count][3] = view->y1;
	viewport = &renderer.viewports[renderer.view_count++];
	if (!(renderer.frame_index % 30))
	{
		debugf("view %d: at %.2f %.2f %.2f forward %.2f %.2f %.2f up %.2f %.2f %.2f fov %.2f rect %d,%d-%d,%d\n",
			renderer.view_count - 1, view->position[0], view->position[1], view->position[2],
			view->forward[0], view->forward[1], view->forward[2], view->up[0], view->up[1], view->up[2],
			view->vertical_field_of_view, view->x0, view->y0, view->x1, view->y1);
	}
	t3d_viewport_set_area(viewport, view->x0, view->y0, view->x1 - view->x0, view->y1 - view->y0);
	t3d_viewport_set_projection(viewport, view->vertical_field_of_view,
		view->z_near * WORLD_SCALE, view->z_far * WORLD_SCALE);
	renderer.fov = view->vertical_field_of_view;
	renderer.eye[0] = view->position[0];
	renderer.eye[1] = view->position[1];
	renderer.eye[2] = view->position[2];
	renderer.pixels_per_unit = (view->y1 - view->y0) * 0.5f / tanf(view->vertical_field_of_view * 0.5f);
	eye = (T3DVec3){{view->position[0] * WORLD_SCALE, view->position[1] * WORLD_SCALE, view->position[2] * WORLD_SCALE}};
	target = (T3DVec3){{eye.v[0] + view->forward[0] * WORLD_SCALE, eye.v[1] + view->forward[1] * WORLD_SCALE,
		eye.v[2] + view->forward[2] * WORLD_SCALE}};
	up = (T3DVec3){{view->up[0], view->up[1], view->up[2]}};
	if (debug_overview && renderer.view_count == 1)
	{
		/* the first view looks down on the whole level from above */
		eye = (T3DVec3){{0, -12 * WORLD_SCALE, 30 * WORLD_SCALE}};
		target = (T3DVec3){{0, 0, 0}};
		up = (T3DVec3){{0, 0, 1}};
	}
	t3d_viewport_look_at(viewport, &eye, &target, &up);
	t3d_viewport_attach(viewport);

	/* the vertex colours are the lighting: no lights, a white ambient */
	rdpq_set_env_color(RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
	t3d_light_set_ambient(white);
	t3d_light_set_count(0);
	render_phase(PHASE_LEVEL);
	draw_level(viewport);
	render_phase(PHASE_OBJECTS);
	renderer.first_person = 0;
	renderer.viewport = viewport;
	renderer.current_texture = -2;
}

/* models: textured, shaded by their baked sky light, one RDP cycle. Halo's
two-sided shaders (tools/n64/models.py) are drawn without back-face
culling. A player's colour is the upper half of a texture's palette times
that colour (Halo tints where the multipurpose map says): a palette of its
own, made for the draw, in a ring that lasts the frames in flight. */
#define PALETTES_PER_FRAME 256

static unsigned long current_color;
static uint16_t palettes[FRAMES_IN_FLIGHT][PALETTES_PER_FRAME][16] __attribute__((aligned(16)));

static uint16_t *tinted_palette(sprite_t *sprite, unsigned long color)
{
	uint16_t *source = sprite_get_palette(sprite), *palette;
	unsigned r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;

	if (!source || palette_count >= PALETTES_PER_FRAME)
		return NULL;
	palette = palettes[renderer.frame_index % FRAMES_IN_FLIGHT][palette_count++];
	for (int i = 0; i < 16; i++)
	{
		uint16_t c = source[i];

		if (i >= 8)
		{
			unsigned cr = ((c >> 11) & 31) * r / 255, cg = ((c >> 6) & 31) * g / 255, cb = ((c >> 1) & 31) * b / 255;

			c = (uint16_t)((cr << 11) | (cg << 6) | (cb << 1) | (c & 1));
		}
		palette[i] = c;
	}
	data_cache_hit_writeback(palette, sizeof(uint16_t) * 16);
	return palette;
}

/* a batch's render mode and texture, unless they are the ones in force; a
player's colour on a MATERIAL_CHANGE_COLOR texture (color 0xRRGGBB; white
leaves it as it is) */
static int current_model_mode = -1;

static void use_setup(struct loaded_model *loaded, int b, struct model_batch *batch, unsigned long color)
{
	int key = (batch->texture << 8) | batch->flags;
	sprite_t *sprite = debug_untextured || batch->texture < 0 ? NULL : models.sprites[batch->texture];
	int mode = model_mode(batch->texture >= 0 ? sprite : NULL, batch->flags);

	if (!(batch->flags & MATERIAL_CHANGE_COLOR))
		color = 0xFFFFFF;
	if (renderer.current_texture == -2)
		current_model_mode = -1;
	if (mode != current_model_mode)
	{
		rspq_block_run(model_modes[mode]);
		current_model_mode = mode;
	}
	if (key != renderer.current_texture || (color == 0xFFFFFF && current_color != 0xFFFFFF))
	{
		rspq_block_run(loaded->setups[b]);
		renderer.current_texture = key;
		current_color = 0xFFFFFF;
	}
	if (color != current_color && sprite)
	{
		uint16_t *palette = tinted_palette(sprite, color);

		if (palette)
			rdpq_tex_upload_tlut(palette, 0, 16);
		current_color = color;
	}
}

void n64_render_first_person_begin(void)
{
	T3DViewport *viewport = renderer.viewport;

	if (!viewport)
		return;
	/* the game's first-person near and far planes (rasterizer.c), and the
	weapon in front of whatever the world put in this view's depth */
	t3d_viewport_set_projection(viewport, renderer.fov, 0.01171875f * WORLD_SCALE, 16.0f * WORLD_SCALE);
	t3d_viewport_attach(viewport);
	t3d_screen_clear_depth();
	renderer.current_texture = -2;
	renderer.first_person = 1;
	render_phase(PHASE_FIRST_PERSON);
}

static void render_object(const struct n64_object *object);

/* the node matrices converted this frame, by object */
#define CONVERTED_SLOTS 256
static struct converted_matrices
{
	unsigned long frame, key;
	T3DMat4FP *matrices;
} converted[CONVERTED_SLOTS];

void n64_render_object(const struct n64_object *object)
{
	render_object(object);
}

static void render_phase(int phase)
{
	unsigned long now;

	gpu_time_sync();
	now = TICKS_READ();
	switch (renderer.phase)
	{
	case PHASE_LEVEL: renderer.level_ticks += now - renderer.phase_start; break;
	case PHASE_OBJECTS: renderer.object_ticks += now - renderer.phase_start; break;
	case PHASE_FIRST_PERSON: renderer.first_person_ticks += now - renderer.phase_start; break;
	case PHASE_HUD: renderer.hud_ticks += now - renderer.phase_start; break;
	}
	renderer.phase = phase;
	renderer.phase_start = now;
}

void n64_render_hud_phase(void)
{
	render_phase(PHASE_HUD);
}

static void render_object(const struct n64_object *object)
{
	struct model_record *model;
	T3DMat4FP *matrices;
	T3DVec3 center;
	int node_count;
	int far;

	if (n64_debug_flag("debug_no_objects"))
		return;
	if (!models.data || !renderer.viewport || object->model_tag_index < 0 ||
		object->model_tag_index >= models.tag_count || !(model = models.by_tag[object->model_tag_index]))
		return;
	center = (T3DVec3){{object->center[0] * WORLD_SCALE, object->center[1] * WORLD_SCALE, object->center[2] * WORLD_SCALE}};
	if (!t3d_frustum_vs_sphere(&renderer.viewport->viewFrustum, &center, object->radius * WORLD_SCALE + 8.0f))
		return;
	far = 0;
	if (!renderer.first_person)
	{
		/* smaller than a pixel and a half across: not drawn; smaller than
		FAR_PIXELS: its far version, if it has one */
		float dx = object->center[0] - renderer.eye[0], dy = object->center[1] - renderer.eye[1];
		float dz = object->center[2] - renderer.eye[2];
		float distance = sqrtf(dx * dx + dy * dy + dz * dz);
		float pixels = distance > object->radius ? object->radius * 2.0f * renderer.pixels_per_unit / distance : 1e9f;

		if (pixels < 1.5f)
			return;
		far = model->far_count && pixels < FAR_PIXELS;
	}
	node_count = object->node_count < model->node_count ? object->node_count : model->node_count;
	if (node_count <= 0 || renderer.matrix_count + model->node_count > MATRICES_PER_FRAME)
		return;
	if (!load_model(model - models.models))
		return;
	/* an object seen in an earlier view of this frame has its matrices */
	matrices = NULL;
	if (object->key)
	{
		for (int probe = 0; probe < 8; probe++)
		{
			struct converted_matrices *entry = &converted[(object->key * 2654435761u + probe) % CONVERTED_SLOTS];

			if (entry->frame == renderer.frame_index && entry->key == object->key)
			{
				matrices = entry->matrices;
				break;
			}
		}
	}
	if (matrices)
		goto converted_already;
	matrices = renderer.matrices[renderer.frame_index % FRAMES_IN_FLIGHT] + renderer.matrix_count;
	renderer.matrix_count += model->node_count;
	if (object->key)
	{
		for (int probe = 0; probe < 8; probe++)
		{
			struct converted_matrices *entry = &converted[(object->key * 2654435761u + probe) % CONVERTED_SLOTS];

			if (entry->frame != renderer.frame_index)
			{
				entry->frame = renderer.frame_index;
				entry->key = object->key;
				entry->matrices = matrices;
				break;
			}
		}
	}
	for (int n = 0; n < node_count; n++)
	{
		const float *m = object->node_matrices + n * 13;
		float axes = m[0] * (WORLD_SCALE / MODEL_SCALE);
		T3DMat4 matrix = {{
			{m[1] * axes, m[2] * axes, m[3] * axes, 0.0f},
			{m[4] * axes, m[5] * axes, m[6] * axes, 0.0f},
			{m[7] * axes, m[8] * axes, m[9] * axes, 0.0f},
			{m[10] * WORLD_SCALE, m[11] * WORLD_SCALE, m[12] * WORLD_SCALE, 1.0f},
		}};

		t3d_mat4_to_fixed(&matrices[n], &matrix);
	}
	/* nodes the game does not have (a model with more nodes than its
	animation drives) keep the first node's matrix */
	for (int n = node_count; n < model->node_count; n++)
		matrices[n] = matrices[0];
converted_already:
	t3d_segment_set(SEGMENT_NODE_MATRICES, matrices);
	renderer.objects++;
	unsigned long color = ((unsigned long)object->tint[0] << 16) | ((unsigned long)object->tint[1] << 8) | object->tint[2];

	if (far)
	{
		struct loaded_model *loaded = &models.loaded[model - models.models];

		for (int b = model->far_first; b < model->far_first + model->far_count; b++)
		{
			struct model_batch *batch = &models.batches[model->first_batch + b];

			use_setup(loaded, b, batch, color);
			rspq_block_run(loaded->blocks[b]);
			renderer.triangles += batch->triangle_count;
		}
		return;
	}
	for (int r = 0; r < model->region_count; r++)
	{
		struct region_record *region = &models.regions[model->first_region + r];
		int permutation = r < object->region_count ? object->region_permutations[r] : 0;
		int geometry_index;
		struct geometry_record *geometry;

		if (!region->permutation_count)
			continue;
		if (permutation >= region->permutation_count)
			permutation = 0;
		geometry_index = models.permutations[region->first_permutation + permutation];
		if (geometry_index < 0)
			continue;
		geometry = &models.geometries[geometry_index];
		struct loaded_model *loaded = &models.loaded[model - models.models];

		for (int b = geometry->first_batch; b < geometry->first_batch + geometry->batch_count; b++)
		{
			struct model_batch *batch = &models.batches[b];

			use_setup(loaded, b - model->first_batch, batch, color);
			rspq_block_run(loaded->blocks[b - model->first_batch]);
			renderer.triangles += batch->triangle_count;
		}
	}
}

/* without the game's HUD (debug_no_hud), a crosshair in the middle of
each view; always the line between the views */
static void draw_crosshairs(void)
{
	rdpq_sync_pipe();
	rdpq_set_scissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	rdpq_set_mode_fill(RGBA32(0x60, 0xD0, 0xFF, 0xFF));
	for (int i = 0; i < renderer.view_count && n64_debug_flag("debug_no_hud"); i++)
	{
		int x = (renderer.rects[i][0] + renderer.rects[i][2]) / 2;
		int y = (renderer.rects[i][1] + renderer.rects[i][3]) / 2;

		rdpq_fill_rectangle(x - 6, y, x - 2, y + 1);
		rdpq_fill_rectangle(x + 2, y, x + 6, y + 1);
		rdpq_fill_rectangle(x, y - 5, x + 1, y - 1);
		rdpq_fill_rectangle(x, y + 2, x + 1, y + 6);
	}
	if (renderer.view_count == 2)
	{
		/* the line between the two views */
		rdpq_set_mode_fill(RGBA32(0, 0, 0, 0xFF));
		rdpq_fill_rectangle(0, renderer.rects[1][1] - 1, SCREEN_WIDTH, renderer.rects[1][1] + 1);
	}
}

static void draw_counter(void)
{
	unsigned long long now = get_ticks();

	if (!renderer.fps_window_start)
		renderer.fps_window_start = now;
	renderer.fps_frames++;
	if (now - renderer.fps_window_start >= TICKS_PER_SECOND)
	{
		renderer.fps = (int)((unsigned long long)renderer.fps_frames * TICKS_PER_SECOND * 10 / (now - renderer.fps_window_start));
		renderer.fps_frames = 0;
		renderer.fps_window_start = now;
	}
	rdpq_sync_pipe();
	rdpq_set_scissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	rdpq_text_printf(NULL, FONT_BUILTIN_DEBUG_MONO, 8, 12, "%d.%d FPS tick %lu.%lu draw %lu.%lu wait %lu.%lu",
		renderer.fps / 10, renderer.fps % 10, n64_last_tick_us / 1000, (n64_last_tick_us % 1000) / 100,
		render_cpu_us / 1000, (render_cpu_us % 1000) / 100, render_wait_us / 1000, (render_wait_us % 1000) / 100);
}

void n64_profile_poll(void);

void n64_render_frame_end(void)
{
	render_phase(PHASE_NONE);
	n64_profile_poll();
	draw_crosshairs();
	draw_counter();
	rdpq_detach_show();
	render_cpu_us = TICKS_TO_US(TICKS_READ() - render_start_ticks);
	renderer.draw_ticks += TICKS_READ() - render_start_ticks;
	if (!(renderer.frame_index % 120))
	{
		unsigned long n64_stack_used(void);
		heap_stats_t heap;
		unsigned long long now = get_ticks();

		if (renderer.window_start)
		{
			debugf("frames: %lu.%lu fps; per frame: draw %llu us (level %llu, objects %llu, first person %llu, hud %llu), tick %lu us\n",
				(unsigned long)(120ULL * TICKS_PER_SECOND * 10 / (now - renderer.window_start) / 10),
				(unsigned long)(120ULL * TICKS_PER_SECOND * 10 / (now - renderer.window_start) % 10),
				TICKS_TO_US(renderer.draw_ticks) / 120, TICKS_TO_US(renderer.level_ticks) / 120,
				TICKS_TO_US(renderer.object_ticks) / 120, TICKS_TO_US(renderer.first_person_ticks) / 120,
				TICKS_TO_US(renderer.hud_ticks) / 120, n64_last_tick_us);
		}
		renderer.window_start = now;
		renderer.draw_ticks = renderer.level_ticks = renderer.object_ticks = renderer.first_person_ticks = 0;
		renderer.hud_ticks = 0;
		sys_get_heap_stats(&heap);
		debugf("render: frame %lu, per frame %lu clusters, %lu batches, %lu objects, %lu triangles; heap %d KB free, stack %lu KB used; "
			"models %lu KB (%lu reads), textures %lu KB (%lu reads)\n",
			renderer.frame_index, renderer.clusters / 120, renderer.batches / 120, renderer.objects / 120,
			renderer.triangles / 120, heap.free / 1024, n64_stack_used() / 1024,
			models.loaded_bytes / 1024, models.reads, models.texture_bytes / 1024, models.texture_reads);
		renderer.clusters = renderer.batches = renderer.triangles = renderer.objects = 0;
	}
}
