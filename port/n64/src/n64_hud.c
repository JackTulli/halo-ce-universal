/*
N64_HUD.C

The HUD's drawing (n64.h): the game's own HUD code (interface/hud*.c,
motion_sensor.c, game_engine.c) lays out its bitmaps, meters, numbers and
text; port/n64/game/n64_hud_game.c hands each quad here. The bitmaps are
half-size sprites on the cartridge (tools/n64/hud.py), loaded on first use.

An upright quad is a blit (rdpq handles bitmaps larger than TMEM by parts);
a rotated one is textured triangles. A meter's texture is made on the CPU,
only the part of the sprite the quad shows, and kept until its value or
colours change.

Halo's HUD code costs the N64 several milliseconds a view, so each view's
HUD is recorded (an rspq block) and the game draws it every other frame,
the views taking turns; in between the recording is shown again. Whatever
a recording uses stays in memory: the sprites until the next map, the
meters' textures in fixed slots.
*/

#include <libdragon.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "n64.h"

#define MAXIMUM_SPRITES 256
#define MAXIMUM_VIEWS 4
#define HUD_INTERVAL 2
#define METER_CACHE 12
/* a meter texture's slot: 128x32 RGBA16 (the parts of meters shown are
smaller) */
#define METER_SLOT_WIDTH 128
#define METER_SLOT_HEIGHT 32
#define BLIP_SIZE 16

#define MIN(a, b) ((a) < (b) ? (a) : (b))

enum
{
	_mode_none,
	_mode_over,      /* colour * texture over the framebuffer */
	_mode_meter,     /* a meter's texture (opaque or cut out) */
	_mode_meter_glow, /* a meter's texture, partly over (it keeps most of the framebuffer) */
	_mode_blip,      /* motion sensor blips */
};

static struct
{
	char map_name[32];
	/* rom:/hud/<map>.list: 16 characters a sprite */
	char *names;
	int name_count;
	sprite_t *sprites[MAXIMUM_SPRITES];
	int missing[MAXIMUM_SPRITES];
	int x0, y0, x1, y1;
	int view_count;
	int mode;
	surface_t blip;
	rspq_block_t *recordings[MAXIMUM_VIEWS];
	unsigned long view_frames[MAXIMUM_VIEWS];
	int view;      /* the view begun */
	int recording; /* the view being recorded, or -1 */
	/* statistics: microseconds in the HUD, its quads and meters, every 240
	views; one view's quads are logged at view 600 */
	unsigned long begin_ticks, ticks, quad_ticks, meter_ticks, quads, views;
	int dump;
	/* what the last sprite quad left in TMEM (tile 0): its pixels and
	rectangle; NULL when anything else has been loaded since */
	const void *tmem_pixels;
	int tmem_s0, tmem_t0, tmem_s1, tmem_t1;
	/* statistics: quads drawn by the fast path, uploads it skipped */
	unsigned long fast_quads, skipped_uploads;
} hud;

static struct meter_entry
{
	int sprite;
	int s0, t0, width, height;
	unsigned long minimum_color, maximum_color, flash_color, background_color, tint_color;
	surface_t slot, surface;
	unsigned long last_used;
} meters[METER_CACHE];
static unsigned long meter_clock;

/* a soft round dot (Halo's blip bitmap is one), intensity 8 bits */
static void make_blip(void)
{
	hud.blip = surface_alloc(FMT_I8, BLIP_SIZE, BLIP_SIZE);
	for (int y = 0; y < BLIP_SIZE; y++)
	{
		uint8_t *row = (uint8_t *)hud.blip.buffer + y * hud.blip.stride;

		for (int x = 0; x < BLIP_SIZE; x++)
		{
			float dx = (x + 0.5f) / (BLIP_SIZE / 2) - 1.f, dy = (y + 0.5f) / (BLIP_SIZE / 2) - 1.f;
			float d = 1.f - sqrtf(dx * dx + dy * dy);

			row[x] = d <= 0.f ? 0 : d >= 0.5f ? 255 : (uint8_t)(d * 510.f);
		}
	}
	data_cache_hit_writeback(hud.blip.buffer, hud.blip.stride * BLIP_SIZE);
}

void n64_hud_set_map(const char *map_name)
{
	for (int i = 0; i < MAXIMUM_SPRITES; i++)
	{
		if (hud.sprites[i])
			sprite_free(hud.sprites[i]);
		hud.sprites[i] = NULL;
		hud.missing[i] = 0;
	}
	for (int i = 0; i < MAXIMUM_VIEWS; i++)
	{
		if (hud.recordings[i])
			rdpq_call_deferred((void (*)(void *))rspq_block_free, hud.recordings[i]);
		hud.recordings[i] = NULL;
	}
	for (int i = 0; i < METER_CACHE; i++)
	{
		surface_t slot = meters[i].slot;

		memset(&meters[i], 0, sizeof(meters[i]));
		meters[i].sprite = -1;
		meters[i].slot = slot.buffer ? slot : surface_alloc(FMT_RGBA16, METER_SLOT_WIDTH, METER_SLOT_HEIGHT);
	}
	if (!hud.blip.buffer)
		make_blip();
	snprintf(hud.map_name, sizeof(hud.map_name), "%s", map_name);
	/* the sprites' names, in number order (tools/n64/hud.py) */
	free(hud.names);
	hud.names = NULL;
	hud.name_count = 0;
	{
		char path[64], line[32];
		FILE *file;

		snprintf(path, sizeof(path), "rom:/hud/%s.list", map_name);
		file = fopen(path, "r");
		if (file)
		{
			hud.names = malloc(MAXIMUM_SPRITES * 16);
			while (hud.names && hud.name_count < MAXIMUM_SPRITES && fgets(line, sizeof(line), file))
			{
				if (strlen(line) >= 16)
					memcpy(hud.names + 16 * hud.name_count++, line, 16);
			}
			fclose(file);
		}
	}
}

static sprite_t *hud_sprite(int number)
{
	char path[64];

	if (number < 0 || number >= MAXIMUM_SPRITES || hud.missing[number])
		return NULL;
	if (!hud.sprites[number])
	{
		FILE *file;

		if (!hud.names || number >= hud.name_count)
		{
			hud.missing[number] = 1;
			return NULL;
		}
		snprintf(path, sizeof(path), "rom:/hud/%.16s.sprite", hud.names + number * 16);
		file = fopen(path, "rb");
		if (!file)
		{
			hud.missing[number] = 1;
			return NULL;
		}
		fclose(file);
		hud.sprites[number] = sprite_load(path);
		if (hud.sprites[number] && n64_debug_flag("debug_hud_sprites"))
			debugf("hud sprite %d: %dx%d format %d\n", number, hud.sprites[number]->width,
				hud.sprites[number]->height, (int)sprite_get_format(hud.sprites[number]));
	}
	return hud.sprites[number];
}

void n64_render_hud_phase(void);

void n64_hud_set_view_count(int count)
{
	hud.view_count = count;
}

int n64_hud_begin(int view, int x0, int y0, int x1, int y1)
{
	n64_render_hud_phase();
	hud.x0 = x0;
	hud.y0 = y0;
	hud.x1 = x1;
	hud.y1 = y1;
	hud.mode = _mode_none;
	hud.tmem_pixels = NULL;
	hud.begin_ticks = TICKS_READ();
	hud.recording = -1;
	hud.view = view;
	if (view < 0 || view >= MAXIMUM_VIEWS)
		return 0;
	/* with three or four views, each view's every fourth frame: one view's
	HUD code a frame */
	if (hud.recordings[view] && (hud.view_frames[view]++ + view) % (hud.view_count > 2 ? 4 : HUD_INTERVAL))
		return 0;
	hud.recording = view;
	hud.dump = hud.views == 600 || hud.views == 601;
	n64_profile_zone = 1;
	rspq_block_begin();
	rdpq_sync_pipe();
	rdpq_set_scissor(x0, y0, x1, y1);
	return 1;
}

void n64_hud_end(void)
{
	n64_profile_zone = 0;
	if (hud.recording >= 0)
	{
		rspq_block_t *recording = rspq_block_end();

		/* the last recording may still be in the RSP's queue */
		if (hud.recordings[hud.recording])
			rdpq_call_deferred((void (*)(void *))rspq_block_free, hud.recordings[hud.recording]);
		hud.recordings[hud.recording] = recording;
		rspq_block_run(recording);
		hud.recording = -1;
	}
	else if (hud.view >= 0 && hud.view < MAXIMUM_VIEWS && hud.recordings[hud.view])
		rspq_block_run(hud.recordings[hud.view]);
	hud.ticks += TICKS_READ() - hud.begin_ticks;
	if (!(++hud.views % 240))
	{
		extern unsigned long n64_emulated_count;
		static unsigned long emulated;

		debugf("hud: %lu fast quads, %lu uploads skipped\n", hud.fast_quads, hud.skipped_uploads);
		hud.fast_quads = hud.skipped_uploads = 0;
		debugf("hud: %lu us per view (quads %lu, meters %lu), %lu quads; %lu emulated instructions per view\n",
			TICKS_TO_US(hud.ticks) / 240, TICKS_TO_US(hud.quad_ticks) / 240, TICKS_TO_US(hud.meter_ticks) / 240,
			hud.quads / 240, (n64_emulated_count - emulated) / 240);
		emulated = n64_emulated_count;
		hud.ticks = hud.quad_ticks = hud.meter_ticks = hud.quads = 0;
	}
}

static unsigned channel(unsigned long color, int shift)
{
	return (color >> shift) & 0xFF;
}

static void set_mode(int mode)
{
	if (hud.mode == mode)
		return;
	rdpq_sync_pipe();
	rdpq_set_mode_standard();
	rdpq_mode_filter(FILTER_BILINEAR);
	switch (mode)
	{
	case _mode_over:
		rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (TEX0, 0, PRIM, 0)));
		rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
		break;
	case _mode_meter:
		rdpq_mode_combiner(RDPQ_COMBINER_TEX);
		rdpq_mode_alphacompare(1);
		break;
	case _mode_meter_glow:
		rdpq_mode_combiner(RDPQ_COMBINER_TEX);
		rdpq_mode_alphacompare(1);
		rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY_CONST);
		rdpq_set_fog_color(RGBA32(0, 0, 0, 0xB0));
		break;
	case _mode_blip:
		/* the dot's intensity times the blip's as alpha */
		rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, PRIM), (TEX0, 0, PRIM, 0)));
		rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
		break;
	}
	hud.mode = mode;
}

/* a colour Halo adds (colour * texture + framebuffer * (1 - alpha * texture
alpha)) as one the RDP blends over the framebuffer (its additive blending
wraps around): mostly opaque colours keep their alpha (the colour brightened
by 1 / alpha, so that colour * alpha is Halo's); mostly transparent ones, a
glow, take their brightest channel as alpha (the colour brightened to full) */
static color_t over_color(unsigned long argb)
{
	unsigned a = argb >> 24, r = channel(argb, 16), g = channel(argb, 8), b = channel(argb, 0);

	if (a < 0x80)
	{
		a = r > g ? r : g;
		a = a > b ? a : b;
	}
	if (!a)
		return RGBA32(0, 0, 0, 0);
	return RGBA32(MIN(255u, r * 255 / a), MIN(255u, g * 255 / a), MIN(255u, b * 255 / a), a);
}

static void set_quad_color(unsigned long argb)
{
	set_mode(_mode_over);
	rdpq_set_prim_color(over_color(argb));
}

/* the part of a meter's sprite a quad shows, as Halo's meter shader colours
it (n64.h), in RGBA16. Halo adds it to the framebuffer times the tint's
alpha (full texels) or the background's (empty ones): a meter keeping half
or more of the framebuffer is drawn partly over it, the rest opaque; an
empty part keeping half or more is cut out, as is the outside of the shape */
static surface_t *meter_surface(const struct n64_hud_quad *quad, sprite_t *sprite, int s0, int t0, int width, int height)
{
	struct meter_entry *entry, *oldest = &meters[0];
	surface_t source = sprite_get_pixels(sprite);
	unsigned minimum_alpha = quad->minimum_color >> 24, maximum_alpha = quad->flash_color >> 24;
	unsigned scale = channel(quad->tint_color, 0);
	unsigned empty_alpha = quad->background_color >> 24;
	uint16_t empty;

	meter_clock++;
	for (int i = 0; i < METER_CACHE; i++)
	{
		struct meter_entry *m = &meters[i];

		if (m->sprite == quad->sprite && m->s0 == s0 && m->t0 == t0 && m->width == width && m->height == height &&
			m->minimum_color == quad->minimum_color && m->maximum_color == quad->maximum_color &&
			m->flash_color == quad->flash_color && m->background_color == quad->background_color &&
			m->tint_color == quad->tint_color)
		{
			m->last_used = meter_clock;
			return &m->surface;
		}
		if (m->last_used < oldest->last_used)
			oldest = m;
	}
	entry = oldest;
	if (!entry->slot.buffer)
		return NULL;
	width = MIN(width, METER_SLOT_WIDTH);
	height = MIN(height, METER_SLOT_HEIGHT);
	entry->surface = surface_make_sub(&entry->slot, 0, 0, width, height);
	entry->sprite = quad->sprite;
	entry->s0 = s0;
	entry->t0 = t0;
	entry->width = width;
	entry->height = height;
	entry->minimum_color = quad->minimum_color;
	entry->maximum_color = quad->maximum_color;
	entry->flash_color = quad->flash_color;
	entry->background_color = quad->background_color;
	entry->tint_color = quad->tint_color;
	entry->last_used = meter_clock;
	/* the empty part: the background where it covers (its alpha is what
	the framebuffer keeps), else nothing */
	if (empty_alpha < 0x80)
	{
		unsigned r = channel(quad->background_color, 16) * scale / 255;
		unsigned g = channel(quad->background_color, 8) * scale / 255;
		unsigned b = channel(quad->background_color, 0) * scale / 255;

		empty = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1;
	}
	else
		empty = 0;
	for (int y = 0; y < height; y++)
	{
		const uint8_t *in = (const uint8_t *)source.buffer + (t0 + y) * source.stride + s0 * 2;
		uint16_t *out = (uint16_t *)((uint8_t *)entry->surface.buffer + y * entry->surface.stride);

		for (int x = 0; x < width; x++)
		{
			unsigned order = in[x * 2], shape = in[x * 2 + 1];

			if (!shape)
				out[x] = 0;
			else if (order >= maximum_alpha)
				out[x] = empty;
			else
			{
				unsigned k = shape * scale / 255;
				unsigned r = (channel(quad->minimum_color, 16) * (255 - order) + channel(quad->maximum_color, 16) * order) / 255;
				unsigned g = (channel(quad->minimum_color, 8) * (255 - order) + channel(quad->maximum_color, 8) * order) / 255;
				unsigned b = (channel(quad->minimum_color, 0) * (255 - order) + channel(quad->maximum_color, 0) * order) / 255;

				if (order >= minimum_alpha)
				{
					r = MIN(255u, r + channel(quad->flash_color, 16));
					g = MIN(255u, g + channel(quad->flash_color, 8));
					b = MIN(255u, b + channel(quad->flash_color, 0));
				}
				r = r * k / 255;
				g = g * k / 255;
				b = b * k / 255;
				out[x] = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1;
			}
		}
	}
	data_cache_hit_writeback(entry->surface.buffer, entry->surface.stride * entry->surface.height);
	return &entry->surface;
}

/* a rotated quad: textured triangles, the bitmap loaded into TMEM in strips
of rows (with one row more for the filtering), each strip drawn as the part
of the quad its rows cover (the quad maps the bitmap affinely) */
static void draw_rotated(const struct n64_hud_quad *quad, const surface_t *surface)
{
	tex_format_t format = surface_get_format(surface);
	int row_bytes = (TEX_FORMAT_PIX2BYTES(format, surface->width) + 7) & ~7;
	int strip = 4096 / row_bytes - 1;
	float ds = quad->s[1] - quad->s[0], dt = quad->t[3] - quad->t[0];
	float t_low = fminf(quad->t[0], quad->t[3]), t_high = fmaxf(quad->t[0], quad->t[3]);

	if (strip < 1 || fabsf(dt) < 1e-3f || fabsf(ds) < 1e-3f)
		return;
	for (int t0 = (int)fmaxf(floorf(t_low), 0); t0 < (int)ceilf(fminf(t_high, surface->height)); t0 += strip)
	{
		int t1 = MIN(t0 + strip, surface->height);
		/* the strip's rows as fractions along the quad's left edge (v0 to v3) */
		float a = (fmaxf(t0, t_low) - quad->t[0]) / dt, b = (fminf(t1, t_high) - quad->t[0]) / dt;
		float v[4][5];

		rdpq_tex_upload_sub(TILE0, surface, NULL, 0, t0, surface->width, MIN(t1 + 1, surface->height));
		for (int i = 0; i < 4; i++)
		{
			/* corners 0 and 3 on the left edge (v0 to v3), 1 and 2 on the
			right (v1 to v2); 0 and 1 at fraction a, 2 and 3 at b */
			float f = i < 2 ? a : b;
			int from = i == 0 || i == 3 ? 0 : 1, to = from ? 2 : 3;

			v[i][0] = quad->x[from] + (quad->x[to] - quad->x[from]) * f;
			v[i][1] = quad->y[from] + (quad->y[to] - quad->y[from]) * f;
			v[i][2] = quad->s[from] + (quad->s[to] - quad->s[from]) * f;
			v[i][3] = quad->t[from] + (quad->t[to] - quad->t[from]) * f;
			v[i][4] = 1.0f;
		}
		rdpq_triangle(&TRIFMT_TEX, v[0], v[1], v[2]);
		rdpq_triangle(&TRIFMT_TEX, v[0], v[2], v[3]);
	}
}

void n64_hud_quad(const struct n64_hud_quad *quad)
{
	sprite_t *sprite = hud_sprite(quad->sprite);
	unsigned long start = TICKS_READ();
	surface_t surface;
	float s0, t0, s1, t1;
	int is0, it0, is1, it1;
	int upright;

	hud.quads++;
	if (hud.dump)
		debugf("hud quad %d: %d,%d %d,%d %d,%d %d,%d st %d,%d-%d,%d color %08lx meter %d %08lx %08lx %08lx %08lx %08lx%s\n",
			quad->sprite, (int)quad->x[0], (int)quad->y[0], (int)quad->x[1], (int)quad->y[1],
			(int)quad->x[2], (int)quad->y[2], (int)quad->x[3], (int)quad->y[3],
			(int)quad->s[0], (int)quad->t[0], (int)quad->s[2], (int)quad->t[2], quad->color, quad->meter,
			quad->minimum_color, quad->maximum_color, quad->flash_color, quad->background_color, quad->tint_color,
			sprite ? "" : " (missing)");
	if (!sprite)
		return;
	surface = sprite_get_pixels(sprite);

	/* v0 top left, v1 top right, v2 bottom right, v3 bottom left */
	upright = fabsf(quad->y[0] - quad->y[1]) < 0.5f && fabsf(quad->x[1] - quad->x[2]) < 0.5f &&
		quad->x[1] > quad->x[0] && quad->y[2] > quad->y[1];
	s0 = fminf(quad->s[0], quad->s[2]);
	s1 = fmaxf(quad->s[0], quad->s[2]);
	t0 = fminf(quad->t[0], quad->t[2]);
	t1 = fmaxf(quad->t[0], quad->t[2]);
	is0 = (int)floorf(fmaxf(s0, 0));
	it0 = (int)floorf(fmaxf(t0, 0));
	is1 = (int)ceilf(fminf(s1, surface.width));
	it1 = (int)ceilf(fminf(t1, surface.height));
	if (is1 <= is0 || it1 <= it0)
		return;
	if (quad->meter)
	{
		unsigned long meter_start = TICKS_READ();

		/* the meter's own texture holds just the part shown */
		surface_t *meter = meter_surface(quad, sprite, is0, it0, is1 - is0, it1 - it0);

		if (!meter)
			return;
		surface = *meter;
		hud.meter_ticks += TICKS_READ() - meter_start;
		s0 -= is0;
		s1 -= is0;
		t0 -= it0;
		t1 -= it0;
		is1 = MIN(is1 - is0, surface.width);
		it1 = MIN(it1 - it0, surface.height);
		is0 = it0 = 0;
		set_mode(quad->tint_color >> 24 >= 0x80 ? _mode_meter_glow : _mode_meter);
	}
	else
		set_quad_color(quad->color);

	if (upright && s1 > s0 && t1 > t0)
	{
		int width = is1 - is0, height = it1 - it0;
		float scale_x = (quad->x[1] - quad->x[0]) / (s1 - s0);
		float scale_y = (quad->y[2] - quad->y[1]) / (t1 - t0);
		int flip_x = quad->s[0] > quad->s[2], flip_y = quad->t[0] > quad->t[2];
		int row_bytes = (TEX_FORMAT_PIX2BYTES(surface_get_format(&surface), width) + 7) & ~7;

		/* a rectangle of the sprite that fits TMEM, unflipped: one upload
		(none when the last quad's is still there) and one scaled rectangle
		-- rdpq_tex_blit planned its strips on the CPU for every quad */
		if (!flip_x && !flip_y && !quad->meter && row_bytes * height <= 4096)
		{
			if (hud.tmem_pixels != surface.buffer || hud.tmem_s0 != is0 || hud.tmem_t0 != it0 ||
				hud.tmem_s1 != is1 || hud.tmem_t1 != it1)
			{
				rdpq_tex_upload_sub(TILE0, &surface, NULL, is0, it0, is1, it1);
				hud.tmem_pixels = surface.buffer;
				hud.tmem_s0 = is0;
				hud.tmem_t0 = it0;
				hud.tmem_s1 = is1;
				hud.tmem_t1 = it1;
			}
			else
				hud.skipped_uploads++;
			rdpq_texture_rectangle_scaled(TILE0, quad->x[0], quad->y[0],
				quad->x[0] + width * scale_x, quad->y[0] + height * scale_y, is0, it0, is1, it1);
			hud.fast_quads++;
		}
		else
		{
			hud.tmem_pixels = NULL;
			rdpq_tex_blit(&surface, quad->x[0], quad->y[0], &(rdpq_blitparms_t){
				.s0 = is0, .t0 = it0, .width = width, .height = height,
				.scale_x = scale_x, .scale_y = scale_y, .flip_x = flip_x, .flip_y = flip_y,
			});
		}
	}
	else if (!quad->meter)
	{
		hud.tmem_pixels = NULL;
		draw_rotated(quad, &surface);
	}
	hud.quad_ticks += TICKS_READ() - start;
}

void n64_hud_text(int x, int y, unsigned long color, const char *text)
{
	static struct n64_font *font;
	static int font_loaded;

	if (hud.dump)
		debugf("hud text %d,%d: %s\n", x, y, text);
	hud.mode = _mode_none;
	hud.tmem_pixels = NULL;
	/* Halo's small font (the menu's), from the top of the text's box */
	if (!font_loaded)
	{
		font = n64_font_load("small");
		font_loaded = 1;
	}
	if (!font)
	{
		rdpq_sync_pipe();
		rdpq_text_print(&(rdpq_textparms_t){0}, FONT_BUILTIN_DEBUG_MONO, x, y + 8, text);
		return;
	}
	n64_text_draw(font, x, y + n64_font_ascent(font), color, text);
}

void n64_hud_blip(float x, float y, float size, unsigned long color)
{
	float scale = size / BLIP_SIZE;

	if (hud.dump)
		debugf("hud blip %d,%d size %d color %08lx\n", (int)x, (int)y, (int)size, color);
	hud.tmem_pixels = NULL;
	if (!(color & 0xF0F0F0) || !hud.blip.buffer)
		return;
	set_mode(_mode_blip);
	rdpq_set_prim_color(over_color(color & 0xFFFFFF));
	rdpq_tex_blit(&hud.blip, x - size * 0.5f, y - size * 0.5f, &(rdpq_blitparms_t){
		.scale_x = scale, .scale_y = scale,
	});
}
