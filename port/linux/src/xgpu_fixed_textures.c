/*
XGPU_FIXED_TEXTURES.C

The light renderer's textures (xgpu_fixed.h): the Xbox's textures as OpenGL
1.1 takes them.

- Only 2D textures are uploaded. The passes that need cube and volume
  textures are not drawn by this renderer, and a unit that is given one
  binds a white texel.
- OpenGL 1.1 has power-of-two textures only: a linear texture (the Xbox
  addresses those in texels and they have any size) is uploaded into the
  next power of two, its edge texels repeated into the padding, and the
  texture matrix scales the coordinates to the part that is the picture.
- No dimension may be above GL_MAX_TEXTURE_SIZE: the levels above it are
  left out, or, where there are none below, made smaller here.
- Texels are 16 bits unless display.gl1_texture_bits is 32: 5:6:5, 5:5:5:1
  or 4:4:4:4 by what the texture's alpha holds. Luminance formats stay as
  they are. DXT goes to the driver compressed where it has S3TC and is
  decoded otherwise.
- A mipmapped texture gets a complete chain of levels, down to one texel
  (OpenGL 1.1 samples no texture whose chain stops early); the levels the
  Xbox's texture lacks are made from its last.

The cache is xbox_textures.c's: an entry for each texture header and
palette, valid until a page its texels were read from is written
(memory_watch.c).

Two more kinds of texture are made here: the copy of the window's back
buffer that a draw sampling the frame reads, and the derived textures of
nv2a_combiner_ff.h's plans, which are a texture's texels put through a
function of the plan.
*/

#include "xgpu_fixed.h"

#if !defined(HALO_ANDROID) && !defined(HALO_XBOX)

#include "xgpu_fixed_gl.h"
#include "xbox_textures_convert.h"
#include "nv2a_combiner_ff.h"
#include "port_config.h"

#include <stdlib.h>
#include <string.h>

/* ---------- sampler state

OpenGL 1.1 keeps the filters and the address modes with each texture: what
a texture was last given is kept with it, so that drawing with the same
state again costs nothing. */

#define SAMPLER_INPUT_COUNT 7

struct sampler_shadow
{
	BOOL valid;
	DWORD inputs[SAMPLER_INPUT_COUNT];
};

static GLenum address_mode(DWORD mode)
{
	switch (mode)
	{
	case D3DTADDRESS_MIRROR:
		return xgpu_fixed_context.mirrored_repeat ? GL_MIRRORED_REPEAT : GL_REPEAT;
	case D3DTADDRESS_CLAMP:
	case D3DTADDRESS_CLAMPTOEDGE:
		return xgpu_fixed_context.edge_clamp ? GL_CLAMP_TO_EDGE : GL_CLAMP;
	/* (OpenGL 1.1's clamp runs into the border colour) */
	case D3DTADDRESS_BORDER: return GL_CLAMP;
	default: return GL_REPEAT;
	}
}

void xgpu_fixed_texture_parameters(const struct xgpu_fixed_texture *texture, int stage)
{
	struct sampler_shadow *shadow = texture->sampler;
	DWORD *state = D3D__TextureState[stage];
	DWORD min_filter = state[D3DTSS_MINFILTER];
	DWORD mip_filter = texture->mipmapped ? state[D3DTSS_MIPFILTER] : D3DTEXF_NONE;
	BOOL border = state[D3DTSS_ADDRESSU] == D3DTADDRESS_BORDER || state[D3DTSS_ADDRESSV] == D3DTADDRESS_BORDER;
	BOOL level_of_detail = texture->mipmapped && xgpu_fixed_context.texture_lod;
	DWORD inputs[SAMPLER_INPUT_COUNT];
	GLenum minification;

	inputs[0] = min_filter;
	inputs[1] = mip_filter;
	inputs[2] = state[D3DTSS_MAGFILTER];
	inputs[3] = state[D3DTSS_ADDRESSU];
	inputs[4] = state[D3DTSS_ADDRESSV];
	inputs[5] = border ? state[D3DTSS_BORDERCOLOR] : 0;
	inputs[6] = level_of_detail ? state[D3DTSS_MAXMIPLEVEL] : 0;
	if (shadow && shadow->valid && !memcmp(shadow->inputs, inputs, sizeof(inputs)))
		return;
	if (shadow)
	{
		shadow->valid = TRUE;
		memcpy(shadow->inputs, inputs, sizeof(inputs));
	}

	if (min_filter == D3DTEXF_POINT)
		minification = mip_filter == D3DTEXF_NONE ? GL_NEAREST :
			mip_filter == D3DTEXF_POINT ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR;
	else
		minification = mip_filter == D3DTEXF_NONE ? GL_LINEAR :
			mip_filter == D3DTEXF_POINT ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minification);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, state[D3DTSS_MAGFILTER] == D3DTEXF_POINT ? GL_NEAREST : GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)address_mode(state[D3DTSS_ADDRESSU]));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)address_mode(state[D3DTSS_ADDRESSV]));
	if (border)
	{
		D3DCOLOR color = state[D3DTSS_BORDERCOLOR];
		float rgba[4];

		rgba[0] = ((color >> 16) & 0xff) / 255.0f;
		rgba[1] = ((color >> 8) & 0xff) / 255.0f;
		rgba[2] = (color & 0xff) / 255.0f;
		rgba[3] = ((color >> 24) & 0xff) / 255.0f;
		glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, rgba);
	}
	if (level_of_detail)
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, (float)state[D3DTSS_MAXMIPLEVEL]);
}

/* ---------- formats */

/* what OpenGL is given for a texture: the converter's destination
(xbox_textures_convert.h) and the arguments of glTexImage2D */
struct upload_format
{
	int destination;
	GLint internal;
	GLenum format, type;
};

/* 32-bit texels in the order the context takes */
static void upload_format_words(struct upload_format *format, GLint internal)
{
	format->destination = xgpu_fixed_context.bgra ? _texture_destination_bgra8 : _texture_destination_rgba8;
	format->internal = internal;
	format->format = xgpu_fixed_context.bgra ? GL_BGRA : GL_RGBA;
	format->type = GL_UNSIGNED_BYTE;
}

/* alpha is what the texture's alpha holds (_texture_alpha_*).

The luminance and intensity formats keep the Xbox's texel as it is, and
OpenGL's combine environments read them as the Xbox does (luminance with an
alpha of 1, intensity in every channel). Its own environments of OpenGL 1.1
do not: REPLACE takes the alpha from the fragment where the texture has
none, DECAL is undefined on them, BLEND and ADD put an intensity into the
alpha. With the combine extension the renderer spells those environments
out (xgpu_fixed.c, unit_environment); without it the texels go as red,
green, blue, alpha, which every environment reads alike, and opaque ones
keep an alpha of their own (the one bit of 5:5:5:1) for REPLACE's sake. */
static void upload_format_choose(int kind, unsigned long alpha, struct upload_format *format)
{
	int sixteen;

	switch (kind)
	{
	case _texture_kind_l8:
	case _texture_kind_al8:
		if (!xgpu_fixed_context.combine)
			break;
		/* (an AL8 texel is its byte in every channel: an intensity) */
		format->destination = _texture_destination_y8;
		format->internal = kind == _texture_kind_l8 ? GL_LUMINANCE8 : GL_INTENSITY8;
		format->format = GL_LUMINANCE;
		format->type = GL_UNSIGNED_BYTE;
		return;
	case _texture_kind_a8l8:
		if (!xgpu_fixed_context.combine)
			break;
		format->destination = _texture_destination_y8a8;
		format->internal = GL_LUMINANCE8_ALPHA8;
		format->format = GL_LUMINANCE_ALPHA;
		format->type = GL_UNSIGNED_BYTE;
		return;
	case _texture_kind_a8:
		if (!xgpu_fixed_context.combine)
			break;
		/* white with the alpha (an OpenGL alpha texture's colour would be
		black to a combine environment) */
		upload_format_words(format, GL_LUMINANCE8_ALPHA8);
		return;
	default:
		break;
	}
	if (xgpu_fixed_context.textures_32bit || (!xgpu_fixed_context.combine && (kind == _texture_kind_l8 ||
		kind == _texture_kind_al8 || kind == _texture_kind_a8l8 || kind == _texture_kind_a8)))
	{
		/* (the four kinds above keep their eight bits without the combine
		extension: they are small, and the masks and glows they hold band
		in four) */
		upload_format_words(format, GL_RGBA8);
		return;
	}
	sixteen = texture_convert_destination_16bit(alpha);
	if (sixteen == _texture_destination_r5g6b5 && !xgpu_fixed_context.combine)
		sixteen = _texture_destination_rgb5a1;
	if (sixteen == _texture_destination_r5g6b5)
	{
		if (xgpu_fixed_context.packed_565)
		{
			format->destination = _texture_destination_r5g6b5;
			format->internal = GL_RGB5;
			format->format = GL_RGB;
			format->type = GL_UNSIGNED_SHORT_5_6_5;
			return;
		}
		/* (the packed pixels extension has no 5:6:5 type) */
		sixteen = _texture_destination_rgb5a1;
		format->internal = GL_RGB5;
	}
	else
	{
		format->internal = sixteen == _texture_destination_rgb5a1 ? GL_RGB5_A1 : GL_RGBA4;
	}
	if (xgpu_fixed_context.packed_pixels)
	{
		format->destination = sixteen;
		format->format = GL_RGBA;
		format->type = sixteen == _texture_destination_rgb5a1 ? GL_UNSIGNED_SHORT_5_5_5_1 : GL_UNSIGNED_SHORT_4_4_4_4;
		return;
	}
	/* bytes, which the driver packs */
	upload_format_words(format, format->internal);
}

static GLenum compressed_format(int kind)
{
	switch (kind)
	{
	case _texture_kind_dxt1: return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
	case _texture_kind_dxt3: return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
	default: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
	}
}

static unsigned long level_dimension(unsigned long base, unsigned long level)
{
	unsigned long value = base >> level;

	return value ? value : 1;
}

/* ---------- derived textures: a plan's function over the texels */

struct bake_function
{
	const struct nv2a_ff_plan *plan;
	int bake;
	const struct nv2a_ff_constants *constants;
};

/* textures have few different texels next to each other: the last results
are kept by the texel's value */
#define BAKE_MEMORY 1024

static void bake_apply(const struct bake_function *function, unsigned int *texels, unsigned long count, BOOL bgra)
{
	static struct
	{
		unsigned int texel, baked;
		BOOL valid;
	} memory[BAKE_MEMORY];
	unsigned long index;
	int red = bgra ? 16 : 0, blue = bgra ? 0 : 16;

	memset(memory, 0, sizeof(memory));
	for (index = 0; index < count; index++)
	{
		unsigned int texel = texels[index];
		unsigned long slot = ((texel * 2654435761UL) >> 16) % BAKE_MEMORY;

		if (!memory[slot].valid || memory[slot].texel != texel)
		{
			float in[4], out[4];
			unsigned int bytes[4];
			int channel;

			in[0] = (float)((texel >> red) & 0xff) / 255.0f;
			in[1] = (float)((texel >> 8) & 0xff) / 255.0f;
			in[2] = (float)((texel >> blue) & 0xff) / 255.0f;
			in[3] = (float)(texel >> 24) / 255.0f;
			nv2a_ff_plan_bake(function->plan, function->bake, function->constants, in, out);
			for (channel = 0; channel < 4; channel++)
				bytes[channel] = out[channel] <= 0.0f ? 0 : out[channel] >= 1.0f ? 255 :
					(unsigned int)(out[channel] * 255.0f + 0.5f);
			memory[slot].texel = texel;
			memory[slot].baked = (bytes[0] << red) | (bytes[1] << 8) | (bytes[2] << blue) | (bytes[3] << 24);
			memory[slot].valid = TRUE;
		}
		texels[index] = memory[slot].baked;
	}
}

/* what tells one function from another: its results for a few texels */
static unsigned long bake_fingerprint(const struct bake_function *function)
{
	static const float probes[][4] =
	{
		{ 0.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 0.5f },
		{ 0.0f, 0.0f, 1.0f, 0.25f }, { 0.5f, 0.5f, 0.5f, 0.75f }, { 0.25f, 0.75f, 0.5f, 0.0f }, { 0.75f, 0.25f, 1.0f, 1.0f },
		{ 0.1f, 0.9f, 0.3f, 0.6f }, { 0.6f, 0.4f, 0.8f, 0.2f },
	};
	unsigned long hash = 2166136261UL, probe;
	int channel;

	for (probe = 0; probe < sizeof(probes) / sizeof(probes[0]); probe++)
	{
		float out[4];

		nv2a_ff_plan_bake(function->plan, function->bake, function->constants, probes[probe], out);
		for (channel = 0; channel < 4; channel++)
			hash = (hash ^ (unsigned long)(long)(out[channel] * 4095.0f + 0.5f)) * 16777619UL;
	}
	return hash ? hash : 1;
}

/* ---------- upload */

/* the next smaller level of an image, by box filter; returns its size */
static void halve_image(int destination_format, unsigned long bytes, void *smaller, const void *larger,
	unsigned long *width, unsigned long *height)
{
	unsigned long pitch = *width * bytes;
	unsigned long smaller_width = *width > 1 ? *width / 2 : 1;
	unsigned long smaller_height = *height > 1 ? *height / 2 : 1;
	unsigned long y;

	for (y = 0; y < smaller_height; y++)
	{
		const unsigned char *row0 = (const unsigned char *)larger + (*height > 1 ? 2 * y : 0) * pitch;
		const unsigned char *row1 = *height > 1 ? row0 + pitch : row0;

		texture_convert_halve_row(destination_format, (unsigned char *)smaller + y * smaller_width * bytes,
			row0, row1, NULL, NULL, *width);
	}
	*width = smaller_width;
	*height = smaller_height;
}

/* the base internal format (xgpu_fixed.h) of what glTexImage2D was given */
static int upload_base_format(const struct upload_format *format)
{
	switch (format->internal)
	{
	case GL_LUMINANCE8: return _xgpu_fixed_base_luminance;
	case GL_INTENSITY8: return _xgpu_fixed_base_intensity;
	case GL_LUMINANCE8_ALPHA8: return _xgpu_fixed_base_luminance_alpha;
	case GL_RGB5: return _xgpu_fixed_base_rgb;
	default: return _xgpu_fixed_base_rgba;
	}
}

/* Gives the texture bound to the active unit the levels of an Xbox texture
(or of what function makes of them). Returns whether it has a chain of mip
levels; *base_format is the base internal format they were given in. */
static BOOL upload(const struct xgpu_texture_description *description, const unsigned char *base,
	const D3DCOLOR *palette, const struct bake_function *function, int *base_format)
{
	int kind = xgpu_texture_format_kind(description->format);
	unsigned long width = description->width, height = description->height;
	unsigned long levels = description->levels ? description->levels : 1;
	/* (only linear textures have other sizes than powers of two) */
	unsigned long padded_width = texture_convert_power_of_two(width);
	unsigned long padded_height = texture_convert_power_of_two(height);
	BOOL padded = padded_width != width || padded_height != height;
	unsigned long drop = texture_convert_levels_to_drop(padded_width, padded_height, 1,
		(unsigned long)xgpu_fixed_context.maximum_texture_size);
	unsigned long first = drop < levels ? drop : levels - 1;
	unsigned long halvings = drop - first;
	unsigned long level, output_level = 0, bytes, current_width = 0, current_height = 0;
	struct upload_format format;
	unsigned char *buffers[2], *current = NULL;
	unsigned long alpha = _texture_alpha_full;

	if (padded)
		levels = 1, first = 0, halvings = drop;
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	*base_format = _xgpu_fixed_base_rgba;

	if (description->compressed && xgpu_fixed_context.s3tc && !function && !halvings && !padded)
	{
		unsigned long last_width = level_dimension(width, levels - 1), last_height = level_dimension(height, levels - 1);

		for (level = first; level < levels; level++)
		{
			unsigned long level_width = level_dimension(width, level), level_height = level_dimension(height, level);
			unsigned long size = ((level_width + 3) / 4) * ((level_height + 3) / 4) * texture_convert_kind_bytes(kind);

			glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)(level - first), compressed_format(kind),
				(GLsizei)level_width, (GLsizei)level_height, 0, (GLsizei)size,
				base + xgpu_texture_level_offset(description, level));
		}
		if (levels - first < 2)
			return FALSE;
		if (last_width == 1 && last_height == 1)
			return TRUE;
		/* a chain that stops early: OpenGL 1.2 can be told so; 1.1 samples
		the first level alone */
		if (xgpu_fixed_context.texture_lod)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)(levels - first - 1));
			return TRUE;
		}
		return FALSE;
	}

	/* what the alpha of the first level uploaded holds decides among the
	16-bit formats */
	if (function)
	{
		upload_format_words(&format, xgpu_fixed_context.textures_32bit ? GL_RGBA8 : GL_RGBA4);
	}
	else
	{
		alpha = texture_convert_kind_alpha(kind);
		if (alpha != _texture_alpha_opaque && !xgpu_fixed_context.textures_32bit)
		{
			const unsigned char *source = base + xgpu_texture_level_offset(description, first);
			unsigned long level_width = level_dimension(width, first), level_height = level_dimension(height, first);
			unsigned long y;

			alpha = _texture_alpha_opaque;
			if (description->compressed)
			{
				unsigned long block_bytes = texture_convert_kind_bytes(kind);
				unsigned long pitch = ((level_width + 3) / 4) * block_bytes;

				for (y = 0; y < level_height && alpha != _texture_alpha_full; y += 4)
					alpha |= texture_convert_dxt_alpha(kind, source + (y / 4) * pitch, level_width,
						level_height - y < 4 ? level_height - y : 4);
			}
			else if (description->linear)
			{
				for (y = 0; y < level_height && alpha != _texture_alpha_full; y++)
					alpha |= texture_convert_alpha(kind, source + y * description->pitch, level_width, palette);
			}
			else
			{
				alpha = texture_convert_alpha(kind, source, level_width * level_height, palette);
			}
		}
		upload_format_choose(kind, alpha, &format);
	}
	*base_format = upload_base_format(&format);
	bytes = texture_convert_destination_bytes(format.destination);

	{
		unsigned long largest_width = padded ? padded_width : level_dimension(width, first);
		unsigned long largest_height = padded ? padded_height : level_dimension(height, first);

		buffers[0] = malloc(largest_width * largest_height * bytes + 16);
		buffers[1] = malloc(largest_width * largest_height * bytes / 2 + 64);
		if (!buffers[0] || !buffers[1])
		{
			free(buffers[0]);
			free(buffers[1]);
			return FALSE;
		}
	}
	for (level = first; level < levels; level++)
	{
		const unsigned char *source = base + xgpu_texture_level_offset(description, level);
		unsigned long level_width = level_dimension(width, level), level_height = level_dimension(height, level);

		current = buffers[0];
		current_width = padded ? padded_width : level_width;
		current_height = padded ? padded_height : level_height;
		if (description->compressed)
			texture_convert_dxt_level(kind, format.destination, current, current_width * bytes, source,
				level_width, level_height, 1);
		else
			texture_convert_level(kind, format.destination, current, current_width * bytes, source,
				level_width, level_height, 1, description->linear ? description->pitch : 0, palette);
		if (padded)
		{
			unsigned long y;

			for (y = 0; y < level_height; y++)
				texture_convert_pad_row(current + y * current_width * bytes, level_width, current_width, bytes);
			texture_convert_pad_rows(current, current_width * bytes, level_height, current_height);
		}
		if (function)
			bake_apply(function, (unsigned int *)current, current_width * current_height,
				format.destination == _texture_destination_bgra8);
		if (level == first)
		{
			/* no level is small enough: this one is made smaller */
			for (; halvings; halvings--)
			{
				unsigned char *smaller = current == buffers[0] ? buffers[1] : buffers[0];

				halve_image(format.destination, bytes, smaller, current, &current_width, &current_height);
				current = smaller;
			}
		}
		glTexImage2D(GL_TEXTURE_2D, (GLint)output_level++, format.internal, (GLsizei)current_width,
			(GLsizei)current_height, 0, format.format, format.type, current);
	}
	/* the levels below the Xbox texture's last, down to one texel */
	if (levels > 1)
	{
		while (current_width > 1 || current_height > 1)
		{
			unsigned char *smaller = current == buffers[0] ? buffers[1] : buffers[0];

			halve_image(format.destination, bytes, smaller, current, &current_width, &current_height);
			current = smaller;
			glTexImage2D(GL_TEXTURE_2D, (GLint)output_level++, format.internal, (GLsizei)current_width,
				(GLsizei)current_height, 0, format.format, format.type, current);
		}
	}
	free(buffers[0]);
	free(buffers[1]);
	return levels > 1;
}

/* ---------- the cache */

struct bake
{
	struct bake *next;
	unsigned long fingerprint;
	/* the texture's generation when this was made from it */
	unsigned long generation;
	GLuint name;
	BOOL mipmapped;
	int base;
	unsigned long last_used_frame;
	struct sampler_shadow sampler;
};

struct texture_entry
{
	struct texture_entry *next;
	DWORD data, format_word, size_word;
	unsigned long palette_hash;
	GLuint name;
	struct xgpu_texture_description description;
	unsigned long address, size;
	unsigned long generation;
	unsigned long last_used_frame;
	BOOL mipmapped;
	/* _xgpu_fixed_base_* of the texels as they were uploaded */
	int base;
	float scale[2];
	struct sampler_shadow sampler;
	struct bake *bakes;
};

#define TEXTURE_BUCKET_COUNT 4096
#define TEXTURE_IDLE_FRAMES 1800
#define MAXIMUM_PALETTE_VARIANTS 8
/* Derived textures kept for one texture. A plan made for the constants of
the moment has their values in its derived textures, so one texture drawn
by several such plans (objects of different colours) has one for each. */
#define MAXIMUM_BAKES_A_TEXTURE 8
#define BAKE_IDLE_FRAMES 600
/* derived textures made in one frame: each is the whole texture converted
again, and a scene may ask for many at once */
#define BAKES_A_FRAME 4
#define RECENT_TEXTURE_COUNT 64

static struct texture_entry *texture_buckets[TEXTURE_BUCKET_COUNT];

/* as in xbox_textures.c: a lookup is remembered with the memory watch
serial it started at; while no watched page has been written since and no
texture has been dropped, the same lookup finds the same current texture */
static struct
{
	DWORD data, format_word, size_word;
	struct texture_entry *entry;
	unsigned long watch_serial;
	unsigned long drop_serial;
} recent_textures[RECENT_TEXTURE_COUNT];
static unsigned long texture_drop_serial = 1;
static unsigned long texture_frame;
static unsigned long bakes_this_frame;

/* debug.gpu_stats */
unsigned long xgpu_fixed_texture_uploads, xgpu_fixed_texture_bakes, xgpu_fixed_back_buffer_copies;

static unsigned long bucket_index(DWORD data, DWORD format_word, DWORD size_word)
{
	return ((data >> 7) ^ (format_word * 2654435761UL) ^ size_word) % TEXTURE_BUCKET_COUNT;
}

static unsigned long palette_hash(const D3DCOLOR *palette)
{
	unsigned long hash = 2166136261UL, index;

	if (!palette)
		return 0;
	for (index = 0; index < 256; index++)
		hash = (hash ^ palette[index]) * 16777619UL;
	return hash ? hash : 1;
}

/* a new texture, bound to the active unit behind the state cache's back */
static GLuint texture_new(void)
{
	GLuint name = 0;

	glGenTextures(1, &name);
	glBindTexture(GL_TEXTURE_2D, name);
	xgpu_fixed_state_invalidate();
	return name;
}

static struct texture_entry *texture_entry_get(const DWORD *resource, const D3DCOLOR *palette)
{
	DWORD data = resource[1], format_word = resource[3], size_word = resource[4];
	struct texture_entry **bucket = &texture_buckets[bucket_index(data, format_word, size_word)];
	struct texture_entry *entry, *oldest_variant = NULL;
	BOOL palettized = ((format_word & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT) == 0x0b;
	unsigned long hash = palettized ? palette_hash(palette) : 0;
	unsigned long variant_count = 0, generation;
	unsigned long recent = bucket_index(data, format_word, size_word) % RECENT_TEXTURE_COUNT;
	unsigned long watch_serial = memory_watch_serial();

	if (!palettized && recent_textures[recent].entry && recent_textures[recent].data == data &&
		recent_textures[recent].format_word == format_word && recent_textures[recent].size_word == size_word &&
		recent_textures[recent].watch_serial == watch_serial &&
		recent_textures[recent].drop_serial == texture_drop_serial)
	{
		entry = recent_textures[recent].entry;
		entry->last_used_frame = texture_frame;
		return entry;
	}
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->data == data && entry->format_word == format_word && entry->size_word == size_word)
		{
			if (entry->palette_hash == hash)
				break;
			variant_count++;
			if (!oldest_variant || entry->last_used_frame < oldest_variant->last_used_frame)
				oldest_variant = entry;
		}
	}
	if (!entry && variant_count >= MAXIMUM_PALETTE_VARIANTS)
	{
		struct bake *bake;

		/* a palette that keeps changing reuses the stalest copy; its derived
		textures were made with the old palette (the generation the upload
		below gets may be the one they carry) */
		entry = oldest_variant;
		entry->palette_hash = hash;
		entry->generation = 0;
		for (bake = entry->bakes; bake; bake = bake->next)
			bake->generation = 0;
	}
	if (!entry)
	{
		entry = calloc(1, sizeof(*entry));
		if (!entry)
			return NULL;
		entry->data = data;
		entry->format_word = format_word;
		entry->size_word = size_word;
		entry->palette_hash = hash;
		xgpu_texture_describe(format_word, size_word, &entry->description);
		entry->address = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(data);
		entry->size = xgpu_texture_face_size(&entry->description);
		entry->scale[0] = entry->scale[1] = 1.0f;
		if (entry->description.linear)
		{
			texture_convert_pad_scale(entry->description.width, entry->description.height,
				texture_convert_power_of_two(entry->description.width),
				texture_convert_power_of_two(entry->description.height), TRUE, entry->scale);
		}
		entry->next = *bucket;
		*bucket = entry;
	}

	generation = memory_watch_generation(entry->address, entry->size);
	if (!entry->generation || generation > entry->generation)
	{
		/* protect first, so a write racing with the upload is noticed */
		memory_watch_protect(entry->address, entry->size);
		entry->generation = memory_watch_generation(entry->address, entry->size);
		if (!entry->generation)
			entry->generation = 1;
		if (platform_is_contiguous((void *)entry->address) &&
			platform_is_contiguous((void *)(entry->address + entry->size - 1)))
		{
			if (!entry->name)
			{
				entry->name = texture_new();
			}
			else
			{
				glBindTexture(GL_TEXTURE_2D, entry->name);
				xgpu_fixed_state_invalidate();
			}
			entry->mipmapped = upload(&entry->description, (const unsigned char *)entry->address, palette, NULL, &entry->base);
			entry->sampler.valid = FALSE;
			xgpu_fixed_texture_uploads++;
		}
	}
	entry->last_used_frame = texture_frame;
	if (!palettized)
	{
		recent_textures[recent].data = data;
		recent_textures[recent].format_word = format_word;
		recent_textures[recent].size_word = size_word;
		recent_textures[recent].entry = entry;
		recent_textures[recent].watch_serial = watch_serial;
		recent_textures[recent].drop_serial = texture_drop_serial;
	}
	return entry;
}

/* ---------- the white texture */

void xgpu_fixed_white_texture(struct xgpu_fixed_texture *texture)
{
	static GLuint name;
	static struct sampler_shadow sampler;

	if (!name)
	{
		static const unsigned char white[4] = { 255, 255, 255, 255 };

		name = texture_new();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
	}
	memset(texture, 0, sizeof(*texture));
	texture->type = _xgpu_fixed_texture_none;
	texture->name = name;
	texture->scale[0] = texture->scale[1] = 1.0f;
	texture->sampler = &sampler;
}

/* ---------- off-screen targets and the back buffer */

#define OFF_SCREEN_TARGETS 256

static unsigned long off_screen_targets[OFF_SCREEN_TARGETS];

static unsigned long off_screen_slot(unsigned long data)
{
	return ((data >> 12) ^ (data >> 20) ^ data) % OFF_SCREEN_TARGETS;
}

static BOOL off_screen_target_known(unsigned long data)
{
	unsigned long slot = off_screen_slot(data), tries;

	for (tries = 0; tries < OFF_SCREEN_TARGETS && off_screen_targets[slot]; tries++)
	{
		if (off_screen_targets[slot] == data)
			return TRUE;
		slot = (slot + 1) % OFF_SCREEN_TARGETS;
	}
	return FALSE;
}

BOOL xgpu_fixed_off_screen_target(unsigned long data)
{
	unsigned long slot = off_screen_slot(data), tries;

	if (!data)
		return FALSE;
	for (tries = 0; tries < OFF_SCREEN_TARGETS - 1 && off_screen_targets[slot]; tries++)
	{
		if (off_screen_targets[slot] == data)
			return FALSE;
		slot = (slot + 1) % OFF_SCREEN_TARGETS;
	}
	if (off_screen_targets[slot])
		return FALSE;
	off_screen_targets[slot] = data;
	return TRUE;
}

BOOL xgpu_fixed_off_screen_sampled(unsigned long data)
{
	static unsigned long reported[OFF_SCREEN_TARGETS];
	unsigned long slot = off_screen_slot(data), tries;

	for (tries = 0; tries < OFF_SCREEN_TARGETS - 1 && reported[slot]; tries++)
	{
		if (reported[slot] == data)
			return FALSE;
		slot = (slot + 1) % OFF_SCREEN_TARGETS;
	}
	if (!data || reported[slot])
		return FALSE;
	reported[slot] = data;
	return TRUE;
}

/* the copies of the back buffer made in one frame: a draw that samples
the frame after it has been drawn into again gets a new copy, up to this
many (each is the whole picture) */
#define BACK_BUFFER_COPIES_A_FRAME 8

static struct
{
	GLuint name;
	unsigned long width, height;
	BOOL stale;
	unsigned long copies_this_frame;
	struct sampler_shadow sampler;
} back_buffer = { 0, 0, 0, TRUE, 0, { FALSE, { 0 } } };

void xgpu_fixed_back_buffer_drawn(void)
{
	back_buffer.stale = TRUE;
}

/* texture samples the back buffer through the Xbox texture header
description */
static BOOL back_buffer_copy(const struct xgpu_fixed_target *target, const struct xgpu_texture_description *description,
	struct xgpu_fixed_texture *texture)
{
	unsigned long width = texture_convert_power_of_two(target->gl_width);
	unsigned long height = texture_convert_power_of_two(target->gl_height);
	float used_width, used_height;

	if (!target->window || !target->gl_width || !target->gl_height ||
		width > (unsigned long)xgpu_fixed_context.maximum_texture_size ||
		height > (unsigned long)xgpu_fixed_context.maximum_texture_size)
	{
		static BOOL reported;

		if (!reported)
		{
			reported = TRUE;
			platform_log("gl1: the frame (%lux%lu) does not fit a texture (%d at most): what samples it is left out",
				target->gl_width, target->gl_height, xgpu_fixed_context.maximum_texture_size);
		}
		return FALSE;
	}
	if (!back_buffer.name || back_buffer.width != width || back_buffer.height != height)
	{
		if (!back_buffer.name)
			glGenTextures(1, &back_buffer.name);
		glBindTexture(GL_TEXTURE_2D, back_buffer.name);
		xgpu_fixed_state_invalidate();
		glTexImage2D(GL_TEXTURE_2D, 0, xgpu_fixed_context.alpha_bits ? GL_RGBA : GL_RGB, (GLsizei)width,
			(GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		back_buffer.width = width;
		back_buffer.height = height;
		back_buffer.stale = TRUE;
		back_buffer.sampler.valid = FALSE;
	}
	if (back_buffer.stale && back_buffer.copies_this_frame < BACK_BUFFER_COPIES_A_FRAME)
	{
		glBindTexture(GL_TEXTURE_2D, back_buffer.name);
		xgpu_fixed_state_invalidate();
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLint)target->window_x, (GLint)target->window_y,
			(GLsizei)target->gl_width, (GLsizei)target->gl_height);
		back_buffer.stale = FALSE;
		back_buffer.copies_this_frame++;
		xgpu_fixed_back_buffer_copies++;
	}
	memset(texture, 0, sizeof(*texture));
	texture->type = _xgpu_fixed_texture_back_buffer;
	texture->name = back_buffer.name;
	texture->base = xgpu_fixed_context.alpha_bits ? _xgpu_fixed_base_rgba : _xgpu_fixed_base_rgb;
	texture->sampler = &back_buffer.sampler;
	/* the picture is the lower left part of the texture, its rows from
	the bottom: t becomes 1 - t */
	used_width = (float)target->gl_width / (float)width;
	used_height = (float)target->gl_height / (float)height;
	if (description->linear)
	{
		/* (coordinates in the back buffer's texels) */
		used_width /= (float)target->width;
		texture->scale[1] = -used_height / (float)target->height;
	}
	else
	{
		texture->scale[1] = -used_height;
	}
	texture->scale[0] = used_width;
	texture->offset[1] = used_height;
	return TRUE;
}

/* ---------- a stage's texture */

int xgpu_fixed_texture_type(const struct xgpu_fixed_draw *draw, int stage)
{
	D3DBaseTexture *texture = draw->textures[stage];
	struct xgpu_texture_description description;

	if (!texture || !texture->Data)
		return _xgpu_fixed_texture_none;
	if (texture->Data == draw->target.back_buffer_data)
		return _xgpu_fixed_texture_back_buffer;
	if (off_screen_target_known(texture->Data))
		return _xgpu_fixed_texture_off_screen;
	xgpu_texture_describe(texture->Format, texture->Size, &description);
	if (description.cube_map)
		return _xgpu_fixed_texture_cube;
	if (description.depth > 1)
		return _xgpu_fixed_texture_volume;
	return _xgpu_fixed_texture_2d;
}

BOOL xgpu_fixed_texture_get(const struct xgpu_fixed_draw *draw, int stage, struct xgpu_fixed_texture *texture)
{
	D3DBaseTexture *resource = draw->textures[stage];
	int type = xgpu_fixed_texture_type(draw, stage);

	if (type == _xgpu_fixed_texture_back_buffer)
	{
		struct xgpu_texture_description description;

		xgpu_texture_describe(resource->Format, resource->Size, &description);
		return back_buffer_copy(&draw->target, &description, texture);
	}
	if (type == _xgpu_fixed_texture_2d)
	{
		const D3DCOLOR *palette = draw->palettes[stage] && draw->palettes[stage]->Data ?
			(const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(draw->palettes[stage]->Data) : NULL;
		struct texture_entry *entry = texture_entry_get((const DWORD *)resource, palette);

		if (entry && entry->name)
		{
			memset(texture, 0, sizeof(*texture));
			texture->type = _xgpu_fixed_texture_2d;
			texture->name = entry->name;
			texture->mipmapped = entry->mipmapped;
			texture->base = entry->base;
			texture->scale[0] = entry->scale[0];
			texture->scale[1] = entry->scale[1];
			texture->sampler = &entry->sampler;
			texture->entry = entry;
			return TRUE;
		}
	}
	xgpu_fixed_white_texture(texture);
	texture->type = type;
	return TRUE;
}

/* ---------- derived textures */

BOOL xgpu_fixed_bake_get(struct xgpu_fixed_texture *texture, const D3DCOLOR *palette,
	const struct nv2a_ff_plan *plan, int bake_index, const struct nv2a_ff_constants *constants)
{
	struct texture_entry *entry = texture->entry;
	struct bake_function function;
	struct bake *bake, *oldest = NULL;
	unsigned long fingerprint, count = 0;

	if (!entry)
		return FALSE;
	function.plan = plan;
	function.bake = bake_index;
	function.constants = constants;
	fingerprint = bake_fingerprint(&function);
	for (bake = entry->bakes; bake; bake = bake->next)
	{
		if (bake->fingerprint == fingerprint)
			break;
		count++;
		if (!oldest || bake->last_used_frame < oldest->last_used_frame)
			oldest = bake;
	}
	/* One is to be made: not when this frame has made its share (the draw
	takes the texture as it is, and the next frame makes it), and not in
	the place of one that is in use, which would only be made again for its
	own draw a moment later, every frame. */
	if ((!bake || bake->generation != entry->generation) && bakes_this_frame >= BAKES_A_FRAME)
		return FALSE;
	if (!bake && count >= MAXIMUM_BAKES_A_TEXTURE)
	{
		if (oldest->last_used_frame + 1 >= texture_frame)
		{
			static BOOL reported;

			if (!reported)
			{
				reported = TRUE;
				platform_log("gl1: a texture is drawn in more than %d derived forms at once; the draws beyond them "
					"take it as it is", MAXIMUM_BAKES_A_TEXTURE);
			}
			return FALSE;
		}
		/* a function that keeps changing (a colour that fades) reuses the
		stalest one */
		bake = oldest;
		bake->fingerprint = fingerprint;
		bake->generation = 0;
	}
	if (!bake)
	{
		bake = calloc(1, sizeof(*bake));
		if (!bake)
			return FALSE;
		bake->fingerprint = fingerprint;
		bake->next = entry->bakes;
		entry->bakes = bake;
	}
	if (bake->generation != entry->generation)
	{
		bakes_this_frame++;
		if (!bake->name)
		{
			bake->name = texture_new();
		}
		else
		{
			glBindTexture(GL_TEXTURE_2D, bake->name);
			xgpu_fixed_state_invalidate();
		}
		bake->mipmapped = upload(&entry->description, (const unsigned char *)entry->address, palette, &function, &bake->base);
		bake->sampler.valid = FALSE;
		bake->generation = entry->generation;
		xgpu_fixed_texture_bakes++;
	}
	bake->last_used_frame = texture_frame;
	texture->name = bake->name;
	texture->mipmapped = bake->mipmapped;
	texture->base = bake->base;
	texture->sampler = &bake->sampler;
	return TRUE;
}

/* ---------- frames */

static void bakes_drop(struct texture_entry *entry, BOOL all)
{
	struct bake **link = &entry->bakes;

	while (*link)
	{
		struct bake *bake = *link;

		if (all || texture_frame - bake->last_used_frame > BAKE_IDLE_FRAMES)
		{
			*link = bake->next;
			if (bake->name)
				glDeleteTextures(1, &bake->name);
			free(bake);
		}
		else
		{
			link = &bake->next;
		}
	}
}

void xgpu_fixed_textures_frame_begin(void)
{
	unsigned long index;

	texture_frame++;
	bakes_this_frame = 0;
	back_buffer.copies_this_frame = 0;
	if (texture_frame % 600)
		return;
	/* drop textures that have not been used for a while */
	for (index = 0; index < TEXTURE_BUCKET_COUNT; index++)
	{
		struct texture_entry **link = &texture_buckets[index];

		while (*link)
		{
			struct texture_entry *entry = *link;

			if (texture_frame - entry->last_used_frame > TEXTURE_IDLE_FRAMES)
			{
				*link = entry->next;
				bakes_drop(entry, TRUE);
				if (entry->name)
					glDeleteTextures(1, &entry->name);
				texture_drop_serial++;
				free(entry);
			}
			else
			{
				bakes_drop(entry, FALSE);
				link = &entry->next;
			}
		}
	}
	xgpu_fixed_state_invalidate();
}

#endif
