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
} level;

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
	unsigned long triangles, batches, clusters;
} renderer;

void n64_render_initialize(void)
{
	display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE_ANTIALIAS);
	rdpq_init();
	t3d_init((T3DInitParams){});
	for (int i = 0; i < MAXIMUM_VIEWS; i++)
		renderer.viewports[i] = t3d_viewport_create();
	renderer.identity = malloc_uncached(sizeof(T3DMat4FP));
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

int n64_render_load_level(const char *map_name)
{
	char path[64];
	FILE *file;
	long size;

	if (level.data)
	{
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
	assertf(!memcmp(level.header->magic, "H64L", 4) && level.header->version == 1,
		"%s was converted for another build", path);
	level.textures = (struct level_texture *)(level.data + level.header->textures_offset);
	level.materials = (struct level_material *)(level.data + level.header->materials_offset);
	level.clusters = (struct level_cluster *)(level.data + level.header->clusters_offset);
	level.batches = (struct level_batch *)(level.data + level.header->batches_offset);
	level.vertices = (T3DVertPacked *)(level.data + level.header->vertices_offset);
	level.indices = level.data + level.header->indices_offset;
	level.sprites = calloc(level.header->texture_count, sizeof(sprite_t *));
	for (int i = 0; i < level.header->texture_count; i++)
	{
		snprintf(path, sizeof(path), "rom:/tex/%s/%s.sprite", map_name, level.textures[i].name);
		level.sprites[i] = sprite_load(path);
	}
	debugf("n64: level %s: %d clusters, %d batches, %lu KB, %lu KB of heap left\n", map_name,
		level.header->cluster_count, level.header->batch_count, (unsigned long)size / 1024,
		n64_heap_free() / 1024);
	return 1;
}

void n64_render_frame_begin(void)
{
	renderer.surface = display_get();
	rdpq_attach(renderer.surface, display_get_zbuf());
	t3d_frame_start();
	t3d_screen_clear_color(RGBA32(96, 112, 140, 0xFF));
	t3d_screen_clear_depth();
	renderer.view_count = 0;
	renderer.frame_index++;
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
	/* Halo's environment shader doubles texture x light (its lightmaps and
	detail maps are stored at half brightness); a second combiner cycle
	does the same: (texture x shade) + (texture x shade) x env (white) */
	if (textured)
		rdpq_mode_combiner(RDPQ_COMBINER2((TEX0, 0, SHADE, 0), (TEX0, 0, SHADE, 0),
			(COMBINED, 0, ENV, COMBINED), (0, 0, 0, COMBINED)));
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

	if (!level.data)
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

			if (batch->material != current_material)
			{
				current_material = batch->material;
				set_material(current_material);
			}
			t3d_vert_load(level.vertices + batch->vertex_pair_offset, 0, batch->vertex_count);
			for (int t = 0; t < batch->triangle_count; t++, index += 3)
				t3d_tri_draw(index[0], index[1], index[2]);
			t3d_tri_sync();
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
}

void n64_render_frame_end(void)
{
	rdpq_detach_show();
	if (!(renderer.frame_index % 120))
	{
		debugf("render: frame %lu, per frame %lu clusters, %lu batches, %lu triangles\n", renderer.frame_index,
			renderer.clusters / 120, renderer.batches / 120, renderer.triangles / 120);
		renderer.clusters = renderer.batches = renderer.triangles = 0;
	}
}
