/*
XGPU.H

Internals shared by the OpenGL implementation of the Xbox Direct3D API:
the NV2A shader translators (nv2a_vsh.c, nv2a_psh.c), texture decoding
(xbox_textures.c), guest memory write tracking (memory_watch.c) and the
device itself (d3d8_gl.c).
*/

#ifndef __HALO_LINUX_XGPU_H
#define __HALO_LINUX_XGPU_H

#include "platform.h"
#include "gl.h"

#include <string.h>

#ifdef HALO_ANDROID
/* OpenGL ES features that are optional (d3d8_gl.c gl_initialize) */
struct xgpu_capabilities
{
	BOOL copy_image;
	BOOL border_clamp;
	BOOL anisotropy;
	BOOL s3tc;
	/* ES 3.2: glDrawElementsBaseVertex */
	BOOL base_vertex;
	/* ES 3.1 with fragment atomic counters: exact visibility test counts */
	BOOL atomic_counters;
	/* "300 es" or "310 es" */
	const char *shading_language;
};

extern struct xgpu_capabilities xgpu_capabilities;

/* port/android/guest/runtime/guest_host.h */
int host_gl_has_extension(const char *name);
unsigned int host_gl_read_buffer_word(unsigned int buffer, unsigned int offset);
void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data);
void host_gl_fence_frame(unsigned int slot);
void host_gl_wait_frame(unsigned int slot);
#elif !defined(HALO_XBOX)
/* ---------- the desktop's renderers (display.renderer, port_config.c)

gl4 is the renderer as it was written, on an OpenGL 4.5 core context, where
everything below is present. gl2 runs the same code on OpenGL 2.0 with the
extensions of its time (GLSL 1.10; Windows 98/ME, old drivers, virtual
machines): each feature it lacks has a replacement, described with the
feature. gl1 is the light renderer for cards without shaders, OpenGL 1.1's
fixed function. d3d8_gl.c's gl_initialize chooses and fills this in. */

enum
{
	_xgpu_renderer_none,
	_xgpu_renderer_gl4,
	_xgpu_renderer_gl2,
	_xgpu_renderer_gl1,
};

struct xgpu_capabilities
{
	int renderer;
	/* the context's version, and its shading language's times 100 */
	int version_major, version_minor;
	int shading_language_version;

	/* ----- what the shader translators write differently */

	/* "#version 450 core"; without it "#version 110": attribute and
	varying, gl_FragColor, texture2D and its kin, no integer operators */
	BOOL modern_shading_language;
	/* glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE). Without it the vertex
	shader maps depth to GL's -1..1 and flips y by the clip_flip uniform
	(render targets keep their rows from the top; the window's own back
	buffer is not flipped), and the front face is reversed to match */
	BOOL clip_control;
	/* NORMPACKED3 elements as one 32-bit integer attribute. Without it,
	as four unsigned bytes that the shader puts together again in floats */
	BOOL integer_attributes;
	/* D3DCOLOR elements as GL_BGRA attributes. Without it the vertex
	shader swaps red and blue */
	BOOL bgra_attributes;
	/* sampler objects. Without them the sampler state is set on each
	texture as it is bound (a copy of it is kept per texture), and the
	pixel shader applies the LOD bias */
	BOOL sampler_objects;
	/* gl_FragDepth for the depth-replacing dot product texture mode,
	which the translator otherwise leaves out (as it always did) */
	BOOL depth_replace;

	/* ----- what the device does differently */

	/* vertex array objects, which a core context needs */
	BOOL vertex_array_objects;
	/* glDebugMessageCallback. Without it debug.gl_debug polls glGetError */
	BOOL debug_output;
	/* glDrawElementsBaseVertex. Without it mirrored streams are pointed
	at their vertex 0 and streamed draws get their indices rebased */
	BOOL base_vertex;
	/* GL_COPY_WRITE_BUFFER as the mirror's upload target. Without it,
	GL_ARRAY_BUFFER */
	BOOL copy_buffer;
	/* visibility test counts written by the GPU into a mapped query
	buffer. Without it the queries are polled, a test that is not done yet
	answering with its slot's last count */
	BOOL query_buffer;
	/* framebuffer objects of any kind; none leaves only the window */
	BOOL framebuffer_objects;
	/* OpenGL 3.0's: depth and stencil in one texture of any size, at
	GL_DEPTH_STENCIL_ATTACHMENT. Without it (EXT_framebuffer_object) depth
	surfaces are renderbuffers of the colour target's size */
	BOOL depth_textures;
	/* a depth format with stencil for framebuffer objects */
	BOOL packed_depth_stencil;
	/* glBlitFramebuffer for presentation. Without it, a textured quad */
	BOOL framebuffer_blit;
	/* glCopyImageSubData. Without it glCopyTexSubImage2D from a
	framebuffer */
	BOOL copy_image;
	/* S3TC textures, and S3TC volume textures; the others are decoded on
	the CPU */
	BOOL s3tc;
	BOOL s3tc_3d;
	BOOL anisotropy;
	/* 32 MB of vertices and 8 MB of indices streamed a frame; otherwise
	4 MB and 1 MB, the buffers orphaned as they fill */
	BOOL large_streams;

	/* the frame is drawn into the window's own back buffer and presented
	by the swap alone: the mode without framebuffer objects, or without a
	depth format with stencil for them. A texture that samples the back
	buffer gets a copy of it (glCopyTexSubImage2D), rows from the bottom */
	BOOL window_back_buffer;
	/* the window's own depth and stencil bits (that mode's) */
	int window_depth_bits, window_stencil_bits;
};

extern struct xgpu_capabilities xgpu_capabilities;

/* what a renderer has before the context is asked: everything for gl4,
OpenGL 2.0 alone for gl2 */
static inline void xgpu_capabilities_set_renderer(struct xgpu_capabilities *capabilities, int renderer)
{
	BOOL modern = renderer == _xgpu_renderer_gl4;

	memset(capabilities, 0, sizeof(*capabilities));
	capabilities->renderer = renderer;
	capabilities->modern_shading_language = modern;
	capabilities->clip_control = modern;
	capabilities->integer_attributes = modern;
	capabilities->bgra_attributes = modern;
	capabilities->sampler_objects = modern;
	/* (the 4.5 renderer never wrote depth there, and still does not) */
	capabilities->depth_replace = renderer == _xgpu_renderer_gl2;
	capabilities->vertex_array_objects = modern;
	capabilities->debug_output = modern;
	capabilities->base_vertex = modern;
	capabilities->copy_buffer = modern;
	capabilities->query_buffer = modern;
	capabilities->framebuffer_objects = modern;
	capabilities->depth_textures = modern;
	capabilities->packed_depth_stencil = modern;
	capabilities->framebuffer_blit = modern;
	capabilities->copy_image = modern;
	capabilities->s3tc = modern;
	capabilities->s3tc_3d = modern;
	capabilities->anisotropy = modern;
	capabilities->large_streams = modern;
}
#endif

/* ---------- GL state

The device caches the GL state it sets for draws (d3d8_gl.c); code that
changes GL state behind it (binding a texture to upload it, deleting one)
must call this afterwards. */

void xgpu_gl_state_invalidate(void);
/* a GL texture is being deleted: the device forgets the sampler state it
had set on it (the renderer without sampler objects keeps a copy) */
void xgpu_gl_texture_deleted(GLuint texture);

/* ---------- generated source text */

struct xgpu_text
{
	char *buffer;
	unsigned long length;
	unsigned long capacity;
};

void xgpu_text_append(struct xgpu_text *text, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* ---------- vertex shaders */

#define XGPU_VERTEX_ATTRIBUTE_COUNT 16
#define XGPU_VERTEX_CONSTANT_COUNT 192
/* D3D constant register -96 is hardware register 0 */
#define XGPU_VERTEX_CONSTANT_BIAS 96

/* GLSL for an NV2A vertex program (the instruction words after the program
header). Attributes whose bit is set in packed_attribute_mask are fed as
NORMPACKED3 32-bit integers (or as their four bytes, where there are no
integer attributes) and unpacked in the shader; those in
color_attribute_mask are D3DCOLORs fed in memory order, blue first, which
the shader puts in order (the desktop without GL_BGRA attributes; 0
otherwise). Returns a malloc'd string. */
char *nv2a_vertex_shader_to_glsl(const DWORD *instructions, unsigned long instruction_count,
	unsigned long packed_attribute_mask, unsigned long color_attribute_mask);

/* ---------- pixel shaders */

enum
{
	_xgpu_sampler_none = 0,
	_xgpu_sampler_2d,
	_xgpu_sampler_3d,
	_xgpu_sampler_cube,
};

/* everything a translated pixel shader depends on; the GLSL program cache
is keyed by these bytes */
struct nv2a_pixel_shader_key
{
	DWORD combiner_state[D3DRS_PS_MAX];
	/* D3DRS_PSTEXTUREMODES lies past D3DRS_PS_MAX */
	DWORD texture_modes;
	unsigned char sampler_type[4];
	unsigned char alpha_kill[4];
	/* D3DTSS_COLORSIGN: channels (bit 0 alpha ... bit 3 blue, as
	D3DTSIGN_*) that hold signed data in an unsigned texture format */
	unsigned char color_sign[4];
	/* D3DCMP_* function for the alpha test, or 0 when disabled */
	unsigned long alpha_test_function;
	unsigned char fog_enable;
	unsigned char fog_table_mode;
	/* inside a visibility test: count the samples that pass (Android) */
	unsigned char count_samples;
	unsigned char pad;
};

char *nv2a_pixel_shader_to_glsl(const struct nv2a_pixel_shader_key *key);

#ifdef HALO_ANDROID
/* ES samplers have no LOD bias of their own */
#define XGPU_PIXEL_UNIFORMS_ES "uniform vec4 texture_lod_bias;\n"
#else
#define XGPU_PIXEL_UNIFORMS_ES ""
#endif

/* what the desktop's pixel shaders declare besides, by the context's
capabilities (nv2a_psh.c): the LOD bias where samplers have none, the
offset that turns a copy of the window's back buffer the right way up, and
the depth buffer's range for the depth-replacing texture mode */
#define XGPU_PIXEL_UNIFORM_LOD_BIAS "uniform vec4 texture_lod_bias;\n"
#define XGPU_PIXEL_UNIFORM_TEXTURE_OFFSET "uniform vec4 texture_offset[4];\n"
#define XGPU_PIXEL_UNIFORM_DEPTH_SCALE "uniform float depth_scale;\n"

/* the combiner registers that live in uniforms rather than in the program:
C0/C1 of each stage and the final combiner, and texture constants */
#define XGPU_PIXEL_UNIFORMS \
	"uniform vec4 ps_c0[8];\n" \
	"uniform vec4 ps_c1[8];\n" \
	"uniform vec4 ps_final_c0;\n" \
	"uniform vec4 ps_final_c1;\n" \
	"uniform vec4 fog_color;\n" \
	"uniform vec4 fog_parameters;\n" \
	"uniform float alpha_reference;\n" \
	"uniform vec4 bump_matrix[4];\n" \
	"uniform vec4 bump_luminance[4];\n" \
	"uniform vec4 texture_scale[4];\n" \
	XGPU_PIXEL_UNIFORMS_ES

/* ---------- textures */

struct xgpu_texture_description
{
	DWORD format;       /* D3DFMT_* */
	unsigned long width, height, depth, levels;
	BOOL cube_map;
	BOOL linear;        /* not swizzled; addressed with texel coordinates */
	BOOL compressed;
	unsigned long pitch; /* linear textures */
};

void xgpu_texture_describe(DWORD format_word, DWORD size_word, struct xgpu_texture_description *description);
/* bytes of one face, mip levels included (cube faces are padded) */
unsigned long xgpu_texture_face_size(const struct xgpu_texture_description *description);
unsigned long xgpu_texture_level_offset(const struct xgpu_texture_description *description, unsigned long level);
unsigned long xgpu_texture_level_pitch(const struct xgpu_texture_description *description, unsigned long level);

/* the GL texture for an Xbox texture header, uploading or refreshing it
from guest memory as needed; *target receives GL_TEXTURE_2D etc. */
GLuint xgpu_texture_get(const DWORD *resource, const D3DCOLOR *palette, GLenum *target,
	struct xgpu_texture_description *description);
void xgpu_texture_cache_begin_frame(void);

/* ---------- render targets */

struct xgpu_render_target
{
	unsigned long data;  /* physical address */
	unsigned long width, height;
	BOOL depth;
	GLuint texture;
	/* pixels per unit of width and height: more than 1 for the screen's
	targets when the game draws at the display's resolution (d3d8_gl.c) */
	float scale[2];
	unsigned long gl_width, gl_height;
	/* the window's own back buffer (xgpu_capabilities.window_back_buffer,
	desktop): texture is then the copy that samples of it read, rows from
	the bottom, and the picture starts at this pixel of the window */
	BOOL window;
	long window_x, window_y;
};

/* the GL texture holding a render target with this physical address, or 0 */
struct xgpu_render_target *xgpu_render_target_find(unsigned long data);

#endif
