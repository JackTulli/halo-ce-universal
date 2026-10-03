/*
XGPU_FIXED.H

The light renderer (display.renderer "gl1"): the Xbox Direct3D device drawn
with OpenGL 1.1's fixed function, for cards without shaders (xgpu_fixed.c,
xgpu_fixed_textures.c).

It is a device of its own behind d3d8_gl.c's Direct3D entry points. That
file keeps the Direct3D state as it does for the other renderers and, where
an entry point would reach OpenGL, describes what is to be done with the
structures below and calls in here; nothing of its GLSL renderers runs.

What is drawn and how:
- The window's own back buffer is the only render target. A small
  off-screen target (the motion sensor's 64x64) is drawn in a corner of
  it and copied to a texture, when the window has alpha of its own; what
  the game draws into the larger ones (and into the small one in a window
  without alpha) is left out, and so is every draw that would sample one
  of them (shadows, glows, screen effects); a texture that is the back
  buffer itself samples a copy of it.
- A vertex program whose position is four constant rows times one input
  (every one of the game's that is not skinned) goes through OpenGL's own
  transform, with the game's streams as the arrays; the rest of each
  program, and the skinned programs whole, run on the processor
  (nv2a_vsh_cpu.c), and OpenGL is given clip-space positions, texture
  coordinates and the passes' colours.
- The pixel state (texture modes, register combiners) becomes texture
  environments and passes through nv2a_combiner_ff.c's plans.
- Textures are converted by xbox_textures_convert.c to what OpenGL 1.1
  takes.
*/

#ifndef __HALO_LINUX_XGPU_FIXED_H
#define __HALO_LINUX_XGPU_FIXED_H

#include "xgpu.h"

#if !defined(HALO_ANDROID) && !defined(HALO_XBOX)

/* ---------- what d3d8_gl.c tells the light renderer */

/* the render targets of a draw, a clear or the presentation */
struct xgpu_fixed_target
{
	/* the colour target is the device's back buffer, which is the window */
	BOOL window;
	/* the depth target is the device's own depth buffer: the window's */
	BOOL depth;
	/* the targets' physical addresses and the colour target's size, for
	the list of off-screen targets and the log */
	unsigned long color_data, depth_data;
	unsigned long width, height;
	/* the device's back buffer, which a texture may sample */
	unsigned long back_buffer_data;
	/* where in the window the picture is (pixels from the window's left
	and bottom), its size in pixels, and the pixels per unit of the back
	buffer's width and height */
	long window_x, window_y;
	unsigned long gl_width, gl_height;
	float scale[2];
};

/* one input register of the vertex program that a stream feeds */
struct xgpu_fixed_element
{
	unsigned char reg;
	/* D3DVSDT_* */
	unsigned char type;
	/* the register's bytes in vertex 0 of the stream */
	const void *data;
	unsigned long stride;
};

struct xgpu_fixed_draw
{
	struct xgpu_fixed_target target;

	/* the vertex program: its instruction words (after the header), and
	where the device keeps what the light renderer makes of it (a null
	pointer until the program is first drawn with) */
	const DWORD *instructions;
	unsigned long instruction_count;
	unsigned long program_id;
	void **program;
	/* c[-96..95] */
	const float (*constants)[4];
	/* the device's viewport, and what it gives the vertex shaders of the
	other renderers as viewport_scale, viewport_offset and screen_offset */
	D3DVIEWPORT8 viewport;
	float viewport_scale[4];
	float viewport_offset[4];
	float screen_offset;

	D3DPRIMITIVETYPE primitive;
	unsigned long element_count;
	struct xgpu_fixed_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
	/* the current value of each input register, which those without a
	stream read */
	const float (*attributes)[4];
	/* without indices: vertices first to first + count - 1 of the streams.
	With indices: count of them, each the number of a vertex. */
	unsigned long first, count;
	const WORD *indices;

	/* the stages' textures and palettes (D3DTSS_MAXSTAGES of each) */
	D3DBaseTexture *const *textures;
	D3DPalette *const *palettes;

	unsigned long frame;
	/* debug.gpu_trace_frame names this frame */
	BOOL trace;
};

/* Reads what the context has (the version, the extensions that
debug.gl1_extensions allows, the limits, the window's bits) and says so in
the log. FALSE if the context cannot be drawn with. */
BOOL xgpu_fixed_initialize(void);

/* the depth and stencil bits of the window, for d3d8_gl.c's
xgpu_capabilities */
void xgpu_fixed_window_bits(int *depth, int *stencil);

void xgpu_fixed_draw(const struct xgpu_fixed_draw *draw);

void xgpu_fixed_clear(const struct xgpu_fixed_target *target, long screen_offset, DWORD count,
	const D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil);

/* Visibility tests. The draws between begin and end are measured as they
are transformed; the count is made at the next presentation, from the
depth buffer as the frame left it, and is the test's answer from then on. */
void xgpu_fixed_visibility_begin(void);
void xgpu_fixed_visibility_end(DWORD slot);
/* the latest count of the slot's tests, in the game's pixels */
UINT xgpu_fixed_visibility_result(DWORD slot);

/* before the swap: counts the frame's visibility tests */
void xgpu_fixed_present(const struct xgpu_fixed_target *target);
/* after the swap: the back buffer's contents are gone */
void xgpu_fixed_frame_begin(void);

/* ---------- between xgpu_fixed.c and xgpu_fixed_textures.c */

/* what the context has and the light renderer uses */
struct xgpu_fixed_context
{
	/* major * 100 + minor * 10 */
	int version;
	int maximum_texture_size;
	int red_bits, green_bits, blue_bits, alpha_bits, depth_bits, stencil_bits;

	/* ARB_multitexture and its units (1 without) */
	BOOL multitexture;
	int units;
	/* EXT_texture_env_combine or ARB's; ARB's (or OpenGL 1.3's) has
	SUBTRACT and every operand */
	BOOL combine, combine_arb;
	BOOL environment_add;
	BOOL dot3;
	/* GL_BGRA data */
	BOOL bgra;
	/* GL_UNSIGNED_SHORT_4_4_4_4 and _5_5_5_1; _5_6_5 is OpenGL 1.2's alone */
	BOOL packed_pixels, packed_565;
	BOOL s3tc;
	BOOL compiled_vertex_array;
	BOOL blend_color, blend_minmax, blend_subtract;
	BOOL edge_clamp;
	/* ARB_texture_mirrored_repeat; without it a mirrored texture repeats */
	BOOL mirrored_repeat;
	/* OpenGL 1.2: GL_TEXTURE_MAX_LEVEL, GL_TEXTURE_MIN_LOD */
	BOOL texture_lod;
	/* SGIS_generate_mipmap (reported; the mip levels are made here) */
	BOOL generate_mipmap;

	/* display.gl1_texture_bits is 32: textures keep 8 bits a channel */
	BOOL textures_32bit;
	/* debug.gl1_extensions has "simple": no combiner plans */
	BOOL simple;
};

extern struct xgpu_fixed_context xgpu_fixed_context;

/* xbox_textures.c: the texel kind of a format, in xbox_textures_convert.h's
numbering */
int xgpu_texture_format_kind(DWORD format);

/* the light renderer's GL state cache (xgpu_fixed.c): code that binds a
texture or changes its parameters behind it calls this afterwards */
void xgpu_fixed_state_invalidate(void);

/* what a stage's texture is to the light renderer */
enum
{
	_xgpu_fixed_texture_none,
	/* a 2D texture that can be bound */
	_xgpu_fixed_texture_2d,
	/* a cube or volume texture: never sampled; a unit that wants one gets
	the white texture */
	_xgpu_fixed_texture_cube,
	_xgpu_fixed_texture_volume,
	/* the back buffer: a copy of it */
	_xgpu_fixed_texture_back_buffer,
	/* one of the game's off-screen targets, which hold nothing */
	_xgpu_fixed_texture_off_screen
};

/* The base internal format OpenGL keeps a texture's texels in. The Xbox
samples every texture as red, green, blue, alpha (a luminance texture's
alpha is 1, an intensity texture's is its intensity), and so do the plans;
OpenGL 1.1's own environments read a luminance or intensity texture
otherwise (xgpu_fixed.c, unit_environment). */
enum
{
	_xgpu_fixed_base_rgba,
	_xgpu_fixed_base_rgb,
	_xgpu_fixed_base_luminance,
	_xgpu_fixed_base_luminance_alpha,
	_xgpu_fixed_base_intensity
};

struct xgpu_fixed_texture
{
	int type;
	GLuint name;
	/* the texture has a complete chain of mip levels */
	BOOL mipmapped;
	/* _xgpu_fixed_base_* */
	int base;
	/* what the texture matrix does to s and t: scale, then offset (times
	q). 1, 1, 0, 0 for a power-of-two texture addressed from 0 to 1 */
	float scale[2];
	float offset[2];
	/* for xgpu_fixed_texture_parameters */
	void *sampler;
	/* the cache's entry (2D textures), for xgpu_fixed_bake_get */
	void *entry;
};

/* what the texture bound to a stage is, without reading its texels */
int xgpu_fixed_texture_type(const struct xgpu_fixed_draw *draw, int stage);
/* The texture of a stage for a unit to bind: a 2D texture uploaded or
refreshed from guest memory as needed, the copy of the back buffer made
again if the back buffer has been drawn into since, the white texture for
everything else. FALSE when a copy of the back buffer cannot be made
(larger than the context's textures). An upload binds textures on the
active unit and calls xgpu_fixed_state_invalidate. */
BOOL xgpu_fixed_texture_get(const struct xgpu_fixed_draw *draw, int stage, struct xgpu_fixed_texture *texture);
/* a 1x1 white texture */
void xgpu_fixed_white_texture(struct xgpu_fixed_texture *texture);
/* the back buffer has been drawn into: its copy is stale */
void xgpu_fixed_back_buffer_drawn(void);
/* an address has been a colour target that is not the back buffer; TRUE
the first time */
BOOL xgpu_fixed_off_screen_target(unsigned long data);
/* a draw that samples the off-screen target at an address has been left
out; TRUE the first time */
BOOL xgpu_fixed_off_screen_sampled(unsigned long data);

struct nv2a_ff_plan;
struct nv2a_ff_constants;
/* The derived texture a plan's unit asks for in place of its stage's own
(nv2a_combiner_ff.h): made from the stage's texels and kept until the
texture or the function changes. texture is the stage's, and becomes the
derived one; FALSE (and texture unchanged) if it cannot be made. */
BOOL xgpu_fixed_bake_get(struct xgpu_fixed_texture *texture, const D3DCOLOR *palette,
	const struct nv2a_ff_plan *plan, int bake, const struct nv2a_ff_constants *constants);

/* The stage's filters and address modes (D3D__TextureState) as parameters
of the texture, which is bound to the active unit; nothing is set that the
texture already has. */
void xgpu_fixed_texture_parameters(const struct xgpu_fixed_texture *texture, int stage);

void xgpu_fixed_textures_frame_begin(void);

/* counted for debug.gpu_stats */
extern unsigned long xgpu_fixed_texture_uploads, xgpu_fixed_texture_bakes, xgpu_fixed_back_buffer_copies;

#endif

#endif
