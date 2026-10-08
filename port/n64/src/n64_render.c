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

enum
{
	MATERIAL_ALPHA_TEST = 1,
	MATERIAL_TWO_SIDED = 2,
	MATERIAL_TRANSPARENT = 4,
	MATERIAL_UNTEXTURED = 8,
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

struct model_record { uint16_t tag_index; uint8_t node_count, region_count; uint16_t first_region, pad; };
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

static struct
{
	uint8_t *data;
	struct models_header *header;
	struct level_texture *textures;
	struct model_record *models;
	struct region_record *regions;
	int16_t *permutations;
	struct geometry_record *geometries;
	struct model_batch *batches;
	struct model_segment *segments;
	T3DVertPacked *vertices;
	uint8_t *indices;
	sprite_t **sprites;
	/* tag index -> model record, or NULL */
	struct model_record **by_tag;
	int tag_count;
	/* each batch's loads (node matrices from the segment the object sets)
	and triangles, recorded once */
	rspq_block_t **blocks;
} models;

#define SEGMENT_NODE_MATRICES T3D_SEGMENT_1

/* debug switches: files on the cartridge (port.json "flags") */
static int debug_overview;
static int debug_untextured;

static struct
{
	T3DViewport viewports[MAXIMUM_VIEWS];
	T3DMat4FP *identity;
	int view_count;
	surface_t *surface;
	unsigned long frame_index;
	unsigned long triangles, batches, clusters, objects;
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
		renderer.viewports[i] = t3d_viewport_create();
	renderer.identity = malloc_uncached(sizeof(T3DMat4FP));
	for (int i = 0; i < FRAMES_IN_FLIGHT; i++)
		renderer.matrices[i] = malloc_uncached(sizeof(T3DMat4FP) * MATRICES_PER_FRAME);
	rdpq_text_register_font(FONT_BUILTIN_DEBUG_MONO, rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));
	{
		FILE *flag;

		if ((flag = fopen("rom:/debug_overview", "rb"))) { debug_overview = 1; fclose(flag); }
		if ((flag = fopen("rom:/debug_untextured", "rb"))) { debug_untextured = 1; fclose(flag); }
	}
	{
		T3DMat4 identity;

		t3d_mat4_identity(&identity);
		t3d_mat4_to_fixed(renderer.identity, &identity);
	}
}

static void load_models(const char *map_name)
{
	char path[64];
	FILE *file;
	long size;

	if (models.data)
	{
		rspq_wait();
		for (int i = 0; i < models.header->batch_count; i++)
			rspq_block_free(models.blocks[i]);
		free(models.blocks);
		for (int i = 0; i < models.header->texture_count; i++)
		{
			if (models.sprites[i])
				sprite_free(models.sprites[i]);
		}
		free(models.sprites);
		free(models.by_tag);
		free(models.data);
		memset(&models, 0, sizeof(models));
	}
	snprintf(path, sizeof(path), "rom:/levels/%s.mdl", map_name);
	file = fopen(path, "rb");
	if (!file)
	{
		debugf("n64: no models %s\n", path);
		return;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	models.data = memalign(16, size);
	fread(models.data, 1, size, file);
	fclose(file);
	data_cache_hit_writeback_invalidate(models.data, size);
	models.header = (struct models_header *)models.data;
	assertf(!memcmp(models.header->magic, "H64O", 4) && models.header->version == 1,
		"%s was converted for another build", path);
	models.textures = (struct level_texture *)(models.data + models.header->textures_offset);
	models.models = (struct model_record *)(models.data + models.header->models_offset);
	models.regions = (struct region_record *)(models.data + models.header->regions_offset);
	models.permutations = (int16_t *)(models.data + models.header->permutations_offset);
	models.geometries = (struct geometry_record *)(models.data + models.header->geometries_offset);
	models.batches = (struct model_batch *)(models.data + models.header->batches_offset);
	models.segments = (struct model_segment *)(models.data + models.header->segments_offset);
	models.vertices = (T3DVertPacked *)(models.data + models.header->vertices_offset);
	models.indices = models.data + models.header->indices_offset;
	models.sprites = calloc(models.header->texture_count, sizeof(sprite_t *));
	for (int i = 0; i < models.header->texture_count; i++)
	{
		snprintf(path, sizeof(path), "rom:/tex/%s/%s.sprite", map_name, models.textures[i].name);
		models.sprites[i] = sprite_load(path);
	}
	models.tag_count = 0;
	for (int i = 0; i < models.header->model_count; i++)
	{
		if (models.models[i].tag_index + 1 > models.tag_count)
			models.tag_count = models.models[i].tag_index + 1;
	}
	models.by_tag = calloc(models.tag_count, sizeof(struct model_record *));
	for (int i = 0; i < models.header->model_count; i++)
		models.by_tag[models.models[i].tag_index] = &models.models[i];
	models.blocks = calloc(models.header->batch_count, sizeof(rspq_block_t *));
	for (int b = 0; b < models.header->batch_count; b++)
	{
		struct model_batch *batch = &models.batches[b];
		uint8_t *index = models.indices + batch->index_offset;

		rspq_block_begin();
		for (int g = 0; g < batch->segment_count; g++)
		{
			struct model_segment *segment = &models.segments[batch->first_segment + g];

			t3d_matrix_push(t3d_segment_address(SEGMENT_NODE_MATRICES, (void *)(segment->node * sizeof(T3DMat4FP))));
			t3d_vert_load(models.vertices + segment->vertex_pair_offset, segment->slot, segment->count);
			t3d_matrix_pop(1);
		}
		for (int t = 0; t < batch->triangle_count; t++, index += 3)
			t3d_tri_draw(index[0], index[1], index[2]);
		t3d_tri_sync();
		models.blocks[b] = rspq_block_end();
	}
	debugf("n64: %d models, %ld KB\n", models.header->model_count, size / 1024);
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
	draw_level(viewport);
	renderer.viewport = viewport;
	renderer.current_texture = -2;
}

/* models: textured (the doubling is in the texture), shaded by their baked
sky light; one RDP cycle, or two with a player's tint. Halo's two-sided
shaders (tools/n64/models.py) are drawn without back-face culling. */
static int current_tinted, current_two_sided;

static void set_model_texture(int texture, int flags, int tinted)
{
	int textured = texture >= 0 && models.sprites[texture] && !debug_untextured;
	int two_sided = (flags & MATERIAL_TWO_SIDED) != 0;

	if (texture == renderer.current_texture && tinted == current_tinted && two_sided == current_two_sided)
		return;
	renderer.current_texture = texture;
	current_tinted = tinted;
	current_two_sided = two_sided;
	rdpq_sync_pipe();
	rdpq_sync_tile();
	rdpq_mode_begin();
	rdpq_set_mode_standard();
	rdpq_mode_zbuf(true, true);
	rdpq_mode_persp(true);
	rdpq_mode_filter(FILTER_BILINEAR);
	if (textured && tinted)
		rdpq_mode_combiner(RDPQ_COMBINER2((TEX0, 0, SHADE, 0), (TEX0, 0, SHADE, 0),
			(COMBINED, 0, PRIM, 0), (0, 0, 0, COMBINED)));
	else if (textured)
		rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
	else
		rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
	rdpq_mode_end();
	if (textured)
	{
		rdpq_sprite_upload(TILE0, models.sprites[texture], &(rdpq_texparms_t){
			.s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE,
		});
	}
	t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | (two_sided ? 0 : T3D_FLAG_CULL_BACK) |
		(textured ? T3D_FLAG_TEXTURED : 0));
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
}

void n64_render_object(const struct n64_object *object)
{
	struct model_record *model;
	T3DMat4FP *matrices;
	T3DVec3 center;
	int node_count;

	if (n64_debug_flag("debug_no_objects"))
		return;
	if (!models.data || !renderer.viewport || object->model_tag_index < 0 ||
		object->model_tag_index >= models.tag_count || !(model = models.by_tag[object->model_tag_index]))
		return;
	center = (T3DVec3){{object->center[0] * WORLD_SCALE, object->center[1] * WORLD_SCALE, object->center[2] * WORLD_SCALE}};
	if (!t3d_frustum_vs_sphere(&renderer.viewport->viewFrustum, &center, object->radius * WORLD_SCALE + 8.0f))
		return;
	node_count = object->node_count < model->node_count ? object->node_count : model->node_count;
	if (node_count <= 0 || renderer.matrix_count + model->node_count > MATRICES_PER_FRAME)
		return;
	matrices = renderer.matrices[renderer.frame_index % FRAMES_IN_FLIGHT] + renderer.matrix_count;
	renderer.matrix_count += model->node_count;
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
	t3d_segment_set(SEGMENT_NODE_MATRICES, matrices);
	renderer.objects++;
	int tinted = object->tint[0] != 0xFF || object->tint[1] != 0xFF || object->tint[2] != 0xFF;

	if (tinted)
		rdpq_set_prim_color(RGBA32(object->tint[0], object->tint[1], object->tint[2], 0xFF));
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
		for (int b = geometry->first_batch; b < geometry->first_batch + geometry->batch_count; b++)
		{
			struct model_batch *batch = &models.batches[b];
			uint8_t *index = models.indices + batch->index_offset;

			set_model_texture(batch->texture, batch->flags, tinted);
			rspq_block_run(models.blocks[b]);
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
	n64_profile_poll();
	draw_crosshairs();
	draw_counter();
	rdpq_detach_show();
	render_cpu_us = TICKS_TO_US(TICKS_READ() - render_start_ticks);
	if (!(renderer.frame_index % 120))
	{
		unsigned long n64_stack_used(void);
		heap_stats_t heap;

		sys_get_heap_stats(&heap);
		debugf("render: frame %lu, per frame %lu clusters, %lu batches, %lu objects, %lu triangles; heap %d KB free, stack %lu KB used\n",
			renderer.frame_index, renderer.clusters / 120, renderer.batches / 120, renderer.objects / 120,
			renderer.triangles / 120, heap.free / 1024, n64_stack_used() / 1024);
		renderer.clusters = renderer.batches = renderer.triangles = renderer.objects = 0;
	}
}
