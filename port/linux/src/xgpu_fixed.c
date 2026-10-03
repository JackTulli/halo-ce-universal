/*
XGPU_FIXED.C

The light renderer (xgpu_fixed.h): the Xbox Direct3D device drawn with
OpenGL 1.1's fixed function.

A draw goes through these steps:
- The pixel state - texture modes, register combiners, alpha test, blend -
  is looked up in a cache of plans (nv2a_combiner_ff.h): one or more passes
  of texture units with their environments, a primary colour and a blend. A
  state the compiler has no plan for is drawn as its first texture times
  the vertex colour, and its key is logged once.
- A program whose position is four constant rows times one input register
  is lifted: GL_PROJECTION is those rows with the screen-space conversion,
  and the vertex array is the game's own stream; texture coordinates that
  are affine in one input go through GL_TEXTURE the same way (the lifted
  path, below). What is left of the program, and the whole of a skinned
  one, runs on the processor (nv2a_vsh_cpu.h) over the range of vertices
  the draw uses, in one run that writes every array the passes read: the
  clip-space position (GL_PROJECTION is then the identity, so OpenGL only
  clips, divides and maps to the viewport), the texture coordinates of the
  stages the plan samples, and the passes' primary colours. The path is
  chosen by the program, so two draws of the same triangles give the same
  depth and the game's (and the plans') passes with the depth function
  EQUAL find their pixels.
- Each pass sets its units, alpha test and blend and draws the arrays,
  with the game's own indices where it has them.

Conventions: Direct3D's clip space has depth from 0 to w and rows from the
top of the screen; the evaluator gives OpenGL's (depth from -w to w), and
since clip space's y points up in both, the window's back buffer is drawn
the right way up as it comes. Viewports, scissors and read-backs count rows
from the bottom of the window. Triangles keep their winding.
*/

#include "xgpu_fixed.h"

#if !defined(HALO_ANDROID) && !defined(HALO_XBOX)

#include "xgpu_fixed_gl.h"
#include "nv2a_vsh_cpu.h"
#include "nv2a_combiner_ff.h"
#include "port_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xmmintrin.h>

#ifdef HALO_WIN9X
/* Windows 98/ME: WGL's, with opengl32.dll's own exports for OpenGL 1.1
(port/win9x/platform/w9x_gl.c) */
#include "../../win9x/platform/w9x_backend.h"
#define SDL_GL_GetProcAddress(name) w9x_gl_proc_address(name)
#else
#include <SDL3/SDL.h>
#endif

/* ---------- entry points (xgpu_fixed_gl.h) */

#define GL_DEFINE_FIXED_FUNCTION(name) __typeof__(halo_##name) halo_##name;
#define GL_DEFINE_FIXED_OPTIONAL_FUNCTION(name, other) __typeof__(halo_##name) halo_##name;
GL_FUNCTIONS_FIXED(GL_DEFINE_FIXED_FUNCTION)
GL_FUNCTIONS_FIXED_OPTIONAL(GL_DEFINE_FIXED_OPTIONAL_FUNCTION)
#undef GL_DEFINE_FIXED_FUNCTION
#undef GL_DEFINE_FIXED_OPTIONAL_FUNCTION

/* FALSE (and logs) if one of OpenGL 1.1's is missing */
static BOOL fixed_functions_load(void)
{
	BOOL success = TRUE;

#define GL_LOAD_FUNCTION(name) \
	halo_##name = (__typeof__(halo_##name))SDL_GL_GetProcAddress(#name); \
	if (!halo_##name) \
	{ \
		platform_log("OpenGL function %s is unavailable", #name); \
		success = FALSE; \
	}
	GL_FUNCTIONS_FIXED(GL_LOAD_FUNCTION)
#undef GL_LOAD_FUNCTION
#define GL_LOAD_FUNCTION(name, other) \
	halo_##name = (__typeof__(halo_##name))SDL_GL_GetProcAddress(#name); \
	if (!halo_##name && other) \
		halo_##name = (__typeof__(halo_##name))SDL_GL_GetProcAddress(other ? other : "");
	GL_FUNCTIONS_FIXED_OPTIONAL(GL_LOAD_FUNCTION)
#undef GL_LOAD_FUNCTION
	/* gl.h's, which gl_functions_load looked up by their OpenGL 1.3 and 1.4
	names alone */
	if (!halo_glActiveTexture)
		halo_glActiveTexture = (__typeof__(halo_glActiveTexture))SDL_GL_GetProcAddress("glActiveTextureARB");
	if (!halo_glBlendEquation)
		halo_glBlendEquation = (__typeof__(halo_glBlendEquation))SDL_GL_GetProcAddress("glBlendEquationEXT");
	if (!halo_glBlendColor)
		halo_glBlendColor = (__typeof__(halo_glBlendColor))SDL_GL_GetProcAddress("glBlendColorEXT");
	if (!halo_glCompressedTexImage2D)
		halo_glCompressedTexImage2D =
			(__typeof__(halo_glCompressedTexImage2D))SDL_GL_GetProcAddress("glCompressedTexImage2DARB");
	return success;
}

/* ---------- what the context has */

struct xgpu_fixed_context xgpu_fixed_context;

/* the same, as the plan compiler wants to know it */
static struct nv2a_ff_capabilities plan_capabilities;

/* whether the driver lists the extension */
static BOOL extension_listed(const char *name)
{
#ifdef HALO_WIN9X
	return w9x_gl_extension(name) != 0;
#else
	const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
	size_t length = strlen(name);
	const char *found;

	for (found = extensions; found && (found = strstr(found, name)) != NULL; found += length)
	{
		if ((found == extensions || found[-1] == ' ') && (found[length] == ' ' || !found[length]))
			return TRUE;
	}
	return FALSE;
#endif
}

/* whether debug.gl1_extensions has the word (an extension's name with or
without its "GL_") */
static BOOL setting_lists(const char *name)
{
	const char *list = config_string("debug.gl1_extensions");
	size_t length;

	if (!strncmp(name, "GL_", 3))
		name += 3;
	length = strlen(name);
	while (*list)
	{
		const char *end;

		while (*list == ',' || *list == ' ')
			list++;
		for (end = list; *end && *end != ',' && *end != ' '; end++)
			;
		if (!strncmp(list, "GL_", 3))
			list += 3;
		if ((size_t)(end - list) == length && !strncmp(list, name, length))
			return TRUE;
		list = end;
	}
	return FALSE;
}

/* debug.gl1_extensions: "all" allows every extension, "none" none (plain
OpenGL 1.1), and otherwise the ones it names */
static BOOL extension_allowed(const char *name)
{
	if (setting_lists("none"))
		return FALSE;
	return setting_lists("all") || setting_lists(name);
}

static void text_add(char *text, size_t size, const char *word)
{
	if (strlen(text) + strlen(word) + 2 < size)
	{
		if (*text)
			strcat(text, " ");
		strcat(text, word);
	}
}

/* Whether the context has a feature and the setting allows it. The feature
is an extension, which may have a second name, and part of OpenGL itself
from core_version on (0: never). What is found, and of it what is used,
is noted for the log under the first name. */
static BOOL context_feature(const char *name, const char *second_name, int core_version, BOOL usable,
	char *found, char *used, size_t size)
{
	BOOL present = extension_listed(name) || (second_name && extension_listed(second_name)) ||
		(core_version && xgpu_fixed_context.version >= core_version);
	BOOL allowed = extension_allowed(name) || (second_name && extension_allowed(second_name));

	if (present)
		text_add(found, size, name + 3);
	if (present && allowed && usable)
		text_add(used, size, name + 3);
	return present && allowed && usable;
}

/* "1.2.1", "1.5 Mesa 6.2": major * 100 + minor * 10 */
static int version_number(const char *text)
{
	int major = 0, minor = 0;

	if (!text)
		return 0;
	while (*text >= '0' && *text <= '9')
		major = major * 10 + (*text++ - '0');
	if (*text == '.' && text[1] >= '0' && text[1] <= '9')
		minor = text[1] - '0';
	return major * 100 + minor * 10;
}

void xgpu_fixed_window_bits(int *depth, int *stencil)
{
	*depth = xgpu_fixed_context.depth_bits;
	*stencil = xgpu_fixed_context.stencil_bits;
}

BOOL xgpu_fixed_initialize(void)
{
	struct xgpu_fixed_context *context = &xgpu_fixed_context;
	const char *version = (const char *)glGetString(GL_VERSION);
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	char found[512] = "", used[512] = "";
	GLint value = 0;
	long texture_bits;

	memset(context, 0, sizeof(*context));
	if (!fixed_functions_load())
	{
		platform_log("gl1: the context lacks OpenGL 1.1's fixed function; nothing is drawn");
		return FALSE;
	}
	context->version = version_number(version);
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
	context->maximum_texture_size = value >= 64 ? (int)value : 64;
	glGetIntegerv(GL_RED_BITS, &value); context->red_bits = (int)value;
	glGetIntegerv(GL_GREEN_BITS, &value); context->green_bits = (int)value;
	glGetIntegerv(GL_BLUE_BITS, &value); context->blue_bits = (int)value;
	glGetIntegerv(GL_ALPHA_BITS, &value); context->alpha_bits = (int)value;
	glGetIntegerv(GL_DEPTH_BITS, &value); context->depth_bits = (int)value;
	glGetIntegerv(GL_STENCIL_BITS, &value); context->stencil_bits = (int)value;

	context->multitexture = context_feature("GL_ARB_multitexture", NULL, 130,
		halo_glActiveTexture && halo_glClientActiveTextureARB, found, used, sizeof(found));
	context->units = 1;
	if (context->multitexture)
	{
		value = 1;
		glGetIntegerv(GL_MAX_TEXTURE_UNITS, &value);
		context->units = value < 1 ? 1 : value > NV2A_FF_MAXIMUM_UNITS ? NV2A_FF_MAXIMUM_UNITS : (int)value;
		context->multitexture = context->units > 1;
	}
	context->combine = context_feature("GL_EXT_texture_env_combine", NULL, 0, TRUE, found, used, sizeof(found));
	context->combine_arb = context_feature("GL_ARB_texture_env_combine", NULL, 130, TRUE, found, used, sizeof(found));
	context->combine = context->combine || context->combine_arb;
	context->environment_add = context_feature("GL_EXT_texture_env_add", "GL_ARB_texture_env_add", 130, TRUE,
		found, used, sizeof(found));
	context->dot3 = context_feature("GL_ARB_texture_env_dot3", "GL_EXT_texture_env_dot3", 130, context->combine,
		found, used, sizeof(found));
	context->bgra = context_feature("GL_EXT_bgra", NULL, 120, TRUE, found, used, sizeof(found));
	context->packed_pixels = context_feature("GL_EXT_packed_pixels", NULL, 120, TRUE, found, used, sizeof(found));
	/* (the extension has no 5:6:5 type; OpenGL 1.2 has) */
	context->packed_565 = context->packed_pixels && context->version >= 120;
	context->s3tc = context_feature("GL_EXT_texture_compression_s3tc", NULL, 0, halo_glCompressedTexImage2D != NULL,
		found, used, sizeof(found));
	context->compiled_vertex_array = context_feature("GL_EXT_compiled_vertex_array", NULL, 0,
		halo_glLockArraysEXT && halo_glUnlockArraysEXT, found, used, sizeof(found));
	context->blend_color = context_feature("GL_EXT_blend_color", NULL, 140, halo_glBlendColor != NULL,
		found, used, sizeof(found));
	context->blend_minmax = context_feature("GL_EXT_blend_minmax", NULL, 140, halo_glBlendEquation != NULL,
		found, used, sizeof(found));
	context->blend_subtract = context_feature("GL_EXT_blend_subtract", NULL, 140, halo_glBlendEquation != NULL,
		found, used, sizeof(found));
	context->edge_clamp = context_feature("GL_EXT_texture_edge_clamp", "GL_SGIS_texture_edge_clamp", 120, TRUE,
		found, used, sizeof(found));
	context->mirrored_repeat = context_feature("GL_ARB_texture_mirrored_repeat", "GL_IBM_texture_mirrored_repeat", 140,
		TRUE, found, used, sizeof(found));
	context->texture_lod = context_feature("GL_SGIS_texture_lod", NULL, 120, TRUE, found, used, sizeof(found));
	/* (the mip levels a texture lacks are made on the processor) */
	context->generate_mipmap = context_feature("GL_SGIS_generate_mipmap", NULL, 140, FALSE,
		found, used, sizeof(found));

	texture_bits = config_integer("display.gl1_texture_bits");
	context->textures_32bit = texture_bits >= 32;
	context->simple = setting_lists("simple");

	nv2a_ff_capabilities_level(context->units > 1 ? (context->combine ? 2 : 1) : 0, &plan_capabilities);
	plan_capabilities.units = (unsigned char)context->units;
	plan_capabilities.combine = (unsigned char)context->combine;
	plan_capabilities.combine_arb = (unsigned char)context->combine_arb;
	/* (the combine extension adds as well) */
	plan_capabilities.environment_add = (unsigned char)(context->environment_add || context->combine);
	plan_capabilities.dot3 = (unsigned char)context->dot3;
	plan_capabilities.blend_subtract = (unsigned char)context->blend_subtract;
	plan_capabilities.blend_minmax = (unsigned char)context->blend_minmax;
	plan_capabilities.blend_color = (unsigned char)context->blend_color;
	plan_capabilities.fog_coordinate = 0;
	plan_capabilities.cube_maps = 0;
	plan_capabilities.destination_alpha = context->alpha_bits > 0;
	/* Only derived textures that no constant enters: they are made once.
	One that reads a constant is made again whenever the constant changes,
	and the game's constants change all the time (a colour that pulses, a
	different one for each object): over a hundred textures a frame were
	remade, 57 ms of a frame on a fast processor. */
	plan_capabilities.bakes = _nv2a_ff_bakes_static;

	platform_log("gl1: OpenGL %d.%d (%s), textures up to %d, %d texture unit%s, colour %d:%d:%d:%d, depth %d, "
		"stencil %d, %d-bit textures; found: %s; used: %s%s",
		context->version / 100, context->version / 10 % 10, renderer ? renderer : "?",
		context->maximum_texture_size, context->units, context->units == 1 ? "" : "s",
		context->red_bits, context->green_bits, context->blue_bits, context->alpha_bits, context->depth_bits,
		context->stencil_bits, context->textures_32bit ? 32 : 16, *found ? found : "nothing",
		*used ? used : "nothing", context->simple ? "; simple: no combiner plans" : "");
	if (!context->depth_bits)
		platform_log("gl1: the window has no depth buffer: nothing is depth tested");

	/* the state the renderer never changes: OpenGL transforms nothing */
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glShadeModel(GL_SMOOTH);
	glDisable(GL_LIGHTING);
	glDisable(GL_FOG);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	while (glGetError() != GL_NO_ERROR)
		;
	xgpu_fixed_state_invalidate();
	return TRUE;
}

/* ---------- the processor's floating-point state

The evaluator runs with its own rounding and flushes denormal results to
zero. It puts MXCSR back itself; the x87 control word is kept here as
well, since the game's simulation shares the thread and must compute the
same results on every machine. */

struct float_state
{
	unsigned int mxcsr;
	unsigned short x87;
};

static void float_state_save(struct float_state *saved)
{
	saved->mxcsr = _mm_getcsr();
	__asm__ volatile ("fnstcw %0" : "=m" (saved->x87));
}

static void float_state_restore(const struct float_state *saved)
{
	unsigned int mxcsr = saved->mxcsr;

	__asm__ volatile ("fldcw %0" : : "m" (saved->x87));
	/* (never DAZ, bit 6: a Pentium III without it faults on the load, and
	tools/win9x_check.py wants every load shown to clear it, here by the
	instruction before) */
	__asm__ volatile ("andl $-0x41, %0; ldmxcsr %0" : "+m" (mxcsr));
}

/* ---------- statistics (debug.gpu_stats), reset each time they are logged */

static struct
{
	unsigned long frames;
	unsigned long draws, passes, vertices;
	unsigned long skipped_target, skipped_texture, skipped_program;
	unsigned long plans_compiled, fallback_draws, approximate_draws;
	/* draws whose positions OpenGL transforms (the lifted path) */
	unsigned long lifted_draws;
	unsigned long general_primaries;
	/* of these, by the plan's expressions */
	unsigned long interpreted_primaries;
	unsigned long visibility_tests;
	/* where a draw's time goes, in microseconds (only counted with
	debug.gpu_stats: reading the clock costs time of its own) */
	unsigned long plan_time, texture_time, vertex_time, pass_time;
	/* of the vertex and pass times: the evaluator's runs, the vertices
	they were for (a draw's once for each run over them) and their time;
	and the time of the colours computed vertex by vertex */
	unsigned long evaluator_runs, evaluator_vertices, evaluator_time, primary_time;
} stats;
static BOOL stats_timed;
static struct timespec stats_clock, stats_frames_clock;

/* the statistics are logged when this many seconds have passed */
#define STATISTICS_SECONDS 5

static unsigned long microseconds_between(const struct timespec *start, const struct timespec *end)
{
	return (unsigned long)((end->tv_sec - start->tv_sec) * 1000000L + (end->tv_nsec - start->tv_nsec) / 1000L);
}

/* ---------- GL state cache

As d3d8_gl.c's: each setter skips the call when OpenGL already holds the
value; unknown values are all ones. */

struct unit_state
{
	GLuint texture;
	unsigned char enabled;
	unsigned char coordinates;
	const void *coordinate_pointer;
	GLint coordinate_size;
	GLenum coordinate_type;
	GLsizei coordinate_stride;
	GLenum mode;
	struct nv2a_ff_combine rgb, alpha;
	float color[4];
	/* GL_TEXTURE, column major */
	float matrix[16];
};

static struct
{
	GLint viewport[4];
	float depth_range[2];
	unsigned char depth_test, stencil_test, blend, cull_face, offset_fill, offset_line, alpha_test;
	GLenum depth_function;
	unsigned char depth_mask;
	GLenum stencil_function;
	GLint stencil_reference;
	GLuint stencil_value_mask;
	GLenum stencil_operations[3];
	GLuint stencil_write_mask;
	GLenum blend_source, blend_destination, blend_equation;
	float blend_color[4];
	unsigned char color_mask;
	GLenum front_face, cull_mode, polygon_mode;
	float polygon_offset[2];
	GLenum alpha_function;
	float alpha_reference;
	int active_unit, client_unit;
	GLenum matrix_mode;
	unsigned char vertex_array, color_array;
	const void *vertex_pointer, *color_pointer;
	GLint vertex_size;
	GLenum vertex_type;
	GLsizei vertex_stride;
	float color[4];
	/* GL_PROJECTION: the identity, or the lifted draws' matrix */
	unsigned char projection_identity;
	float projection[16];
	struct unit_state units[NV2A_FF_MAXIMUM_UNITS];
} state;

void xgpu_fixed_state_invalidate(void)
{
	memset(&state, 0xff, sizeof(state));
}

static void state_enable(unsigned char *shadow, GLenum capability, BOOL enabled)
{
	unsigned char value = enabled ? 1 : 0;

	if (*shadow == value)
		return;
	*shadow = value;
	if (value)
		glEnable(capability);
	else
		glDisable(capability);
}

/* the unit that texture binds, enables, environments and matrices go to */
static void state_unit(int unit)
{
	if (state.active_unit != unit)
	{
		state.active_unit = unit;
		if (xgpu_fixed_context.multitexture)
			glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
	}
}

/* the unit whose texture coordinate array is being set */
static void state_client_unit(int unit)
{
	if (state.client_unit != unit)
	{
		state.client_unit = unit;
		if (xgpu_fixed_context.multitexture)
			glClientActiveTextureARB(GL_TEXTURE0 + (GLenum)unit);
	}
}

static void state_unit_enable(int unit, BOOL enabled)
{
	unsigned char value = enabled ? 1 : 0;

	if (state.units[unit].enabled == value)
		return;
	state_unit(unit);
	state.units[unit].enabled = value;
	if (value)
		glEnable(GL_TEXTURE_2D);
	else
		glDisable(GL_TEXTURE_2D);
}

static void state_unit_texture(int unit, GLuint texture)
{
	if (state.units[unit].texture == texture)
		return;
	state_unit(unit);
	state.units[unit].texture = texture;
	glBindTexture(GL_TEXTURE_2D, texture);
}

static const float identity_matrix[16] =
{
	1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f
};

static void state_matrix_mode(GLenum mode)
{
	if (state.matrix_mode != mode)
	{
		state.matrix_mode = mode;
		glMatrixMode(mode);
	}
}

/* a unit's GL_TEXTURE matrix (column major) */
static void state_unit_matrix_full(int unit, const float *matrix)
{
	if (!memcmp(state.units[unit].matrix, matrix, 16 * sizeof(float)))
		return;
	memcpy(state.units[unit].matrix, matrix, 16 * sizeof(float));
	state_unit(unit);
	state_matrix_mode(GL_TEXTURE);
	if (!memcmp(matrix, identity_matrix, sizeof(identity_matrix)))
		glLoadIdentity();
	else
		glLoadMatrixf(matrix);
}

/* s and t scaled and moved (the offset is multiplied by q) */
static void state_unit_matrix(int unit, const float *scale, const float *offset)
{
	float full[16] = { 0.0f };

	full[0] = scale[0];
	full[5] = scale[1];
	full[10] = 1.0f;
	full[12] = offset[0];
	full[13] = offset[1];
	full[15] = 1.0f;
	state_unit_matrix_full(unit, full);
}

/* GL_PROJECTION: the identity (the evaluator's clip-space positions), or
a matrix (the lifted draws': xgpu_fixed_draw) */
static void state_projection(const float *matrix)
{
	if (!matrix)
	{
		if (state.projection_identity == 1)
			return;
		state.projection_identity = 1;
		state_matrix_mode(GL_PROJECTION);
		glLoadIdentity();
		return;
	}
	if (state.projection_identity == 0 && !memcmp(state.projection, matrix, 16 * sizeof(float)))
		return;
	state.projection_identity = 0;
	memcpy(state.projection, matrix, 16 * sizeof(float));
	state_matrix_mode(GL_PROJECTION);
	glLoadMatrixf(matrix);
}

/* a unit's coordinate array (NULL: none); the evaluator's arrays are floats
four apart, a lifted draw's are the game's streams */
static void state_unit_coordinates(int unit, GLint size, GLenum type, GLsizei stride, const void *pointer)
{
	struct unit_state *shadow = &state.units[unit];
	unsigned char value = pointer ? 1 : 0;

	if (shadow->coordinates == value && (!pointer || (shadow->coordinate_pointer == pointer &&
		shadow->coordinate_size == size && shadow->coordinate_type == type && shadow->coordinate_stride == stride)))
	{
		return;
	}
	state_client_unit(unit);
	if (shadow->coordinates != value)
	{
		shadow->coordinates = value;
		if (value)
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		else
			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	}
	if (pointer && (shadow->coordinate_pointer != pointer || shadow->coordinate_size != size ||
		shadow->coordinate_type != type || shadow->coordinate_stride != stride))
	{
		shadow->coordinate_pointer = pointer;
		shadow->coordinate_size = size;
		shadow->coordinate_type = type;
		shadow->coordinate_stride = stride;
		glTexCoordPointer(size, type, stride, pointer);
	}
}

static void state_vertex_pointer(GLint size, GLenum type, GLsizei stride, const void *pointer)
{
	if (state.vertex_array != 1)
	{
		state.vertex_array = 1;
		glEnableClientState(GL_VERTEX_ARRAY);
	}
	if (state.vertex_pointer != pointer || state.vertex_size != size || state.vertex_type != type ||
		state.vertex_stride != stride)
	{
		state.vertex_pointer = pointer;
		state.vertex_size = size;
		state.vertex_type = type;
		state.vertex_stride = stride;
		glVertexPointer(size, type, stride, pointer);
	}
}

/* the primary colour: an array of four bytes a vertex, or one colour */
static void state_color(const void *pointer, const float *color)
{
	unsigned char value = pointer ? 1 : 0;

	if (state.color_array != value)
	{
		state.color_array = value;
		if (value)
			glEnableClientState(GL_COLOR_ARRAY);
		else
			glDisableClientState(GL_COLOR_ARRAY);
		/* (the current colour is undefined after a draw with the array) */
		memset(state.color, 0xff, sizeof(state.color));
	}
	if (pointer)
	{
		if (state.color_pointer != pointer)
		{
			state.color_pointer = pointer;
			glColorPointer(4, GL_UNSIGNED_BYTE, 4, pointer);
		}
	}
	else if (memcmp(state.color, color, sizeof(state.color)))
	{
		memcpy(state.color, color, sizeof(state.color));
		glColor4f(color[0], color[1], color[2], color[3]);
	}
}

/* small off-screen targets drawn in a corner of the window (below) */
static BOOL corner_target(const struct xgpu_fixed_target *target, struct xgpu_fixed_target *mapped);
static void corner_end(void);
static void window_geometry_note(const struct xgpu_fixed_target *target);

/* ---------- the frame: viewport, depth range, clears */

static float dword_to_float(DWORD value)
{
	union { DWORD d; float f; } u;

	u.d = value;
	return u.f;
}

/* an Xbox colour (alpha in the top byte) as red, green, blue, alpha */
static void color_to_vec4(D3DCOLOR color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

/* the pixel edge of a coordinate of the back buffer */
static GLint target_pixel(const struct xgpu_fixed_target *target, float coordinate, int axis)
{
	return (GLint)floorf(coordinate * target->scale[axis] + 0.5f);
}

/* A rectangle of the back buffer, given by its edges in the back buffer's
units with rows counted from the top, as OpenGL's window rectangle: x, y
of the lower left corner, width, height. */
static void window_rectangle(const struct xgpu_fixed_target *target, float x0, float y0, float x1, float y1,
	GLint *rectangle)
{
	GLint left = target_pixel(target, x0, 0), top = target_pixel(target, y0, 1);
	GLint right = target_pixel(target, x1, 0), bottom = target_pixel(target, y1, 1);

	rectangle[0] = (GLint)target->window_x + left;
	rectangle[1] = (GLint)target->window_y + ((GLint)target->gl_height - bottom);
	rectangle[2] = right - left;
	rectangle[3] = bottom - top;
}

static void off_screen_report(const struct xgpu_fixed_target *target)
{
	if (xgpu_fixed_off_screen_target(target->color_data))
	{
		platform_log("gl1: what the game draws into its off-screen target %08lx (%lux%lu) is left out",
			target->color_data, target->width, target->height);
	}
}

void xgpu_fixed_clear(const struct xgpu_fixed_target *target, long screen_offset, DWORD count,
	const D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	struct xgpu_fixed_target mapped;
	D3DRECT whole;
	BOOL has_depth, has_stencil;
	GLbitfield mask = 0;
	float rgba[4];
	DWORD index;

	if (!target->window)
	{
		if (!corner_target(target, &mapped))
		{
			if (target->color_data)
				off_screen_report(target);
			return;
		}
		/* (the corner alone, and the UI offset is the window's) */
		if (!count || !rectangles)
		{
			whole.x1 = whole.y1 = 0;
			whole.x2 = (LONG)target->width;
			whole.y2 = (LONG)target->height;
			rectangles = &whole;
			count = 1;
		}
		screen_offset = 0;
		target = &mapped;
	}
	else
	{
		corner_end();
		window_geometry_note(target);
	}
	has_depth = target->depth && xgpu_fixed_context.depth_bits > 0;
	has_stencil = target->depth && xgpu_fixed_context.stencil_bits > 0;
	color_to_vec4(color, rgba);
	if (flags & D3DCLEAR_TARGET)
	{
		/* the Xbox clears the channels named */
		glColorMask((flags & D3DCLEAR_TARGET_R) != 0, (flags & D3DCLEAR_TARGET_G) != 0,
			(flags & D3DCLEAR_TARGET_B) != 0, (flags & D3DCLEAR_TARGET_A) != 0);
		glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
		state.color_mask = (unsigned char)(((flags & D3DCLEAR_TARGET_R) ? 1 : 0) | ((flags & D3DCLEAR_TARGET_G) ? 2 : 0) |
			((flags & D3DCLEAR_TARGET_B) ? 4 : 0) | ((flags & D3DCLEAR_TARGET_A) ? 8 : 0));
		mask |= GL_COLOR_BUFFER_BIT;
	}
	if (has_depth && (flags & D3DCLEAR_ZBUFFER))
	{
		glDepthMask(GL_TRUE);
		glClearDepth(z);
		state.depth_mask = 1;
		mask |= GL_DEPTH_BUFFER_BIT;
	}
	if (has_stencil && (flags & D3DCLEAR_STENCIL))
	{
		glStencilMask(0xff);
		glClearStencil((GLint)stencil);
		state.stencil_write_mask = 0xff;
		mask |= GL_STENCIL_BUFFER_BIT;
	}
	if (!mask)
		return;
	if (!count || !rectangles)
	{
		/* the whole window, the bars beside the picture too */
		glDisable(GL_SCISSOR_TEST);
		glClear(mask);
	}
	else
	{
		glEnable(GL_SCISSOR_TEST);
		for (index = 0; index < count; index++)
		{
			GLint rectangle[4];

			window_rectangle(target, (float)(rectangles[index].x1 + screen_offset), (float)rectangles[index].y1,
				(float)(rectangles[index].x2 + screen_offset), (float)rectangles[index].y2, rectangle);
			if (rectangle[2] <= 0 || rectangle[3] <= 0)
				continue;
			glScissor(rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
			glClear(mask);
		}
		glDisable(GL_SCISSOR_TEST);
	}
	if (flags & D3DCLEAR_TARGET)
		xgpu_fixed_back_buffer_drawn();
}

/* ---------- raster state: what the plan has no say in */

static GLenum stencil_operation(DWORD operation)
{
	/* Xbox stencil operations are the GL enumerants, plus 0 for ZERO */
	return operation ? (GLenum)operation : GL_ZERO;
}

static void apply_raster_state(const struct xgpu_fixed_draw *draw, const struct xgpu_fixed_target *target, BOOL has_depth,
	BOOL has_stencil)
{
	DWORD *rs = D3D__RenderState;
	GLint viewport[4];
	float depth_range[2];
	BOOL stencil_test = has_stencil && rs[D3DRS_STENCILENABLE];

	window_rectangle(target, (float)draw->viewport.X, (float)draw->viewport.Y,
		(float)(draw->viewport.X + draw->viewport.Width), (float)(draw->viewport.Y + draw->viewport.Height), viewport);
	if (memcmp(state.viewport, viewport, sizeof(viewport)))
	{
		memcpy(state.viewport, viewport, sizeof(viewport));
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	}
	depth_range[0] = draw->viewport.MinZ;
	depth_range[1] = draw->viewport.MaxZ;
	if (memcmp(state.depth_range, depth_range, sizeof(depth_range)))
	{
		memcpy(state.depth_range, depth_range, sizeof(depth_range));
		glDepthRange(depth_range[0], depth_range[1]);
	}

	state_enable(&state.depth_test, GL_DEPTH_TEST, has_depth && rs[D3DRS_ZENABLE]);
	state_enable(&state.stencil_test, GL_STENCIL_TEST, stencil_test);
	if (stencil_test)
	{
		GLenum function = rs[D3DRS_STENCILFUNC] ? (GLenum)rs[D3DRS_STENCILFUNC] : GL_NEVER;
		GLenum operations[3];

		if (state.stencil_function != function || state.stencil_reference != (GLint)rs[D3DRS_STENCILREF] ||
			state.stencil_value_mask != rs[D3DRS_STENCILMASK])
		{
			state.stencil_function = function;
			state.stencil_reference = (GLint)rs[D3DRS_STENCILREF];
			state.stencil_value_mask = rs[D3DRS_STENCILMASK];
			glStencilFunc(function, (GLint)rs[D3DRS_STENCILREF], rs[D3DRS_STENCILMASK]);
		}
		operations[0] = stencil_operation(rs[D3DRS_STENCILFAIL]);
		operations[1] = stencil_operation(rs[D3DRS_STENCILZFAIL]);
		operations[2] = stencil_operation(rs[D3DRS_STENCILPASS]);
		if (memcmp(state.stencil_operations, operations, sizeof(operations)))
		{
			memcpy(state.stencil_operations, operations, sizeof(operations));
			glStencilOp(operations[0], operations[1], operations[2]);
		}
		if (state.stencil_write_mask != rs[D3DRS_STENCILWRITEMASK])
		{
			state.stencil_write_mask = rs[D3DRS_STENCILWRITEMASK];
			glStencilMask(rs[D3DRS_STENCILWRITEMASK]);
		}
	}

	/* the cull mode names the winding to discard, FRONTFACE the front
	winding; the window is drawn the right way up, so windings are the
	game's */
	state_enable(&state.cull_face, GL_CULL_FACE, rs[D3DRS_CULLMODE] != D3DCULL_NONE);
	if (rs[D3DRS_CULLMODE] != D3DCULL_NONE)
	{
		GLenum front_face = rs[D3DRS_FRONTFACE] == D3DFRONT_CCW ? GL_CCW : GL_CW;
		GLenum cull_mode = rs[D3DRS_CULLMODE] == rs[D3DRS_FRONTFACE] ? GL_FRONT : GL_BACK;

		if (state.front_face != front_face)
		{
			state.front_face = front_face;
			glFrontFace(front_face);
		}
		if (state.cull_mode != cull_mode)
		{
			state.cull_mode = cull_mode;
			glCullFace(cull_mode);
		}
	}
	{
		GLenum polygon_mode = rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? GL_LINE :
			rs[D3DRS_FILLMODE] == D3DFILL_POINT ? GL_POINT : GL_FILL;

		if (state.polygon_mode != polygon_mode)
		{
			state.polygon_mode = polygon_mode;
			glPolygonMode(GL_FRONT_AND_BACK, polygon_mode);
		}
	}

	/* D3DRS_ZBIAS is expressed in these states (d3d8_gl.c): a slope factor
	and a number of the Xbox's 24-bit depth steps. OpenGL counts the latter
	in steps of its own depth buffer; in one of 16 bits the same distance
	would be a small fraction of a step, so at least two steps are kept
	(decals lie on walls seen head on). */
	state_enable(&state.offset_fill, GL_POLYGON_OFFSET_FILL, rs[D3DRS_SOLIDOFFSETENABLE] != 0);
	state_enable(&state.offset_line, GL_POLYGON_OFFSET_LINE, rs[D3DRS_SOLIDOFFSETENABLE] != 0);
	if (rs[D3DRS_SOLIDOFFSETENABLE])
	{
		float offset[2];

		offset[0] = dword_to_float(rs[D3DRS_POLYGONOFFSETZSLOPESCALE]);
		offset[1] = dword_to_float(rs[D3DRS_POLYGONOFFSETZOFFSET]);
		if (xgpu_fixed_context.depth_bits > 0 && xgpu_fixed_context.depth_bits < 24 && offset[1] != 0.0f)
		{
			float steps = offset[1] / (float)(1UL << (24 - xgpu_fixed_context.depth_bits));

			if (steps > -2.0f && steps < 2.0f)
				steps = offset[1] < 0.0f ? -2.0f : 2.0f;
			offset[1] = steps;
		}
		if (memcmp(state.polygon_offset, offset, sizeof(offset)))
		{
			memcpy(state.polygon_offset, offset, sizeof(offset));
			glPolygonOffset(offset[0], offset[1]);
		}
	}
}

/* ---------- the pixel state and its plans */

static GLenum blend_equation(DWORD operation)
{
	switch (operation)
	{
	case D3DBLENDOP_SUBTRACT: return GL_FUNC_SUBTRACT;
	case D3DBLENDOP_REVSUBTRACT:
	case D3DBLENDOP_REVSUBTRACTSIGNED: return GL_FUNC_REVERSE_SUBTRACT;
	case D3DBLENDOP_MIN: return GL_MIN;
	case D3DBLENDOP_MAX: return GL_MAX;
	default: return GL_FUNC_ADD;
	}
}

/* what a pass's primary colour is made from */
enum
{
	/* the pass does not read it */
	_primary_none,
	/* one colour for the whole draw */
	_primary_constant,
	/* One of the vertex program's colours, each channel scaled and moved:
	red, green and blue from color_source, alpha from one component of
	alpha_source. The evaluator writes it (with the scale; the bias is
	added afterwards). */
	_primary_output,
	/* anything else: computed here at each vertex, from the polynomial
	below where the colour is one, and by the plan's expressions otherwise */
	_primary_general
};

/* A primary colour as a polynomial of the vertex's colours and fog: each
channel a sum of terms, a term a coefficient times those of the variables
its mask names. Red and green read their own channel of a colour, blue and
alpha its blue. */
enum
{
	_primary_variable_v0,
	_primary_variable_v0_blue,
	_primary_variable_v0_alpha,
	_primary_variable_v1,
	_primary_variable_v1_blue,
	_primary_variable_v1_alpha,
	_primary_variable_fog,
	NUMBER_OF_PRIMARY_VARIABLES,

	PRIMARY_MAXIMUM_TERMS = 12
};

struct primary_term
{
	unsigned char variables;
	float coefficient;
};

struct primary
{
	int kind;
	float constant[4];
	unsigned char color_source, alpha_source, alpha_component;
	BOOL scaled, biased;
	float scale[4];
	float bias[4];
	BOOL uses_v0, uses_v1, uses_fog;
	/* the inputs it changes with (a bit for each _input_*) */
	unsigned depends;
	/* _primary_general: the colour is the polynomial of terms */
	BOOL polynomial;
	unsigned char term_count[4];
	struct primary_term terms[4][PRIMARY_MAXIMUM_TERMS];
	/* _primary_general without a polynomial: the plan's expressions
	compiled (primary_program_compile), or NULL for nv2a_ff_plan_primary */
	struct primary_program *program;
};

/* The vertex program's colours and fog that are the same at every vertex
of a draw: the program does not write them, or makes them from constants
and registers that no stream feeds. A primary colour that reads them is
worked out with their values as if they were constants, and is then often
one colour for the draw, or one the evaluator can write. */
struct primary_fixed
{
	/* a bit for each _input_* that is */
	unsigned mask;
	float v0[4];
	float v1[4];
	/* the fog factor */
	float fog;
};

struct plan_key
{
	struct nv2a_ff_state state;
	struct nv2a_ff_draw draw;
};

/* a plan and what was worked out about it */
struct plan_variant
{
	struct plan_variant *next;
	/* made for these constants alone; otherwise for any */
	BOOL bound;
	struct nv2a_ff_constants bound_constants;
	/* stands for the entry's general plan: the compiler had no better one
	for these constants */
	BOOL same_as_general;
	/* the built-in plan: the compiler had none */
	BOOL fallback;
	struct nv2a_ff_plan plan;
	/* the primaries, for the constants they were last analysed with */
	BOOL analyzed;
	struct nv2a_ff_constants analyzed_constants;
	struct primary primaries[NV2A_FF_MAXIMUM_PASSES];
	/* the same for draws with inputs that do not vary, for the constants
	and the inputs' values they were last analysed with (one for each
	pass, made when the first such draw comes) */
	BOOL fixed_analyzed;
	struct nv2a_ff_constants fixed_constants;
	struct primary_fixed fixed;
	struct primary *fixed_primaries;
};

static void variant_clear_primaries(struct plan_variant *variant);

struct plan_entry
{
	struct plan_entry *next;
	unsigned long hash;
	struct plan_key key;
	struct plan_variant general;
	/* the general plan is not exact: plans for the constants of the
	moment are tried, newest first */
	BOOL try_bound;
	struct plan_variant *bound;
	unsigned long bound_count;
	/* plans compiled for this state's constants so far */
	unsigned long bound_compiles;
	/* the compiler's plan for all constants drew no texture (plan_textureless) */
	BOOL textureless;
	BOOL reported;
	/* what its plans leave out has been logged (debug.gpu_stats): the one
	for all constants, and the first of those for constants */
	BOOL described, described_bound;
};

#define PLAN_BUCKETS 1024
/* plans for particular constants kept for one state */
#define PLAN_BOUND_VARIANTS 6
/* and compiled in one frame: the others draw with the general plan until
their turn comes. Compiling a long state takes tens of milliseconds (a
model's eight stages: 20 ms on a fast processor), so one a frame, and a
state whose constants never stop changing (a colour that fades, a
different one for each object) gets only so many in all: after that it
draws with the plans it has, or the general one. */
#define PLAN_BOUND_COMPILES_A_FRAME 1
#define PLAN_BOUND_COMPILES_A_STATE 8

static struct plan_entry *plan_buckets[PLAN_BUCKETS];
static unsigned long bound_compiles_this_frame;

static unsigned long hash_bytes(const void *data, unsigned long size)
{
	const unsigned char *bytes = data;
	unsigned long hash = 2166136261UL;

	for (; size; size--)
		hash = (hash ^ *bytes++) * 16777619UL;
	return hash;
}

/* The built-in plan: the first texture times the vertex colour, with the
draw's alpha test and blend. */
static void fallback_plan(const struct plan_key *key, struct plan_variant *variant)
{
	struct nv2a_ff_plan *plan = &variant->plan;
	struct nv2a_ff_pass *pass = &plan->passes[0];
	unsigned long mode = key->state.texture_modes & 0x1f;
	int index;

	memset(plan, 0, sizeof(*plan));
	plan->status = _nv2a_ff_plan_approximate;
	plan->pass_count = 1;
	if (key->state.sampler_type[0] == _xgpu_sampler_2d && mode == 1)
	{
		pass->unit_count = 1;
		pass->units[0].stage = 0;
		pass->units[0].bake = NV2A_FF_BAKE_NONE;
		pass->units[0].environment = _nv2a_ff_environment_modulate;
		pass->units[0].constant_rgb = pass->units[0].constant_alpha = NV2A_FF_EXPRESSION_NONE;
	}
	pass->primary_rgb = pass->primary_alpha = NV2A_FF_EXPRESSION_NONE;
	pass->fog_factor = pass->fog_color = NV2A_FF_EXPRESSION_NONE;
	pass->alpha_test_function = (unsigned short)key->state.alpha_test_function;
	pass->alpha_reference = NV2A_FF_EXPRESSION_NONE;
	pass->blend_enable = key->draw.blend_enable;
	pass->source_blend = (unsigned short)key->draw.source_blend;
	pass->destination_blend = (unsigned short)key->draw.destination_blend;
	pass->blend_operation = (unsigned short)key->draw.blend_operation;
	pass->write_color = key->draw.write_color;
	pass->write_alpha = key->draw.write_alpha;

	variant->fallback = TRUE;
	variant->analyzed = TRUE;
	memset(&variant->analyzed_constants, 0xff, sizeof(variant->analyzed_constants));
	for (index = 0; index < NV2A_FF_MAXIMUM_PASSES; index++)
		memset(&variant->primaries[index], 0, sizeof(variant->primaries[index]));
	variant->primaries[0].kind = _primary_output;
	variant->primaries[0].color_source = _nv2a_vsh_output_diffuse;
	variant->primaries[0].alpha_source = _nv2a_vsh_output_diffuse;
	variant->primaries[0].alpha_component = 3;
}

/* the words of a pixel state that say what it computes, for the log */
static void plan_key_describe(const struct plan_key *key, char *text, size_t size)
{
	const unsigned long *words = key->state.combiner_state;
	unsigned long count = words[D3DRS_PSCOMBINERCOUNT] & 0xf, index;
	size_t length;

	snprintf(text, size, "modes %05lx types %d%d%d%d count %lx final %08lx/%08lx compare %lx dot %lx input %lx "
		"alpha test %lx kill %d%d%d%d fog %d/%d blend %d %lx/%lx/%lx write %d%d depth %d rank %d%d%d%d;",
		key->state.texture_modes, key->state.sampler_type[0], key->state.sampler_type[1],
		key->state.sampler_type[2], key->state.sampler_type[3], words[D3DRS_PSCOMBINERCOUNT],
		words[D3DRS_PSFINALCOMBINERINPUTSABCD], words[D3DRS_PSFINALCOMBINERINPUTSEFG], words[D3DRS_PSCOMPAREMODE],
		words[D3DRS_PSDOTMAPPING], words[D3DRS_PSINPUTTEXTURE], key->state.alpha_test_function,
		key->state.alpha_kill[0], key->state.alpha_kill[1], key->state.alpha_kill[2], key->state.alpha_kill[3],
		key->state.fog_enable, key->state.fog_table_mode, key->draw.blend_enable, key->draw.source_blend,
		key->draw.destination_blend, key->draw.blend_operation, key->draw.write_color, key->draw.write_alpha,
		key->draw.depth_write, key->draw.stage_rank[0], key->draw.stage_rank[1], key->draw.stage_rank[2],
		key->draw.stage_rank[3]);
	for (index = 0; index < count && index < 8; index++)
	{
		length = strlen(text);
		if (length + 48 >= size)
			break;
		snprintf(text + length, size - length, " %lu: %08lx>%lx %08lx>%lx", index,
			words[D3DRS_PSRGBINPUTS0 + index], words[D3DRS_PSRGBOUTPUTS0 + index],
			words[D3DRS_PSALPHAINPUTS0 + index], words[D3DRS_PSALPHAOUTPUTS0 + index]);
	}
}

/* Whether a plan draws none of the state's textures where the built-in
plan would draw the first: when the compiler can fit nothing else it leaves
every texture out and keeps the vertex colours (a model then is its
lighting alone, without its skin). The first texture times the vertex
colour is nearer to what the state draws than that. */
static BOOL plan_textureless(const struct plan_key *key, const struct nv2a_ff_plan *plan)
{
	int pass, unit;

	if (plan->status == _nv2a_ff_plan_exact || !(plan->approximations & _nv2a_ff_approximate_dropped_texture))
		return FALSE;
	if (key->state.sampler_type[0] != _xgpu_sampler_2d || (key->state.texture_modes & 0x1f) != 1)
		return FALSE;
	for (pass = 0; pass < plan->pass_count && pass < NV2A_FF_MAXIMUM_PASSES; pass++)
	{
		for (unit = 0; unit < plan->passes[pass].unit_count && unit < NV2A_FF_MAXIMUM_UNITS; unit++)
		{
			if (plan->passes[pass].units[unit].stage < NV2A_FF_STAGE_COUNT)
				return FALSE;
		}
	}
	return TRUE;
}

static struct plan_entry *plan_entry_get(const struct plan_key *key)
{
	static struct plan_entry *last;
	unsigned long hash;
	struct plan_entry **bucket, *entry;

	if (last && !memcmp(&last->key, key, sizeof(*key)))
		return last;
	hash = hash_bytes(key, sizeof(*key));
	bucket = &plan_buckets[hash % PLAN_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && !memcmp(&entry->key, key, sizeof(*key)))
		{
			last = entry;
			return entry;
		}
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	entry->hash = hash;
	entry->key = *key;
	if (xgpu_fixed_context.simple)
	{
		fallback_plan(key, &entry->general);
	}
	else
	{
		stats.plans_compiled++;
		nv2a_ff_compile(&key->state, NULL, &plan_capabilities, &key->draw, &entry->general.plan);
		entry->textureless = plan_textureless(key, &entry->general.plan);
		if (entry->general.plan.status == _nv2a_ff_plan_impossible || !entry->general.plan.pass_count || entry->textureless)
			fallback_plan(key, &entry->general);
		entry->try_bound = entry->general.fallback || entry->general.plan.status != _nv2a_ff_plan_exact;
	}
	entry->next = *bucket;
	*bucket = entry;
	last = entry;
	return entry;
}

/* sample fragments a plan's error is measured on */
#define PLAN_ERROR_SAMPLES 24

/* How far what a plan draws is from what the state draws with these
constants: the largest difference of a channel, in steps of 1/255, on
average over sample fragments (the compiler's own comparison; the same
fragments for every plan). */
static float plan_error(const struct plan_key *key, const struct nv2a_ff_plan *plan,
	const struct nv2a_ff_constants *constants)
{
	unsigned long seed = 0x2f6b43a1UL;
	float sum = 0.0f;
	int index;

	for (index = 0; index < PLAN_ERROR_SAMPLES; index++)
	{
		struct nv2a_ff_sample sample;

		nv2a_ff_sample_random(&seed, &sample);
		sum += nv2a_ff_sample_compare(&key->state, constants, &plan_capabilities, &key->draw, plan, &sample, NULL, NULL);
	}
	return sum / (float)PLAN_ERROR_SAMPLES;
}

/* the plan to draw the state with for these constants */
static struct plan_variant *plan_get(struct plan_entry *entry, const struct nv2a_ff_constants *constants)
{
	struct plan_variant *variant = &entry->general, **link, *bound = NULL;

	if (entry->try_bound)
	{
		/* A plan made for constants holds for as long as the ones it was
		made for keep their values, whatever the others do (a colour that
		fades is seldom one of them). Where the compiler had nothing better
		than the general plan, that is known of the constants asked with
		only. */
		for (link = &entry->bound; (bound = *link) != NULL; link = &bound->next)
		{
			if (bound->same_as_general ? !memcmp(&bound->bound_constants, constants, sizeof(*constants)) :
				nv2a_ff_plan_constants_match(&bound->plan, &bound->bound_constants, constants))
			{
				break;
			}
		}
		if (bound)
		{
			/* to the front */
			*link = bound->next;
			bound->next = entry->bound;
			entry->bound = bound;
		}
		else if (bound_compiles_this_frame < PLAN_BOUND_COMPILES_A_FRAME &&
			entry->bound_compiles < PLAN_BOUND_COMPILES_A_STATE)
		{
			bound_compiles_this_frame++;
			entry->bound_compiles++;
			if (entry->bound_count >= PLAN_BOUND_VARIANTS)
			{
				struct primary *fixed_primaries;

				/* the one unused for longest makes room */
				for (link = &entry->bound; (*link)->next; link = &(*link)->next)
					;
				bound = *link;
				*link = NULL;
				/* (its primaries for fixed inputs are kept for the next plan's) */
				variant_clear_primaries(bound);
				fixed_primaries = bound->fixed_primaries;
				memset(bound, 0, sizeof(*bound));
				bound->fixed_primaries = fixed_primaries;
			}
			else
			{
				bound = calloc(1, sizeof(*bound));
				if (bound)
					entry->bound_count++;
			}
			if (bound)
			{
				bound->bound = TRUE;
				bound->bound_constants = *constants;
				/* (derived textures stay allowed here: the models' plans
				need them. How many are made a frame is bounded in
				xgpu_fixed_bake_get.) */
				stats.plans_compiled++;
				nv2a_ff_compile(&entry->key.state, constants, &plan_capabilities, &entry->key.draw, &bound->plan);
				if (!bound->plan.constants_bound || bound->plan.status == _nv2a_ff_plan_impossible ||
					!bound->plan.pass_count || plan_textureless(&entry->key, &bound->plan))
				{
					bound->same_as_general = TRUE;
				}
				else if (!entry->general.fallback && plan_error(&entry->key, &entry->general.plan, constants) <=
					plan_error(&entry->key, &bound->plan, constants))
				{
					/* The plan for all constants may have derived textures
					and this one has none: where that makes the first the
					nearer to what the state draws with these constants,
					it stays. */
					bound->same_as_general = TRUE;
				}
				bound->next = entry->bound;
				entry->bound = bound;
			}
		}
		else if (entry->general.fallback || (entry->general.plan.approximations & _nv2a_ff_approximate_dropped_texture))
		{
			/* No plan for these constants, and none is to be made now. The
			general plan has given up textures (a model's is its vertex
			colours alone): the newest plan made for other constants is
			nearer the mark. Its constant colours are worked out from the
			draw's own constants all the same. */
			for (bound = entry->bound; bound && bound->same_as_general; bound = bound->next)
				;
		}
		if (bound && !bound->same_as_general)
			variant = bound;
	}
	if (variant->fallback && !entry->reported && !xgpu_fixed_context.simple && (!entry->try_bound || bound))
	{
		char text[1024];

		entry->reported = TRUE;
		plan_key_describe(&entry->key, text, sizeof(text));
		platform_log("gl1: %s for %08lx: %s (drawn as its first texture times the vertex colour)",
			entry->textureless ? "only a plan without textures" : "no fixed-function form",
			nv2a_ff_state_hash(&entry->key.state), text);
	}
	return variant;
}

/* what an approximate plan gave up, in words, for the log */
static void plan_approximations_describe(const struct nv2a_ff_plan *plan, char *text, size_t size)
{
	static const struct
	{
		unsigned long flag;
		const char *name;
	} names[] =
	{
		{ _nv2a_ff_approximate_dropped_texture, "textures" },
		{ _nv2a_ff_approximate_vertex_term, "a term computed at the vertices" },
		{ _nv2a_ff_approximate_bake_filter, "a derived texture's filtering" },
		{ _nv2a_ff_approximate_alpha_kill, "alpha kill" },
		{ _nv2a_ff_approximate_clip_plane, "a clip plane" },
		{ _nv2a_ff_approximate_fog_table, "the fog table at the pixels" },
		{ _nv2a_ff_approximate_blend, "the blend" },
		{ _nv2a_ff_approximate_unexplained, "something the comparison found" },
		{ _nv2a_ff_approximate_destination_alpha, "destination alpha" },
		{ _nv2a_ff_approximate_dropped_term, "a term" },
		{ _nv2a_ff_approximate_clamp, "a clamp" },
	};
	unsigned long index, known = 0;
	int stage;

	*text = 0;
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++)
	{
		known |= names[index].flag;
		if ((plan->approximations & names[index].flag) && strlen(text) + strlen(names[index].name) + 3 < size)
		{
			if (*text)
				strcat(text, ", ");
			strcat(text, names[index].name);
		}
	}
	if ((plan->approximations & ~known) && strlen(text) + 16 < size)
		snprintf(text + strlen(text), size - strlen(text), "%sflags %lx", *text ? ", " : "", plan->approximations & ~known);
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		if (plan->dropped_stage[stage] != _nv2a_ff_substitute_none && strlen(text) + 24 < size)
			snprintf(text + strlen(text), size - strlen(text), "%sstage %d's texture", *text ? ", " : "", stage);
	}
}

/* ---------- a plan's primary colours */

static float fog_factor_from_coordinate(const struct nv2a_ff_state *ff_state,
	const struct nv2a_ff_constants *constants, float coordinate)
{
	float factor = 1.0f;

	/* as nv2a_psh.c's shaders compute it */
	if (ff_state->fog_enable)
	{
		float span;

		switch (ff_state->fog_table_mode)
		{
		case D3DFOG_EXP:
			factor = expf(-constants->fog_parameters[2] * coordinate);
			break;
		case D3DFOG_EXP2:
			factor = constants->fog_parameters[2] * coordinate;
			factor = expf(-factor * factor);
			break;
		case D3DFOG_LINEAR:
			span = constants->fog_parameters[1] - constants->fog_parameters[0];
			factor = (constants->fog_parameters[1] - coordinate) / (span > 1.0e-6f ? span : 1.0e-6f);
			break;
		default:
			factor = coordinate;
			break;
		}
	}
	return factor < 0.0f ? 0.0f : factor > 1.0f ? 1.0f : factor;
}

/* the inputs a primary colour may depend on */
enum
{
	_input_v0_rgb,
	_input_v0_alpha,
	_input_v1_rgb,
	_input_v1_alpha,
	_input_fog,
	NUMBER_OF_PRIMARY_INPUTS
};

struct primary_inputs
{
	float v0[4];
	float v1[4];
	float fog;
};

/* a pass's primary colour for these inputs; those of fixed (which may be
null) have its values whatever inputs says */
static void primary_evaluate(const struct nv2a_ff_plan *plan, int pass, const struct nv2a_ff_constants *constants,
	const struct primary_inputs *inputs, const struct primary_fixed *fixed, float *color)
{
	struct primary_inputs tried = *inputs;
	struct nv2a_ff_values values;

	if (fixed)
	{
		if (fixed->mask & (1u << _input_v0_rgb))
			memcpy(tried.v0, fixed->v0, 3 * sizeof(float));
		if (fixed->mask & (1u << _input_v0_alpha))
			tried.v0[3] = fixed->v0[3];
		if (fixed->mask & (1u << _input_v1_rgb))
			memcpy(tried.v1, fixed->v1, 3 * sizeof(float));
		if (fixed->mask & (1u << _input_v1_alpha))
			tried.v1[3] = fixed->v1[3];
		if (fixed->mask & (1u << _input_fog))
			tried.fog = fixed->fog;
	}
	memset(&values, 0, sizeof(values));
	values.constants = constants;
	values.v0 = tried.v0;
	values.v1 = tried.v1;
	values.fog_factor = tried.fog;
	nv2a_ff_plan_primary(plan, pass, &values, color);
}

static BOOL close_enough(float a, float b)
{
	float difference = a - b;

	return difference > -1.0e-3f && difference < 1.0e-3f;
}

/* Finds out what a pass's primary colour is by trying it: which of the
vertex's colours and fog its channels change with, and whether each
channel is one input channel scaled and moved, which the evaluator can
write directly. Anything else is left general. */
static void primary_analyze_output(const struct nv2a_ff_plan *plan, int pass_index, const struct nv2a_ff_state *ff_state,
	const struct nv2a_ff_constants *constants, const struct primary_fixed *fixed, struct primary *primary)
{
	static const struct primary_inputs bases[2] =
	{
		{ { 0.30f, 0.55f, 0.70f, 0.45f }, { 0.60f, 0.25f, 0.40f, 0.65f }, 0.35f },
		{ { 0.85f, 0.10f, 0.20f, 0.90f }, { 0.15f, 0.75f, 0.95f, 0.20f }, 0.80f },
	};
	const struct nv2a_ff_pass *pass = &plan->passes[pass_index];
	/* a fog factor that is not the vertex program's own output cannot be
	had from the evaluator's colour */
	BOOL fog_varies = ff_state->fog_enable != 0;
	BOOL fog_direct = ff_state->fog_enable && ff_state->fog_table_mode == D3DFOG_NONE;
	unsigned rgb_inputs = 0, alpha_inputs = 0;
	struct primary_inputs inputs;
	float at_base[2][4], color[4], low[4], high[4];
	int base, input, channel;
	int color_channel_base = -1, alpha_input = -1, alpha_channel = 3;

	memset(primary, 0, sizeof(*primary));
	if (pass->primary_rgb == NV2A_FF_EXPRESSION_NONE && pass->primary_alpha == NV2A_FF_EXPRESSION_NONE)
	{
		primary->kind = _primary_none;
		return;
	}
	for (base = 0; base < 2; base++)
	{
		inputs = bases[base];
		if (!fog_varies)
			inputs.fog = 1.0f;
		primary_evaluate(plan, pass_index, constants, &inputs, fixed, at_base[base]);
		for (input = 0; input < NUMBER_OF_PRIMARY_INPUTS; input++)
		{
			inputs = bases[base];
			if (!fog_varies)
				inputs.fog = 1.0f;
			switch (input)
			{
			case _input_v0_rgb: memcpy(inputs.v0, bases[1 - base].v1, 3 * sizeof(float)); break;
			case _input_v0_alpha: inputs.v0[3] = bases[1 - base].v0[3]; break;
			case _input_v1_rgb: memcpy(inputs.v1, bases[1 - base].v0, 3 * sizeof(float)); break;
			case _input_v1_alpha: inputs.v1[3] = bases[1 - base].v1[3]; break;
			default:
				if (!fog_varies)
					continue;
				inputs.fog = bases[1 - base].fog;
				break;
			}
			primary_evaluate(plan, pass_index, constants, &inputs, fixed, color);
			if (color[0] != at_base[base][0] || color[1] != at_base[base][1] || color[2] != at_base[base][2])
				rgb_inputs |= 1u << input;
			if (color[3] != at_base[base][3])
				alpha_inputs |= 1u << input;
		}
	}
	if (pass->primary_is_constant || (!rgb_inputs && !alpha_inputs))
	{
		primary->kind = _primary_constant;
		memcpy(primary->constant, at_base[0], sizeof(primary->constant));
		return;
	}
	primary->depends = rgb_inputs | alpha_inputs;
	primary->kind = _primary_general;
	primary->uses_v0 = ((rgb_inputs | alpha_inputs) & ((1u << _input_v0_rgb) | (1u << _input_v0_alpha))) != 0;
	primary->uses_v1 = ((rgb_inputs | alpha_inputs) & ((1u << _input_v1_rgb) | (1u << _input_v1_alpha))) != 0;
	primary->uses_fog = ((rgb_inputs | alpha_inputs) & (1u << _input_fog)) != 0;

	/* red, green and blue: one colour's own channels, or nothing */
	if (rgb_inputs == 0 || rgb_inputs == 1u << _input_v0_rgb)
		color_channel_base = _input_v0_rgb;
	else if (rgb_inputs == 1u << _input_v1_rgb)
		color_channel_base = _input_v1_rgb;
	else
		return;
	/* alpha: one alpha, one blue, the fog factor, or nothing */
	if (alpha_inputs == 0 || alpha_inputs == 1u << _input_v0_alpha)
		alpha_input = _input_v0_alpha;
	else if (alpha_inputs == 1u << _input_v1_alpha)
		alpha_input = _input_v1_alpha;
	else if (alpha_inputs == 1u << _input_fog && fog_direct)
		alpha_input = _input_fog;
	else if (alpha_inputs == 1u << _input_v0_rgb || alpha_inputs == 1u << _input_v1_rgb)
	{
		alpha_input = alpha_inputs == 1u << _input_v0_rgb ? _input_v0_rgb : _input_v1_rgb;
		alpha_channel = 2;
	}
	else
		return;

	/* each channel with its input at 0 and at 1 gives the bias and the
	scale; the two bases then say whether that is all there is to it */
	inputs = bases[0];
	if (!fog_varies)
		inputs.fog = 1.0f;
	{
		float *color_input = color_channel_base == _input_v0_rgb ? inputs.v0 : inputs.v1;
		float *alpha_value = alpha_input == _input_fog ? &inputs.fog :
			alpha_input == _input_v0_alpha || alpha_input == _input_v0_rgb ? &inputs.v0[alpha_channel] :
			&inputs.v1[alpha_channel];

		color_input[0] = color_input[1] = color_input[2] = 0.0f;
		*alpha_value = 0.0f;
		primary_evaluate(plan, pass_index, constants, &inputs, fixed, low);
		color_input[0] = color_input[1] = color_input[2] = 1.0f;
		*alpha_value = 1.0f;
		primary_evaluate(plan, pass_index, constants, &inputs, fixed, high);
		for (channel = 0; channel < 4; channel++)
		{
			primary->bias[channel] = low[channel];
			primary->scale[channel] = high[channel] - low[channel];
		}
		for (base = 0; base < 2; base++)
		{
			const float *color_at = color_channel_base == _input_v0_rgb ? bases[base].v0 : bases[base].v1;
			float alpha_at = alpha_input == _input_fog ? bases[base].fog :
				alpha_input == _input_v0_alpha || alpha_input == _input_v0_rgb ? bases[base].v0[alpha_channel] :
				bases[base].v1[alpha_channel];

			for (channel = 0; channel < 4; channel++)
			{
				float predicted = primary->bias[channel] +
					primary->scale[channel] * (channel < 3 ? color_at[channel] : alpha_at);

				predicted = predicted < 0.0f ? 0.0f : predicted > 1.0f ? 1.0f : predicted;
				if (!close_enough(predicted, at_base[base][channel]))
					return;
			}
		}
	}
	primary->kind = _primary_output;
	primary->color_source = color_channel_base == _input_v0_rgb ? _nv2a_vsh_output_diffuse : _nv2a_vsh_output_specular;
	primary->alpha_source = alpha_input == _input_fog ? _nv2a_vsh_output_fog :
		alpha_input == _input_v0_alpha || alpha_input == _input_v0_rgb ? _nv2a_vsh_output_diffuse :
		_nv2a_vsh_output_specular;
	primary->alpha_component = alpha_input == _input_fog ? 0 : (unsigned char)alpha_channel;
	for (channel = 0; channel < 4; channel++)
	{
		if (primary->bias[channel] > 1.0e-6f || primary->bias[channel] < -1.0e-6f)
			primary->biased = TRUE;
		if (primary->scale[channel] != 1.0f)
			primary->scaled = TRUE;
	}
}

/* a polynomial's channel for these values of the variables */
static float primary_polynomial(const struct primary *primary, int channel, const float *variables)
{
	const struct primary_term *term = primary->terms[channel];
	unsigned long count = primary->term_count[channel];
	float sum = 0.0f;

	for (; count; count--, term++)
	{
		float product = term->coefficient;
		unsigned mask = term->variables;
		int variable;

		for (variable = 0; mask; variable++, mask >>= 1)
		{
			if (mask & 1)
				product *= variables[variable];
		}
		sum += product;
	}
	return sum;
}

/* the variables of a channel from a vertex's colours and fog factor */
static void primary_variables(int channel, const float *v0, const float *v1, float fog, float *variables)
{
	variables[_primary_variable_v0] = v0[channel < 2 ? channel : 2];
	variables[_primary_variable_v0_blue] = v0[2];
	variables[_primary_variable_v0_alpha] = v0[3];
	variables[_primary_variable_v1] = v1[channel < 2 ? channel : 2];
	variables[_primary_variable_v1_blue] = v1[2];
	variables[_primary_variable_v1_alpha] = v1[3];
	variables[_primary_variable_fog] = fog;
}

/* Tries a general primary colour as a polynomial of the inputs it changes
with, of the first degree in each: most are (a colour times a constant plus
another, a product of two of the vertex's values). The colour at the
corners of the inputs' range gives the coefficients; a few points inside it
then say whether that is all there is to the colour (a clamp or a square in
it shows there). TRUE, and the terms in primary, if so. */
static BOOL primary_fit_polynomial(const struct nv2a_ff_plan *plan, int pass_index, const struct nv2a_ff_state *ff_state,
	const struct nv2a_ff_constants *constants, const struct primary_fixed *fixed, struct primary *primary)
{
	/* values inside the range, different for every variable and channel */
	static const struct primary_inputs points[] =
	{
		{ { 0.21f, 0.64f, 0.83f, 0.37f }, { 0.72f, 0.18f, 0.45f, 0.91f }, 0.56f },
		{ { 0.93f, 0.12f, 0.31f, 0.77f }, { 0.08f, 0.59f, 0.86f, 0.24f }, 0.13f },
		{ { 0.47f, 0.88f, 0.06f, 0.15f }, { 0.34f, 0.97f, 0.22f, 0.61f }, 0.89f },
		{ { 0.68f, 0.29f, 0.52f, 0.96f }, { 0.81f, 0.43f, 0.09f, 0.38f }, 0.42f },
		{ { 0.05f, 0.51f, 0.94f, 0.58f }, { 0.49f, 0.76f, 0.67f, 0.03f }, 0.71f },
	};
	static float corners[1 << NUMBER_OF_PRIMARY_VARIABLES][4];
	int variables[NUMBER_OF_PRIMARY_VARIABLES];
	int count = 0, corner, bit, channel, point;
	BOOL fog_varies = ff_state->fog_enable != 0;

	if (primary->depends & (1u << _input_v0_rgb))
	{
		variables[count++] = _primary_variable_v0;
		variables[count++] = _primary_variable_v0_blue;
	}
	if (primary->depends & (1u << _input_v0_alpha))
		variables[count++] = _primary_variable_v0_alpha;
	if (primary->depends & (1u << _input_v1_rgb))
	{
		variables[count++] = _primary_variable_v1;
		variables[count++] = _primary_variable_v1_blue;
	}
	if (primary->depends & (1u << _input_v1_alpha))
		variables[count++] = _primary_variable_v1_alpha;
	if (primary->depends & (1u << _input_fog))
		variables[count++] = _primary_variable_fog;

	/* the colour at each corner: every variable at 0 or 1 */
	for (corner = 0; corner < 1 << count; corner++)
	{
		struct primary_inputs inputs;

		memset(&inputs, 0, sizeof(inputs));
		inputs.fog = fog_varies ? 0.0f : 1.0f;
		for (bit = 0; bit < count; bit++)
		{
			float value = (corner >> bit) & 1 ? 1.0f : 0.0f;

			switch (variables[bit])
			{
			case _primary_variable_v0: inputs.v0[0] = inputs.v0[1] = value; break;
			case _primary_variable_v0_blue: inputs.v0[2] = value; break;
			case _primary_variable_v0_alpha: inputs.v0[3] = value; break;
			case _primary_variable_v1: inputs.v1[0] = inputs.v1[1] = value; break;
			case _primary_variable_v1_blue: inputs.v1[2] = value; break;
			case _primary_variable_v1_alpha: inputs.v1[3] = value; break;
			default: inputs.fog = value; break;
			}
		}
		primary_evaluate(plan, pass_index, constants, &inputs, fixed, corners[corner]);
	}
	/* the coefficients: each corner less the corners below it */
	for (bit = 0; bit < count; bit++)
	{
		for (corner = 0; corner < 1 << count; corner++)
		{
			if (corner & (1 << bit))
			{
				for (channel = 0; channel < 4; channel++)
					corners[corner][channel] -= corners[corner ^ (1 << bit)][channel];
			}
		}
	}
	for (channel = 0; channel < 4; channel++)
	{
		primary->term_count[channel] = 0;
		for (corner = 0; corner < 1 << count; corner++)
		{
			struct primary_term *term;
			unsigned mask = 0;

			if (corners[corner][channel] > -1.0e-5f && corners[corner][channel] < 1.0e-5f)
				continue;
			if (primary->term_count[channel] == PRIMARY_MAXIMUM_TERMS)
				return FALSE;
			for (bit = 0; bit < count; bit++)
			{
				if (corner & (1 << bit))
					mask |= 1u << variables[bit];
			}
			/* (blue and alpha read a colour's blue as their own channel) */
			if (channel >= 2)
			{
				if (mask & (1u << _primary_variable_v0))
					mask = (mask & ~(1u << _primary_variable_v0)) | (1u << _primary_variable_v0_blue);
				if (mask & (1u << _primary_variable_v1))
					mask = (mask & ~(1u << _primary_variable_v1)) | (1u << _primary_variable_v1_blue);
			}
			term = &primary->terms[channel][primary->term_count[channel]++];
			term->variables = (unsigned char)mask;
			term->coefficient = corners[corner][channel];
		}
	}
	/* inside the range */
	for (point = 0; point < (int)(sizeof(points) / sizeof(points[0])); point++)
	{
		struct primary_inputs inputs = points[point];
		float color[4], values[NUMBER_OF_PRIMARY_VARIABLES];

		if (!fog_varies)
			inputs.fog = 1.0f;
		primary_evaluate(plan, pass_index, constants, &inputs, fixed, color);
		for (channel = 0; channel < 4; channel++)
		{
			float wanted = color[channel] < 0.0f ? 0.0f : color[channel] > 1.0f ? 1.0f : color[channel];
			float fitted;

			primary_variables(channel, inputs.v0, inputs.v1, inputs.fog, values);
			fitted = primary_polynomial(primary, channel, values);
			fitted = fitted < 0.0f ? 0.0f : fitted > 1.0f ? 1.0f : fitted;
			if (!close_enough(fitted, wanted))
				return FALSE;
		}
	}
	return TRUE;
}

/* ---------- a primary colour compiled for the vertices

A plan's expression is a sum of products of atoms (nv2a_combiner_ff.h): a
vertex colour's channels, the fog factor, a constant, or an operation (a
clamp, a dot product, a choice) on expressions. nv2a_ff_plan_primary walks
the tree at every vertex, finding each atom's source as it goes; here the
tree is walked once, when the primary is analysed, into steps over slots
of three floats. What depends on no vertex (constants, and the colours
that are the same at every vertex of the draw) is computed then; a vertex
costs the steps that read its colours. The arithmetic is
nv2a_ff_plan_primary's, in its order, so the colours are the same to the
bit. */

enum
{
	/* the slots a vertex fills: a colour's channels as the atoms read
	them (its blue and alpha in every channel) */
	_slot_v0_rgb,
	_slot_v0_alpha,
	_slot_v0_blue,
	_slot_v1_rgb,
	_slot_v1_alpha,
	_slot_v1_blue,
	_slot_fog_factor,
	PRIMARY_VERTEX_SLOTS,

	PRIMARY_SLOTS = 96,
	PRIMARY_MAXIMUM_STEPS = 48,
	PRIMARY_MAXIMUM_PRODUCTS = 96,

	_step_expression,
	_step_operation
};

struct primary_product
{
	float coefficient[3];
	unsigned char count;
	unsigned char slots[NV2A_FF_TERM_ATOMS];
};

struct primary_step
{
	unsigned char kind;
	unsigned char dst;
	/* _step_expression: the sum of these products */
	unsigned short first_product, product_count;
	/* _step_operation: operation(a, b, c) */
	unsigned char operation;
	unsigned char a, b, c;
};

struct primary_program
{
	/* the slots the result is read from */
	unsigned char rgb, alpha;
	/* the vertex slots the steps read (a bit each) */
	unsigned vertex_slots;
	unsigned short step_count, product_count, slot_count;
	struct primary_step steps[PRIMARY_MAXIMUM_STEPS];
	struct primary_product products[PRIMARY_MAXIMUM_PRODUCTS];
	/* the constants in place; the rest written as the steps run */
	float slots[PRIMARY_SLOTS][3];
	unsigned char slot_is_constant[PRIMARY_SLOTS];
	/* while compiling: the slot each expression came out in, or -1 */
	short expression_slots[NV2A_FF_MAXIMUM_EXPRESSIONS];
};

/* the operations' arithmetic (nv2a_combiner_ff.c, evaluate_atom) */
static float program_clamp_unit(float value)
{
	return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

static float program_clamp_signed(float value)
{
	return value < -1.0f ? -1.0f : value > 1.0f ? 1.0f : value;
}

static float program_signed_byte(float value)
{
	float byte = (float)floor(value * 255.0f + 0.5f);

	return (byte >= 128.0f ? byte - 256.0f : byte) / 127.0f;
}

/* whether an expression reads a vertex's colours or fog factor (those of
fixed, which may be NULL, are constants) */
static BOOL expression_varies(const struct nv2a_ff_plan *plan, short expression, const struct primary_fixed *fixed)
{
	const struct nv2a_ff_expression *entry;
	unsigned fixed_mask = fixed ? fixed->mask : 0;
	int index, atom;

	if (expression < 0 || expression >= plan->expression_count)
		return FALSE;
	entry = &plan->expressions[expression];
	for (index = 0; index < entry->term_count; index++)
	{
		const struct nv2a_ff_term *term = &plan->terms[entry->first_term + index];

		for (atom = 0; atom < term->count; atom++)
		{
			int which = term->atoms[atom];

			if (which < NV2A_FF_LEAF_ATOM_COUNT)
			{
				int source = which / 3, channel = which % 3;

				if (source == _nv2a_ff_atom_v0 || source == _nv2a_ff_atom_v1)
				{
					unsigned rgb = 1u << (source == _nv2a_ff_atom_v0 ? _input_v0_rgb : _input_v1_rgb);
					unsigned alpha = 1u << (source == _nv2a_ff_atom_v0 ? _input_v0_alpha : _input_v1_alpha);

					if (!(fixed_mask & (channel == _nv2a_ff_channel_alpha ? alpha : rgb)))
						return TRUE;
				}
				else if (source == _nv2a_ff_atom_fog && channel == _nv2a_ff_channel_alpha &&
					!(fixed_mask & (1u << _input_fog)))
				{
					return TRUE;
				}
			}
			else if (which - NV2A_FF_LEAF_ATOM_COUNT < plan->operation_count)
			{
				const struct nv2a_ff_operation *operation = &plan->operations[which - NV2A_FF_LEAF_ATOM_COUNT];
				int operand;

				for (operand = 0; operand < 3; operand++)
				{
					if (expression_varies(plan, operation->operands[operand], fixed))
						return TRUE;
				}
			}
		}
	}
	return FALSE;
}

/* an operation on slots (nv2a_combiner_ff.c, evaluate_atom) */
static void program_operation(int operation, const float *a, const float *b, const float *c, float *dst)
{
	int channel;

	switch (operation)
	{
	case _nv2a_ff_operation_maximum_zero:
		for (channel = 0; channel < 3; channel++)
			dst[channel] = a[channel] > 0.0f ? a[channel] : 0.0f;
		break;
	case _nv2a_ff_operation_clamp_unit:
		for (channel = 0; channel < 3; channel++)
			dst[channel] = program_clamp_unit(a[channel]);
		break;
	case _nv2a_ff_operation_clamp_signed:
		for (channel = 0; channel < 3; channel++)
			dst[channel] = program_clamp_signed(a[channel]);
		break;
	case _nv2a_ff_operation_dot:
		dst[0] = dst[1] = dst[2] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		break;
	case _nv2a_ff_operation_mux_msb:
		memcpy(dst, a[0] >= 0.5f ? b : c, 3 * sizeof(float));
		break;
	case _nv2a_ff_operation_mux_lsb:
		memcpy(dst, ((int)(a[0] * 255.0f + 0.5f) & 1) != 0 ? b : c, 3 * sizeof(float));
		break;
	case _nv2a_ff_operation_signed_byte:
		for (channel = 0; channel < 3; channel++)
			dst[channel] = program_signed_byte(a[channel]);
		break;
	default:
		memcpy(dst, a, 3 * sizeof(float));
		break;
	}
}

/* a constant's slot; -1 when the program is full */
static int program_constant(struct primary_program *program, const float *value)
{
	int slot;

	for (slot = PRIMARY_VERTEX_SLOTS; slot < program->slot_count; slot++)
	{
		if (program->slot_is_constant[slot] && !memcmp(program->slots[slot], value, 3 * sizeof(float)))
			return slot;
	}
	if (program->slot_count >= PRIMARY_SLOTS)
		return -1;
	memcpy(program->slots[program->slot_count], value, 3 * sizeof(float));
	program->slot_is_constant[program->slot_count] = TRUE;
	return program->slot_count++;
}

static int program_scratch(struct primary_program *program)
{
	return program->slot_count < PRIMARY_SLOTS ? program->slot_count++ : -1;
}

/* the value of an expression that reads no vertex */
static void program_evaluate_constant(const struct nv2a_ff_plan *plan, short expression,
	const struct nv2a_ff_constants *constants, const struct primary_fixed *fixed, float value[3])
{
	struct nv2a_ff_values values;

	memset(&values, 0, sizeof(values));
	values.constants = constants;
	values.fog_factor = 1.0f;
	if (fixed)
	{
		values.v0 = fixed->v0;
		values.v1 = fixed->v1;
		values.fog_factor = fixed->fog;
	}
	nv2a_ff_plan_evaluate(plan, expression, &values, value);
}

/* the slot of a leaf atom: a vertex slot, or a constant (a fixed colour is
one); -1 when the program is full */
static int program_leaf(struct primary_program *program, int atom, const struct nv2a_ff_constants *constants,
	const struct primary_fixed *fixed)
{
	int source = atom / 3, channel = atom % 3;
	const float *color = NULL;
	float value[3];
	int vertex_slot = -1;

	if (source == _nv2a_ff_atom_v0 || source == _nv2a_ff_atom_v1)
	{
		BOOL first = source == _nv2a_ff_atom_v0;
		unsigned wanted = 1u << (channel == _nv2a_ff_channel_alpha ? (first ? _input_v0_alpha : _input_v1_alpha) :
			(first ? _input_v0_rgb : _input_v1_rgb));

		if (fixed && (fixed->mask & wanted))
			color = first ? fixed->v0 : fixed->v1;
		else
			vertex_slot = (first ? _slot_v0_rgb : _slot_v1_rgb) + channel;
	}
	else if (source == _nv2a_ff_atom_fog)
	{
		if (channel == _nv2a_ff_channel_alpha)
		{
			if (fixed && (fixed->mask & (1u << _input_fog)))
			{
				value[0] = value[1] = value[2] = fixed->fog;
				return program_constant(program, value);
			}
			vertex_slot = _slot_fog_factor;
		}
		else
			color = constants->fog_color;
	}
	else if (source >= _nv2a_ff_atom_c0 && source < _nv2a_ff_atom_c0 + 8)
		color = constants->c0[source - _nv2a_ff_atom_c0];
	else if (source >= _nv2a_ff_atom_c1 && source < _nv2a_ff_atom_c1 + 8)
		color = constants->c1[source - _nv2a_ff_atom_c1];
	else if (source == _nv2a_ff_atom_final_c0)
		color = constants->final_c0;
	else if (source == _nv2a_ff_atom_final_c1)
		color = constants->final_c1;
	else if (source == _nv2a_ff_atom_blend_color)
		color = constants->blend_color;
	else
	{
		/* a texel: never in a primary colour */
		return -1;
	}
	if (vertex_slot >= 0)
	{
		program->vertex_slots |= 1u << vertex_slot;
		return vertex_slot;
	}
	if (channel == _nv2a_ff_channel_rgb)
		memcpy(value, color, sizeof(value));
	else
		value[0] = value[1] = value[2] = color[channel == _nv2a_ff_channel_alpha ? 3 : 2];
	return program_constant(program, value);
}

/* The slot an expression's value is in once the steps have run; -1 when
the program cannot hold it. The steps of what the expression reads are
added before its own. */
static int program_expression(struct primary_program *program, const struct nv2a_ff_plan *plan, short expression,
	const struct nv2a_ff_constants *constants, const struct primary_fixed *fixed)
{
	const struct nv2a_ff_expression *entry;
	struct primary_step *step;
	int first_product, index, atom, slot;

	if (expression < 0 || expression >= plan->expression_count)
	{
		static const float one[3] = { 1.0f, 1.0f, 1.0f };

		return program_constant(program, one);
	}
	if (program->expression_slots[expression] >= 0)
		return program->expression_slots[expression];
	if (!expression_varies(plan, expression, fixed))
	{
		float value[3];

		program_evaluate_constant(plan, expression, constants, fixed, value);
		slot = program_constant(program, value);
		program->expression_slots[expression] = (short)slot;
		return slot;
	}
	entry = &plan->expressions[expression];
	if (program->product_count + entry->term_count > PRIMARY_MAXIMUM_PRODUCTS)
		return -1;
	first_product = program->product_count;
	program->product_count += entry->term_count;
	for (index = 0; index < entry->term_count; index++)
	{
		const struct nv2a_ff_term *term = &plan->terms[entry->first_term + index];
		struct primary_product *product = &program->products[first_product + index];

		memcpy(product->coefficient, term->coefficient, sizeof(product->coefficient));
		product->count = term->count;
		for (atom = 0; atom < term->count && atom < NV2A_FF_TERM_ATOMS; atom++)
		{
			int which = term->atoms[atom];

			if (which < NV2A_FF_LEAF_ATOM_COUNT)
			{
				slot = program_leaf(program, which, constants, fixed);
			}
			else if (which - NV2A_FF_LEAF_ATOM_COUNT < plan->operation_count)
			{
				const struct nv2a_ff_operation *operation = &plan->operations[which - NV2A_FF_LEAF_ATOM_COUNT];
				struct primary_step *operation_step;
				int operands[3], operand;
				BOOL on_constants = TRUE;

				for (operand = 0; operand < 3; operand++)
				{
					/* (the operands the operation reads: the others may be anything) */
					BOOL read = operand == 0 || (operation->operation == _nv2a_ff_operation_dot && operand == 1) ||
						operation->operation == _nv2a_ff_operation_mux_msb ||
						operation->operation == _nv2a_ff_operation_mux_lsb;

					operands[operand] = read ?
						program_expression(program, plan, operation->operands[operand], constants, fixed) : 0;
					if (operands[operand] < 0)
						return -1;
					if (read && !program->slot_is_constant[operands[operand]])
						on_constants = FALSE;
				}
				if (on_constants)
				{
					/* (on constants: now) */
					float value[3];

					program_operation(operation->operation, program->slots[operands[0]], program->slots[operands[1]],
						program->slots[operands[2]], value);
					slot = program_constant(program, value);
					if (slot < 0)
						return -1;
					product->slots[atom] = (unsigned char)slot;
					continue;
				}
				slot = program_scratch(program);
				if (slot < 0 || program->step_count >= PRIMARY_MAXIMUM_STEPS)
					return -1;
				operation_step = &program->steps[program->step_count++];
				operation_step->kind = _step_operation;
				operation_step->operation = operation->operation;
				operation_step->dst = (unsigned char)slot;
				operation_step->a = (unsigned char)operands[0];
				operation_step->b = (unsigned char)operands[1];
				operation_step->c = (unsigned char)operands[2];
			}
			else
				return -1;
			if (slot < 0)
				return -1;
			product->slots[atom] = (unsigned char)slot;
		}
	}
	slot = program_scratch(program);
	if (slot < 0 || program->step_count >= PRIMARY_MAXIMUM_STEPS)
		return -1;
	step = &program->steps[program->step_count++];
	step->kind = _step_expression;
	step->dst = (unsigned char)slot;
	step->first_product = (unsigned short)first_product;
	step->product_count = (unsigned short)entry->term_count;
	program->expression_slots[expression] = (short)slot;
	return slot;
}

/* a pass's primary colour as steps; NULL when it cannot be (too large, or
out of memory: nv2a_ff_plan_primary then) */
static struct primary_program *primary_program_compile(const struct nv2a_ff_plan *plan, int pass_index,
	const struct nv2a_ff_constants *constants, const struct primary_fixed *fixed)
{
	const struct nv2a_ff_pass *pass = &plan->passes[pass_index];
	struct primary_program *program = calloc(1, sizeof(*program));
	int index, rgb, alpha;

	if (!program)
		return NULL;
	program->slot_count = PRIMARY_VERTEX_SLOTS;
	for (index = 0; index < NV2A_FF_MAXIMUM_EXPRESSIONS; index++)
		program->expression_slots[index] = -1;
	rgb = program_expression(program, plan, pass->primary_rgb, constants, fixed);
	alpha = program_expression(program, plan, pass->primary_alpha, constants, fixed);
	if (rgb < 0 || alpha < 0)
	{
		free(program);
		return NULL;
	}
	program->rgb = (unsigned char)rgb;
	program->alpha = (unsigned char)alpha;
	return program;
}

/* the primary colour at a vertex, as nv2a_ff_plan_primary computes it */
static void primary_program_run(struct primary_program *program, const float *v0, const float *v1, float fog,
	float color[4])
{
	float (*slots)[3] = program->slots;
	const struct primary_step *step = program->steps, *end = step + program->step_count;
	unsigned vertex_slots = program->vertex_slots;
	int channel;

	if (vertex_slots & (1u << _slot_v0_rgb))
		memcpy(slots[_slot_v0_rgb], v0, 3 * sizeof(float));
	if (vertex_slots & (1u << _slot_v0_alpha))
		slots[_slot_v0_alpha][0] = slots[_slot_v0_alpha][1] = slots[_slot_v0_alpha][2] = v0[3];
	if (vertex_slots & (1u << _slot_v0_blue))
		slots[_slot_v0_blue][0] = slots[_slot_v0_blue][1] = slots[_slot_v0_blue][2] = v0[2];
	if (vertex_slots & (1u << _slot_v1_rgb))
		memcpy(slots[_slot_v1_rgb], v1, 3 * sizeof(float));
	if (vertex_slots & (1u << _slot_v1_alpha))
		slots[_slot_v1_alpha][0] = slots[_slot_v1_alpha][1] = slots[_slot_v1_alpha][2] = v1[3];
	if (vertex_slots & (1u << _slot_v1_blue))
		slots[_slot_v1_blue][0] = slots[_slot_v1_blue][1] = slots[_slot_v1_blue][2] = v1[2];
	if (vertex_slots & (1u << _slot_fog_factor))
		slots[_slot_fog_factor][0] = slots[_slot_fog_factor][1] = slots[_slot_fog_factor][2] = fog;
	for (; step < end; step++)
	{
		float *dst = slots[step->dst];

		if (step->kind == _step_expression)
		{
			const struct primary_product *product = &program->products[step->first_product];
			int count = step->product_count;
			float sum[3] = { 0.0f, 0.0f, 0.0f };

			for (; count; count--, product++)
			{
				float value[3];
				int atom;

				memcpy(value, product->coefficient, sizeof(value));
				for (atom = 0; atom < product->count; atom++)
				{
					const float *factor = slots[product->slots[atom]];

					value[0] *= factor[0];
					value[1] *= factor[1];
					value[2] *= factor[2];
				}
				sum[0] += value[0];
				sum[1] += value[1];
				sum[2] += value[2];
			}
			memcpy(dst, sum, sizeof(sum));
			continue;
		}
		program_operation(step->operation, slots[step->a], slots[step->b], slots[step->c], dst);
	}
	for (channel = 0; channel < 3; channel++)
		color[channel] = program_clamp_unit(slots[program->rgb][channel]);
	color[3] = program_clamp_unit(slots[program->alpha][0]);
}

/* a primary analysed again, or dropped with its variant */
static void primary_clear(struct primary *primary)
{
	free(primary->program);
	memset(primary, 0, sizeof(*primary));
}

/* a pass's primary colour: what kind it is, and how it is computed */
static void primary_analyze(const struct nv2a_ff_plan *plan, int pass_index, const struct nv2a_ff_state *ff_state,
	const struct nv2a_ff_constants *constants, const struct primary_fixed *fixed, struct primary *primary)
{
	primary_clear(primary);
	primary_analyze_output(plan, pass_index, ff_state, constants, fixed, primary);
	if (primary->kind == _primary_general)
	{
		primary->polynomial = primary_fit_polynomial(plan, pass_index, ff_state, constants, fixed, primary);
		if (!primary->polynomial)
			primary->program = primary_program_compile(plan, pass_index, constants, fixed);
	}
}

/* the primaries of a variant that is dropped or made again */
static void variant_clear_primaries(struct plan_variant *variant)
{
	int pass;

	for (pass = 0; pass < NV2A_FF_MAXIMUM_PASSES; pass++)
	{
		primary_clear(&variant->primaries[pass]);
		if (variant->fixed_primaries)
			primary_clear(&variant->fixed_primaries[pass]);
	}
}

static void plan_analyze(struct plan_variant *variant, const struct nv2a_ff_state *ff_state,
	const struct nv2a_ff_constants *constants)
{
	int pass;

	if (variant->fallback)
		return;
	if (variant->analyzed && !memcmp(&variant->analyzed_constants, constants, sizeof(*constants)))
		return;
	variant->analyzed = TRUE;
	variant->analyzed_constants = *constants;
	for (pass = 0; pass < variant->plan.pass_count; pass++)
		primary_analyze(&variant->plan, pass, ff_state, constants, NULL, &variant->primaries[pass]);
}

/* the inputs any of the plan's primaries (as plan_analyze left them)
changes with */
static unsigned plan_depends(const struct plan_variant *variant)
{
	unsigned depends = 0;
	int pass;

	for (pass = 0; pass < variant->plan.pass_count; pass++)
		depends |= variant->primaries[pass].depends;
	return depends;
}

/* the primaries again, with the inputs of fixed at their values; FALSE
without memory for them */
static BOOL plan_analyze_fixed(struct plan_variant *variant, const struct nv2a_ff_state *ff_state,
	const struct nv2a_ff_constants *constants, const struct primary_fixed *fixed)
{
	int pass;

	if (!variant->fixed_primaries)
	{
		variant->fixed_primaries = calloc(NV2A_FF_MAXIMUM_PASSES, sizeof(*variant->fixed_primaries));
		if (!variant->fixed_primaries)
			return FALSE;
	}
	if (variant->fixed_analyzed && !memcmp(&variant->fixed, fixed, sizeof(*fixed)) &&
		!memcmp(&variant->fixed_constants, constants, sizeof(*constants)))
	{
		return TRUE;
	}
	variant->fixed_analyzed = TRUE;
	variant->fixed_constants = *constants;
	variant->fixed = *fixed;
	for (pass = 0; pass < variant->plan.pass_count; pass++)
		primary_analyze(&variant->plan, pass, ff_state, constants, fixed, &variant->fixed_primaries[pass]);
	return TRUE;
}

/* ---------- vertices */

/* the arrays the evaluator fills and OpenGL reads; they only grow */
static struct
{
	float *position;
	float *coordinates[NV2A_FF_STAGE_COUNT];
	/* the primary colours the evaluator writes, one for each distinct
	primary of the plan's passes; color is the one computed here */
	unsigned char *colors[NV2A_FF_MAXIMUM_PASSES];
	unsigned char *color;
	unsigned char *v0, *v1;
	float *fog;
	unsigned long position_size, coordinates_size[NV2A_FF_STAGE_COUNT];
	unsigned long colors_size[NV2A_FF_MAXIMUM_PASSES];
	unsigned long color_size, v0_size, v1_size, fog_size;
} arrays;

/* room for count elements of size bytes, and a little past them */
static BOOL array_reserve(void *array_pointer, unsigned long *size, unsigned long count, unsigned long element)
{
	void **array = array_pointer;
	unsigned long wanted = (count + 16) * element;

	if (*size < wanted)
	{
		void *grown;

		wanted += wanted / 2;
		grown = realloc(*array, wanted);
		if (!grown)
			return FALSE;
		*array = grown;
		*size = wanted;
	}
	return TRUE;
}

/* the vertices a draw uses: first to first + count - 1 of the streams,
whose results go to elements base and up of the arrays */
struct vertex_range
{
	unsigned long first, count;
	unsigned long base;
};

/* a colour array the evaluator writes */
struct evaluation_color
{
	unsigned char *array;
	unsigned char color_source, alpha_source, alpha_component;
	const float *color_scale;
};

/* Everything one run of the program over a draw's vertices is to write:
the passes' arrays all at once, since a run costs the same whatever it
writes and the program's arithmetic is most of it. */
struct evaluation
{
	BOOL position;
	/* NV2A stages whose texture coordinates are wanted, or -1 */
	int stages[NV2A_VSH_CPU_TEXCOORDS];
	struct evaluation_color colors[NV2A_VSH_CPU_COLORS];
	int color_count;
	BOOL fog;
};

static void evaluation_clear(struct evaluation *evaluation)
{
	int index;

	memset(evaluation, 0, sizeof(*evaluation));
	for (index = 0; index < NV2A_VSH_CPU_TEXCOORDS; index++)
		evaluation->stages[index] = -1;
}

/* adds a stage's coordinates to a run (at most NV2A_VSH_CPU_TEXCOORDS,
which is the number of stages) */
static void evaluation_stage(struct evaluation *evaluation, int stage)
{
	int index;

	for (index = 0; index < NV2A_VSH_CPU_TEXCOORDS; index++)
	{
		if (evaluation->stages[index] == stage)
			return;
		if (evaluation->stages[index] < 0)
		{
			evaluation->stages[index] = stage;
			return;
		}
	}
}

/* adds a colour array to a run; NULL when the run has no room for another */
static struct evaluation_color *evaluation_add_color(struct evaluation *evaluation, unsigned char *array)
{
	struct evaluation_color *color;

	if (evaluation->color_count >= NV2A_VSH_CPU_COLORS)
		return NULL;
	color = &evaluation->colors[evaluation->color_count++];
	color->array = array;
	return color;
}

/* a pass's primary colour as the evaluator writes it (_primary_output) */
static void evaluation_color(struct evaluation *evaluation, const struct primary *primary, unsigned char *array)
{
	struct evaluation_color *color = evaluation_add_color(evaluation, array);

	if (!color)
		return;
	color->color_source = primary->color_source;
	color->alpha_source = primary->alpha_source;
	color->alpha_component = primary->alpha_component;
	/* (with a bias the scale is applied with it, afterwards) */
	color->color_scale = primary->scaled && !primary->biased ? primary->scale : NULL;
}

/* one of the vertex program's colours as it is, for the plan's expressions */
static void evaluation_vertex_color(struct evaluation *evaluation, unsigned char *array, unsigned char source)
{
	struct evaluation_color *color = evaluation_add_color(evaluation, array);

	if (!color)
		return;
	color->color_source = color->alpha_source = source;
	color->alpha_component = 3;
	color->color_scale = NULL;
}

/* whether a program writes the q of a stage's texture coordinates */
static BOOL stage_projective(const struct nv2a_vsh_classification *classification, int stage)
{
	return (classification->outputs[_nv2a_vsh_output_texcoord0 + stage].written & 1) != 0;
}

static BOOL evaluate(const struct xgpu_fixed_draw *draw, struct nv2a_vsh_cpu_program *program,
	const struct vertex_range *range, const struct evaluation *evaluation)
{
	const struct nv2a_vsh_classification *classification = nv2a_vsh_cpu_classification(program);
	struct nv2a_vsh_cpu_draw run;
	struct float_state saved;
	unsigned long index;
	BOOL success;

	memset(&run, 0, sizeof(run));
	run.constants = draw->constants;
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		memcpy(run.inputs[index].value, draw->attributes[index], sizeof(run.inputs[index].value));
		run.inputs[index].type = D3DVSDT_FLOAT4;
	}
	for (index = 0; index < draw->element_count; index++)
	{
		const struct xgpu_fixed_element *element = &draw->elements[index];

		run.inputs[element->reg].data = element->data;
		run.inputs[element->reg].stride = element->stride;
		run.inputs[element->reg].type = element->type;
	}
	memcpy(run.viewport_scale, draw->viewport_scale, sizeof(run.viewport_scale));
	memcpy(run.viewport_offset, draw->viewport_offset, sizeof(run.viewport_offset));
	run.screen_offset = draw->screen_offset;
	/* always the same, for every program: see the top of the file */
	run.flags = NV2A_VSH_CPU_POSITION_CAPTURED | NV2A_VSH_CPU_SIGNED_DEPTH;
	if (evaluation->position)
	{
		run.position.data = arrays.position + range->base * 4;
		run.position.stride = 4 * sizeof(float);
	}
	for (index = 0; index < NV2A_VSH_CPU_TEXCOORDS; index++)
	{
		int stage = evaluation->stages[index];

		run.texcoords[index].source = NV2A_VSH_OUTPUT_NONE;
		if (stage >= 0)
		{
			run.texcoords[index].array.data = arrays.coordinates[stage] + range->base * 4;
			run.texcoords[index].array.stride = 4 * sizeof(float);
			run.texcoords[index].source = (unsigned char)(_nv2a_vsh_output_texcoord0 + stage);
			run.texcoords[index].components = stage_projective(classification, stage) ? 4 : 2;
		}
	}
	for (index = 0; index < (unsigned long)evaluation->color_count && index < NV2A_VSH_CPU_COLORS; index++)
	{
		const struct evaluation_color *color = &evaluation->colors[index];

		run.colors[index].array.data = color->array + range->base * 4;
		run.colors[index].array.stride = 4;
		run.colors[index].color_source = color->color_source;
		run.colors[index].alpha_source = color->alpha_source;
		run.colors[index].alpha_component = color->alpha_component;
		run.colors[index].color_scale = color->color_scale;
	}
	if (evaluation->fog)
	{
		run.fog.data = arrays.fog + range->base;
		run.fog.stride = sizeof(float);
	}
	float_state_save(&saved);
	if (stats_timed)
	{
		struct timespec start, end;

		clock_gettime(CLOCK_MONOTONIC, &start);
		success = nv2a_vsh_cpu_run(program, &run, range->first, range->count);
		clock_gettime(CLOCK_MONOTONIC, &end);
		stats.evaluator_time += microseconds_between(&start, &end);
		stats.evaluator_runs++;
		stats.evaluator_vertices += range->count;
	}
	else
	{
		success = nv2a_vsh_cpu_run(program, &run, range->first, range->count);
	}
	float_state_restore(&saved);
	return success;
}

/* whether an output's component is the same at every vertex of a draw
whose streams feed the registers of stream_registers (a bit each) */
static BOOL output_component_fixed(const struct nv2a_vsh_output_class *output, int component, unsigned stream_registers)
{
	return output->component_class[component] == _nv2a_vsh_class_unwritten ||
		output->component_class[component] == _nv2a_vsh_class_constant || !(output->inputs & stream_registers);
}

/* the _input_* (a bit each) that do not vary over a draw's vertices */
static unsigned fixed_inputs(const struct xgpu_fixed_draw *draw, const struct nv2a_vsh_classification *classification)
{
	const struct nv2a_vsh_output_class *diffuse = &classification->outputs[_nv2a_vsh_output_diffuse];
	const struct nv2a_vsh_output_class *specular = &classification->outputs[_nv2a_vsh_output_specular];
	unsigned stream_registers = 0, mask = 0;
	unsigned long index;

	for (index = 0; index < draw->element_count; index++)
		stream_registers |= 1u << draw->elements[index].reg;
	if (output_component_fixed(diffuse, 0, stream_registers) && output_component_fixed(diffuse, 1, stream_registers) &&
		output_component_fixed(diffuse, 2, stream_registers))
	{
		mask |= 1u << _input_v0_rgb;
	}
	if (output_component_fixed(diffuse, 3, stream_registers))
		mask |= 1u << _input_v0_alpha;
	if (output_component_fixed(specular, 0, stream_registers) && output_component_fixed(specular, 1, stream_registers) &&
		output_component_fixed(specular, 2, stream_registers))
	{
		mask |= 1u << _input_v1_rgb;
	}
	if (output_component_fixed(specular, 3, stream_registers))
		mask |= 1u << _input_v1_alpha;
	if (output_component_fixed(&classification->outputs[_nv2a_vsh_output_fog], 0, stream_registers))
		mask |= 1u << _input_fog;
	return mask;
}

/* The values of the inputs of fixed->mask: the program run for one of the
draw's vertices, its first or its last. arrays.fog has room for an element.
FALSE if the program cannot be run. */
static BOOL fixed_values_at(const struct xgpu_fixed_draw *draw, struct nv2a_vsh_cpu_program *program, BOOL last,
	struct primary_fixed *fixed, float *fog_coordinate)
{
	struct vertex_range one;
	struct evaluation evaluation;
	unsigned char v0[4], v1[4];
	BOOL wants_v0 = (fixed->mask & ((1u << _input_v0_rgb) | (1u << _input_v0_alpha))) != 0;
	BOOL wants_v1 = (fixed->mask & ((1u << _input_v1_rgb) | (1u << _input_v1_alpha))) != 0;
	int channel;

	one.first = draw->indices ? draw->indices[last ? draw->count - 1 : 0] : draw->first + (last ? draw->count - 1 : 0);
	one.count = 1;
	one.base = 0;
	memset(fixed->v0, 0, sizeof(fixed->v0));
	memset(fixed->v1, 0, sizeof(fixed->v1));
	fixed->fog = 1.0f;
	evaluation_clear(&evaluation);
	if (wants_v0)
		evaluation_vertex_color(&evaluation, v0, _nv2a_vsh_output_diffuse);
	if (wants_v1)
		evaluation_vertex_color(&evaluation, v1, _nv2a_vsh_output_specular);
	evaluation.fog = (fixed->mask & (1u << _input_fog)) != 0;
	if (!evaluate(draw, program, &one, &evaluation))
		return FALSE;
	for (channel = 0; channel < 4; channel++)
	{
		if (wants_v0)
			fixed->v0[channel] = (float)v0[channel] * (1.0f / 255.0f);
		if (wants_v1)
			fixed->v1[channel] = (float)v1[channel] * (1.0f / 255.0f);
	}
	*fog_coordinate = evaluation.fog ? arrays.fog[0] : 1.0f;
	/* (the values of the others are not read: zeroes, for the comparison
	with the last draw's) */
	if (!(fixed->mask & (1u << _input_v0_rgb)))
		memset(fixed->v0, 0, 3 * sizeof(float));
	if (!(fixed->mask & (1u << _input_v0_alpha)))
		fixed->v0[3] = 0.0f;
	if (!(fixed->mask & (1u << _input_v1_rgb)))
		memset(fixed->v1, 0, 3 * sizeof(float));
	if (!(fixed->mask & (1u << _input_v1_alpha)))
		fixed->v1[3] = 0.0f;
	return TRUE;
}

/* a program's first draws with fixed inputs are checked: so many of them */
#define FIXED_CHECKED_DRAWS 32

static GLenum primitive_mode(D3DPRIMITIVETYPE type)
{
	switch (type)
	{
	case D3DPT_POINTLIST: return GL_POINTS;
	case D3DPT_LINELIST: return GL_LINES;
	case D3DPT_LINELOOP: return GL_LINE_LOOP;
	case D3DPT_LINESTRIP: return GL_LINE_STRIP;
	case D3DPT_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
	case D3DPT_TRIANGLEFAN: return GL_TRIANGLE_FAN;
	/* OpenGL 1.1 has the Xbox's quads and polygons */
	case D3DPT_QUADLIST: return GL_QUADS;
	case D3DPT_QUADSTRIP: return GL_QUAD_STRIP;
	case D3DPT_POLYGON: return GL_POLYGON;
	default: return GL_TRIANGLES;
	}
}

/* ---------- the lifted path: OpenGL transforms the vertices

The position of every vertex program of the game that is not skinned is
four constant rows times one input register (the world's matrix, or the
screen's), followed by the screen-space conversion that the evaluator
undoes again (nv2a_vsh_cpu.h, position_rows). OpenGL 1.1's own transform
does the same: GL_PROJECTION is set to the rows composed with the
conversion, the matrix the evaluator's arithmetic amounts to, and the
vertex array points into the game's own stream. Texture coordinates that
are an affine function of one input register (the surface's coordinates
times a scale, the lightmap's coordinates as they are, coordinates made
from the position) go the same way, through GL_TEXTURE. The evaluator then
runs, without the position, only for what is left: colours that vary from
vertex to vertex, coordinates that are not affine; for the environment's
lit surfaces that is nothing at all.

The path is chosen for a program, never for a draw: the game draws the
same vertices again through another program and tests for equal depth,
and the two paths round differently. The programs with such positions are
the environment's, the screen's and the particles'; the models' are
skinned and go through the evaluator. */

struct fixed_cache;

/* so many constants read by an output are remembered with its matrix */
#define LIFTED_CONSTANTS 16

/* an output's affine matrix, kept with the constants it was made from */
struct lifted_output
{
	/* the constants the output reads (hardware numbering), or more than
	LIFTED_CONSTANTS of them: the matrix is then made for every draw */
	unsigned long constant_count;
	unsigned char constants[LIFTED_CONSTANTS];
	float values[LIFTED_CONSTANTS][4];
	BOOL valid;
	float matrix[4][5];
};

/* what the light renderer keeps for a vertex program (xgpu_fixed_draw.program) */
struct fixed_program
{
	struct nv2a_vsh_cpu_program *program;
	/* the position goes through GL_PROJECTION */
	BOOL lift;
	/* its matrix, when it is affine otherwise than as four plain rows
	(position_rows_valid: the rows are read as they are) */
	struct lifted_output position;
	struct lifted_output texcoords[NV2A_FF_STAGE_COUNT];
	/* the colours and fog that are the same at every vertex, as last
	computed (fixed_values; NULL until then) */
	struct fixed_cache *fixed_cache;
};

static void lifted_output_prepare(struct lifted_output *output, const struct nv2a_vsh_output_class *class)
{
	unsigned long index;

	output->constant_count = 0;
	for (index = 0; index < XGPU_VERTEX_CONSTANT_COUNT; index++)
	{
		if (class->constants[index / 32] & (1UL << (index % 32)))
		{
			if (output->constant_count < LIFTED_CONSTANTS)
				output->constants[output->constant_count] = (unsigned char)index;
			output->constant_count++;
		}
	}
	/* (constants read relative to a0 are not among them: made for every draw) */
	if (class->relative_constants)
		output->constant_count = LIFTED_CONSTANTS + 1;
}

static struct fixed_program *fixed_program_new(const struct xgpu_fixed_draw *draw)
{
	struct fixed_program *fixed = calloc(1, sizeof(*fixed));
	const struct nv2a_vsh_classification *classification;
	int stage;

	if (!fixed)
		return NULL;
	fixed->program = nv2a_vsh_cpu_program_new(draw->instructions, draw->instruction_count);
	if (!fixed->program)
	{
		free(fixed);
		return NULL;
	}
	classification = nv2a_vsh_cpu_classification(fixed->program);
	fixed->lift = classification->epilogue && classification->position_class == _nv2a_vsh_class_affine &&
		classification->position_input < XGPU_VERTEX_ATTRIBUTE_COUNT;
	lifted_output_prepare(&fixed->position, &classification->outputs[_nv2a_vsh_output_position]);
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
		lifted_output_prepare(&fixed->texcoords[stage], &classification->outputs[_nv2a_vsh_output_texcoord0 + stage]);
	return fixed;
}

/* an output's matrix for the draw's constants, made again when they have
changed; FALSE when the output is not affine */
static BOOL lifted_output_matrix(struct lifted_output *output, const struct nv2a_vsh_cpu_program *program,
	unsigned long address, const float (*constants)[4])
{
	unsigned long index;

	if (output->constant_count <= LIFTED_CONSTANTS)
	{
		BOOL same = output->valid;

		for (index = 0; same && index < output->constant_count; index++)
			same = !memcmp(output->values[index], constants[output->constants[index]], sizeof(output->values[index]));
		if (same)
			return TRUE;
		for (index = 0; index < output->constant_count; index++)
			memcpy(output->values[index], constants[output->constants[index]], sizeof(output->values[index]));
	}
	output->valid = nv2a_vsh_cpu_affine_output(program, address, constants, output->matrix);
	return output->valid;
}

/* the OpenGL type and size of a stream element OpenGL 1.1 can read as it
is (the evaluator reads these the same way: floats as they are, shorts as
whole numbers, normalized shorts over 32767); FALSE for the rest */
static BOOL element_gl_type(unsigned long type, GLenum *gl_type, GLint *size, BOOL *normalized)
{
	*normalized = FALSE;
	switch (type)
	{
	case D3DVSDT_FLOAT2: case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT4:
		*gl_type = GL_FLOAT;
		*size = (GLint)(type >> 4);
		return TRUE;
	case D3DVSDT_SHORT2: case D3DVSDT_SHORT4:
		*gl_type = GL_SHORT;
		*size = (GLint)(type >> 4);
		return TRUE;
	case D3DVSDT_NORMSHORT2: case D3DVSDT_NORMSHORT4:
		*gl_type = GL_SHORT;
		*size = (GLint)(type >> 4);
		*normalized = TRUE;
		return TRUE;
	default:
		return FALSE;
	}
}

/* An affine output's matrix (output = matrix[row][0..3] . input +
matrix[row][4]) as the 4x4 OpenGL applies to the array's element (its
missing components 0, and 1 for the fourth, as the evaluator fills them),
column major. FALSE when the element has four components and the matrix
a constant term, which the fourth column cannot carry. */
static BOOL affine_gl_matrix(const float matrix[4][5], GLint size, BOOL normalized, float *gl)
{
	int row, column;

	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 3; column++)
			gl[column * 4 + row] = column < size ? matrix[row][column] * (normalized ? 1.0f / 32767.0f : 1.0f) : 0.0f;
		if (size == 4)
		{
			if (matrix[row][4] != 0.0f)
				return FALSE;
			gl[12 + row] = matrix[row][3] * (normalized ? 1.0f / 32767.0f : 1.0f);
		}
		else
			gl[12 + row] = matrix[row][3] + matrix[row][4];
	}
	return TRUE;
}

/* The clip-space position of a lifted draw as four rows over the stream's
element (its missing components 0, and 1 for the fourth): the program's
own four constant rows, or the matrix of its affine position with its
constant term in the fourth column (then the element has at most three
components, or no constant term). FALSE when it cannot be had. */
static BOOL lifted_rows(struct fixed_program *fixed, const struct xgpu_fixed_draw *draw, GLint *size, double rows[4][4])
{
	const struct nv2a_vsh_classification *classification = nv2a_vsh_cpu_classification(fixed->program);
	const float (*constants)[4] = draw->constants;
	int row, column;

	if (classification->position_rows_valid)
	{
		/* (the rows applied with dph read the fourth component as 1) */
		if (*size > 3 && !classification->position_homogeneous)
			*size = 3;
		for (row = 0; row < 4; row++)
		{
			for (column = 0; column < 4; column++)
				rows[row][column] = constants[classification->position_rows[row]][column];
		}
		return TRUE;
	}
	if (!lifted_output_matrix(&fixed->position, fixed->program, _nv2a_vsh_output_position, constants))
		return FALSE;
	for (row = 0; row < 4; row++)
	{
		const float *matrix = fixed->position.matrix[row];

		if (*size > 3 && matrix[4] != 0.0f)
			return FALSE;
		for (column = 0; column < 3; column++)
			rows[row][column] = matrix[column];
		rows[row][3] = *size > 3 ? matrix[3] : (double)matrix[3] + matrix[4];
	}
	return TRUE;
}

/* GL_PROJECTION for a lifted draw: the position rows, then the evaluator's
arithmetic (nv2a_vsh_cpu.c, position_terms_prepare and store_vector with
the position captured and the depth signed), as one matrix in double
precision; column major. */
static void lifted_projection(const struct xgpu_fixed_draw *draw, const double rows[4][4], float *gl)
{
	const float (*constants)[4] = draw->constants;
	const float *multiplier = constants[XGPU_VERTEX_CONSTANT_BIAS - 38];
	const float *offset = constants[XGPU_VERTEX_CONSTANT_BIAS - 37];
	double epilogue[4][4], product[4][4];
	float bias[3];
	int row, column, index;

	bias[0] = 0.5f + draw->screen_offset;
	bias[1] = 0.5f;
	bias[2] = 0.0f;
	memset(epilogue, 0, sizeof(epilogue));
	for (index = 0; index < 3; index++)
	{
		float scale = draw->viewport_scale[index] != 0.0f ? draw->viewport_scale[index] : 1.0f;
		float reciprocal = 1.0f / scale;
		float addend = (offset[index] + bias[index]) - draw->viewport_offset[index];

		epilogue[index][index] = (double)multiplier[index] * reciprocal;
		epilogue[index][3] = (double)addend * reciprocal;
	}
	epilogue[3][3] = 1.0;
	/* depth from -w to w */
	for (column = 0; column < 4; column++)
		epilogue[2][column] *= 2.0;
	epilogue[2][3] -= 1.0;
	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 4; column++)
		{
			product[row][column] = 0.0;
			for (index = 0; index < 4; index++)
				product[row][column] += epilogue[row][index] * rows[index][column];
			gl[column * 4 + row] = (float)product[row][column];
		}
	}
}

/* the padding and addressing matrix of a unit's texture
(xgpu_fixed_texture.scale and offset) over the coordinates' own */
static void texture_matrix_compose(const struct xgpu_fixed_texture *texture, const float *coordinates, float *gl)
{
	int column;

	for (column = 0; column < 4; column++)
	{
		const float *in = coordinates + column * 4;
		float *out = gl + column * 4;

		out[0] = texture->scale[0] * in[0] + texture->offset[0] * in[3];
		out[1] = texture->scale[1] * in[1] + texture->offset[1] * in[3];
		out[2] = in[2];
		out[3] = in[3];
	}
}

/* a stage's coordinates as OpenGL reads them in a lifted draw */
struct lifted_coordinates
{
	BOOL lifted;
	const void *pointer;
	GLint size;
	GLenum type;
	GLsizei stride;
	/* the output's matrix as OpenGL applies it (before the texture's own) */
	float matrix[16];
};

/* Whether a stage's coordinates can come from a stream through
GL_TEXTURE: an affine function of one input register that a stream of a
type OpenGL reads feeds, or a constant (then the position's stream is
pointed at and the matrix reads none of it). The coordinates the program
does not write stay 0, 0, 0, 1 in both paths. */
static BOOL lifted_stage(struct fixed_program *fixed, const struct xgpu_fixed_draw *draw,
	const struct xgpu_fixed_element *const *elements, int stage, struct lifted_coordinates *coordinates)
{
	const struct nv2a_vsh_classification *classification = nv2a_vsh_cpu_classification(fixed->program);
	const struct nv2a_vsh_output_class *class = &classification->outputs[_nv2a_vsh_output_texcoord0 + stage];
	const struct xgpu_fixed_element *element;
	BOOL normalized;

	coordinates->lifted = FALSE;
	if (class->class == _nv2a_vsh_class_affine)
		element = class->input < XGPU_VERTEX_ATTRIBUTE_COUNT ? elements[class->input] : NULL;
	else if (class->class == _nv2a_vsh_class_constant || class->class == _nv2a_vsh_class_unwritten)
		element = elements[classification->position_input];
	else
		return FALSE;
	if (!element || !element_gl_type(element->type, &coordinates->type, &coordinates->size, &normalized))
		return FALSE;
	/* (a constant reads two components of the position's stream times 0) */
	if (class->class != _nv2a_vsh_class_affine)
		coordinates->size = 2;
	if (!lifted_output_matrix(&fixed->texcoords[stage], fixed->program, _nv2a_vsh_output_texcoord0 + stage, draw->constants))
		return FALSE;
	if (!affine_gl_matrix(fixed->texcoords[stage].matrix, coordinates->size, normalized, coordinates->matrix))
		return FALSE;
	coordinates->pointer = element->data;
	coordinates->stride = (GLsizei)element->stride;
	coordinates->lifted = TRUE;
	return TRUE;
}

/* so many constants and attributes a program's fixed values are remembered by */
#define FIXED_CACHE_INPUTS 32

/* a program's fixed values as they were last computed, with what they were
computed from: the constants the outputs read and the registers no stream
feeds (the HUD draws hundreds of quads with the same ones) */
struct fixed_cache
{
	BOOL valid;
	unsigned mask;
	unsigned short stream_registers;
	unsigned long input_count;
	float inputs[FIXED_CACHE_INPUTS][4];
	float v0[4], v1[4], fog_coordinate;
};

/* The constants and attributes the fixed values of mask are computed from;
FALSE when they are too many to remember, or not known (constants read
relative to a0). */
static BOOL fixed_cache_inputs(const struct xgpu_fixed_draw *draw, const struct nv2a_vsh_classification *classification,
	unsigned mask, unsigned short stream_registers, float (*inputs)[4], unsigned long *count)
{
	static const struct { unsigned bits; unsigned char output; } outputs[3] =
	{
		{ (1u << _input_v0_rgb) | (1u << _input_v0_alpha), _nv2a_vsh_output_diffuse },
		{ (1u << _input_v1_rgb) | (1u << _input_v1_alpha), _nv2a_vsh_output_specular },
		{ 1u << _input_fog, _nv2a_vsh_output_fog },
	};
	DWORD constants[XGPU_VERTEX_CONSTANT_COUNT / 32] = { 0 };
	unsigned short registers = 0;
	unsigned long index, word;

	for (index = 0; index < 3; index++)
	{
		const struct nv2a_vsh_output_class *output = &classification->outputs[outputs[index].output];

		if (!(mask & outputs[index].bits))
			continue;
		if (output->relative_constants)
			return FALSE;
		for (word = 0; word < XGPU_VERTEX_CONSTANT_COUNT / 32; word++)
			constants[word] |= output->constants[word];
		registers |= output->inputs;
	}
	registers &= (unsigned short)~stream_registers;
	*count = 0;
	for (index = 0; index < XGPU_VERTEX_CONSTANT_COUNT; index++)
	{
		if (!(constants[index / 32] & (1UL << (index % 32))))
			continue;
		if (*count >= FIXED_CACHE_INPUTS)
			return FALSE;
		memcpy(inputs[(*count)++], draw->constants[index], 4 * sizeof(float));
	}
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (!(registers & (1u << index)))
			continue;
		if (*count >= FIXED_CACHE_INPUTS)
			return FALSE;
		memcpy(inputs[(*count)++], draw->attributes[index], 4 * sizeof(float));
	}
	return TRUE;
}

/* The values of the inputs of fixed->mask. That they do not vary is read
off the program (fixed_inputs); the first draws of each program make sure of
it by running it for another vertex as well, and a program whose values
differ is not trusted again. The values are remembered with what they were
computed from, and a draw that computes them from the same again is spared
the run. FALSE if there are no such values. */
static BOOL fixed_values(const struct xgpu_fixed_draw *draw, struct fixed_program *fixed_program,
	const struct nv2a_ff_state *ff_state, const struct nv2a_ff_constants *constants, struct primary_fixed *fixed)
{
	/* by the program's number: draws checked so far, 255 for a program that failed */
	static unsigned char checked[256];
	unsigned char *count = &checked[draw->program_id % 256];
	struct nv2a_vsh_cpu_program *program = fixed_program->program;
	struct fixed_cache *cache = fixed_program->fixed_cache;
	float inputs[FIXED_CACHE_INPUTS][4], fog_coordinate;
	unsigned long input_count = 0, index;
	unsigned short stream_registers = 0;
	BOOL cacheable;

	if (*count == 255)
		return FALSE;
	for (index = 0; index < draw->element_count; index++)
		stream_registers |= (unsigned short)(1u << draw->elements[index].reg);
	if (!cache)
		cache = fixed_program->fixed_cache = calloc(1, sizeof(*cache));
	cacheable = cache && fixed_cache_inputs(draw, nv2a_vsh_cpu_classification(program), fixed->mask, stream_registers,
		inputs, &input_count);
	if (cacheable && cache->valid && cache->mask == fixed->mask && cache->stream_registers == stream_registers &&
		cache->input_count == input_count && !memcmp(cache->inputs, inputs, input_count * sizeof(inputs[0])))
	{
		memcpy(fixed->v0, cache->v0, sizeof(fixed->v0));
		memcpy(fixed->v1, cache->v1, sizeof(fixed->v1));
		fog_coordinate = cache->fog_coordinate;
	}
	else
	{
		if (!fixed_values_at(draw, program, FALSE, fixed, &fog_coordinate))
			return FALSE;
		if (*count < FIXED_CHECKED_DRAWS && draw->count > 1)
		{
			struct primary_fixed other = *fixed;
			float other_fog_coordinate;

			(*count)++;
			if (!fixed_values_at(draw, program, TRUE, &other, &other_fog_coordinate) || memcmp(fixed, &other, sizeof(other)) ||
				other_fog_coordinate != fog_coordinate)
			{
				*count = 255;
				platform_log("gl1: vertex program %lu's colours vary where it says they do not (inputs %x); they are "
					"computed at every vertex", draw->program_id, fixed->mask);
				return FALSE;
			}
		}
		if (cacheable)
		{
			cache->valid = TRUE;
			cache->mask = fixed->mask;
			cache->stream_registers = stream_registers;
			cache->input_count = input_count;
			memcpy(cache->inputs, inputs, input_count * sizeof(inputs[0]));
			memcpy(cache->v0, fixed->v0, sizeof(cache->v0));
			memcpy(cache->v1, fixed->v1, sizeof(cache->v1));
			cache->fog_coordinate = fog_coordinate;
		}
	}
	fixed->fog = (fixed->mask & (1u << _input_fog)) ? fog_factor_from_coordinate(ff_state, constants, fog_coordinate) : 1.0f;
	return TRUE;
}

/* ---------- visibility tests

Without occlusion queries: the test's draws are small rectangles of one
depth (the lens flares'), whose place in the window is known once their
vertices are transformed. When the frame is presented the depth buffer is
read at up to eight by eight points of each test's rectangle, and the share
of them the rectangle would have passed at, times its size, is the count.
A context that cannot be read like this, or only slowly, answers that the
whole rectangle was visible. */

#define VISIBILITY_SLOTS 4096
#define VISIBILITY_PENDING 512
#define VISIBILITY_GRID 8
/* a frame's read-backs taking longer than this, this many frames running,
ends them */
#define VISIBILITY_SLOW_NANOSECONDS 6000000L
#define VISIBILITY_SLOW_FRAMES 5

struct visibility_test
{
	DWORD slot;
	BOOL any;
	/* window pixels, rows from the bottom */
	float x0, y0, x1, y1;
	/* the sum of the vertices' window depths, and how many */
	float depth_sum;
	unsigned long depth_count;
	BOOL depth_test;
	GLenum function;
};

static struct
{
	BOOL active;
	struct visibility_test current;
	struct visibility_test pending[VISIBILITY_PENDING];
	unsigned long pending_count;
	UINT results[VISIBILITY_SLOTS];
	BOOL no_read_back;
	unsigned long slow_frames;
	float *depths;
	unsigned long depths_size;
} visibility;

void xgpu_fixed_visibility_begin(void)
{
	visibility.active = TRUE;
	memset(&visibility.current, 0, sizeof(visibility.current));
}

void xgpu_fixed_visibility_end(DWORD slot)
{
	if (!visibility.active)
		return;
	visibility.active = FALSE;
	slot %= VISIBILITY_SLOTS;
	visibility.current.slot = slot ? slot : 1;
	stats.visibility_tests++;
	if (visibility.pending_count < VISIBILITY_PENDING)
		visibility.pending[visibility.pending_count++] = visibility.current;
}

UINT xgpu_fixed_visibility_result(DWORD slot)
{
	slot %= VISIBILITY_SLOTS;
	return visibility.results[slot ? slot : 1];
}

/* adds a draw's vertices to the running test */
static void visibility_measure(const struct xgpu_fixed_draw *draw, const struct vertex_range *range,
	const GLint *viewport, BOOL depth_test)
{
	struct visibility_test *test = &visibility.current;
	const float *position = arrays.position + range->base * 4;
	unsigned long index;

	for (index = 0; index < range->count; index++, position += 4)
	{
		float x, y, z;

		if (!(position[3] > 0.0f))
			continue;
		x = (float)viewport[0] + (position[0] / position[3] + 1.0f) * 0.5f * (float)viewport[2];
		y = (float)viewport[1] + (position[1] / position[3] + 1.0f) * 0.5f * (float)viewport[3];
		z = draw->viewport.MinZ + (position[2] / position[3] + 1.0f) * 0.5f * (draw->viewport.MaxZ - draw->viewport.MinZ);
		if (!test->any)
		{
			test->any = TRUE;
			test->x0 = test->x1 = x;
			test->y0 = test->y1 = y;
		}
		if (x < test->x0) test->x0 = x;
		if (x > test->x1) test->x1 = x;
		if (y < test->y0) test->y0 = y;
		if (y > test->y1) test->y1 = y;
		test->depth_sum += z;
		test->depth_count++;
	}
	test->depth_test = depth_test;
	test->function = D3D__RenderState[D3DRS_ZFUNC] ? (GLenum)D3D__RenderState[D3DRS_ZFUNC] : GL_NEVER;
}

static BOOL depth_passes(GLenum function, float depth, float stored, float step)
{
	switch (function)
	{
	case GL_NEVER: return FALSE;
	case GL_LESS: return depth < stored;
	case GL_EQUAL: return depth >= stored - step && depth <= stored + step;
	case GL_LEQUAL: return depth <= stored + step;
	case GL_GREATER: return depth > stored;
	case GL_NOTEQUAL: return depth < stored - step || depth > stored + step;
	case GL_GEQUAL: return depth >= stored - step;
	default: return TRUE;
	}
}

static long nanoseconds_since(const struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)(now.tv_sec - start->tv_sec) * 1000000000L + (now.tv_nsec - start->tv_nsec);
}

/* the microseconds since the last call, for one of the statistics */
static void stats_lap(unsigned long *total)
{
	if (stats_timed)
	{
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (total)
		{
			*total += (unsigned long)((now.tv_sec - stats_clock.tv_sec) * 1000000L +
				(now.tv_nsec - stats_clock.tv_nsec) / 1000L);
		}
		stats_clock = now;
	}
}

/* the part of a test's rectangle that is inside the picture, and its size
in the game's pixels; FALSE if there is none */
static BOOL visibility_rectangle(const struct visibility_test *test, const struct xgpu_fixed_target *target,
	long *x0, long *y0, long *width, long *height, float *area)
{
	long x1, y1;

	if (!test->any || !test->depth_count)
		return FALSE;
	*x0 = (long)floorf(test->x0 + 0.5f);
	*y0 = (long)floorf(test->y0 + 0.5f);
	x1 = (long)floorf(test->x1 + 0.5f);
	y1 = (long)floorf(test->y1 + 0.5f);
	if (*x0 < target->window_x) *x0 = target->window_x;
	if (*y0 < target->window_y) *y0 = target->window_y;
	if (x1 > target->window_x + (long)target->gl_width) x1 = target->window_x + (long)target->gl_width;
	if (y1 > target->window_y + (long)target->gl_height) y1 = target->window_y + (long)target->gl_height;
	*width = x1 - *x0;
	*height = y1 - *y0;
	if (*width <= 0 || *height <= 0)
		return FALSE;
	*area = (float)*width * (float)*height / (target->scale[0] * target->scale[1]);
	return TRUE;
}

/* counts the frame's tests, before the swap takes the depth buffer away */
static void visibility_count(const struct xgpu_fixed_target *target)
{
	float step = xgpu_fixed_context.depth_bits >= 24 ? 1.0f / 16777216.0f :
		xgpu_fixed_context.depth_bits > 0 ? 1.5f / (float)(1UL << xgpu_fixed_context.depth_bits) : 0.0f;
	BOOL read_back = !visibility.no_read_back && xgpu_fixed_context.depth_bits > 0;
	BOOL any_read = FALSE;
	struct timespec start;
	unsigned long index;

	if (!visibility.pending_count)
		return;
	clock_gettime(CLOCK_MONOTONIC, &start);
	if (read_back)
	{
		while (glGetError() != GL_NO_ERROR)
			;
	}
	for (index = 0; index < visibility.pending_count; index++)
	{
		const struct visibility_test *test = &visibility.pending[index];
		long x0, y0, width, height;
		float area, depth;
		unsigned long samples = 0, passed = 0;

		visibility.results[test->slot] = 0;
		if (!visibility_rectangle(test, target, &x0, &y0, &width, &height, &area))
			continue;
		depth = test->depth_sum / (float)test->depth_count;
		if (read_back && test->depth_test)
		{
			long columns = width < VISIBILITY_GRID ? width : VISIBILITY_GRID;
			long rows = height < VISIBILITY_GRID ? height : VISIBILITY_GRID;
			long row, column;

			if ((unsigned long)width > visibility.depths_size)
			{
				free(visibility.depths);
				visibility.depths_size = (unsigned long)width + 256;
				visibility.depths = malloc(visibility.depths_size * sizeof(float));
			}
			for (row = 0; row < rows && visibility.depths; row++)
			{
				long y = y0 + (2 * row + 1) * height / (2 * rows);

				glReadPixels((GLint)x0, (GLint)y, (GLsizei)width, 1, GL_DEPTH_COMPONENT, GL_FLOAT, visibility.depths);
				any_read = TRUE;
				for (column = 0; column < columns; column++)
				{
					long x = (2 * column + 1) * width / (2 * columns);

					samples++;
					if (depth_passes(test->function, depth, visibility.depths[x], step))
						passed++;
				}
			}
		}
		/* without a depth test, or without samples: all of it */
		if (!samples)
			visibility.results[test->slot] = (UINT)(area + 0.5f);
		else
			visibility.results[test->slot] = (UINT)(area * (float)passed / (float)samples + 0.5f);
	}
	if (any_read)
	{
		long elapsed = nanoseconds_since(&start);

		if (glGetError() != GL_NO_ERROR)
		{
			visibility.no_read_back = TRUE;
			platform_log("gl1: the depth buffer cannot be read: lens flares shine through walls");
			/* what was read means nothing: this frame's tests as they
			will be answered from now on */
			for (index = 0; index < visibility.pending_count; index++)
			{
				const struct visibility_test *test = &visibility.pending[index];
				long x0, y0, width, height;
				float area;

				if (visibility_rectangle(test, target, &x0, &y0, &width, &height, &area))
					visibility.results[test->slot] = (UINT)(area + 0.5f);
			}
		}
		else if (elapsed > VISIBILITY_SLOW_NANOSECONDS)
		{
			if (++visibility.slow_frames >= VISIBILITY_SLOW_FRAMES)
			{
				visibility.no_read_back = TRUE;
				platform_log("gl1: reading the depth buffer takes %ld ms a frame: lens flares shine through walls",
					elapsed / 1000000L);
			}
		}
		else
		{
			visibility.slow_frames = 0;
		}
	}
	visibility.pending_count = 0;
}

/* ---------- small off-screen targets: a corner of the window

The game draws its motion sensor into a 64x64 texture and then draws that
on the HUD. Without off-screen rendering, such a target is drawn in the
lower left corner of the window instead: when the first draw or clear of a
frame goes to it, what the corner holds is copied to a texture; the
target's draws land in the corner (without depth: the sensor has none);
and when something else is drawn, or a draw samples the target, the
corner is copied to the target's texture and the window's own pixels are
put back. Targets up to CORNER_LARGEST square go this way; the larger
ones (shadows, glows, screen effects) are left out as before.

Only in a window with alpha of its own: the pictures' alpha is what the
draws that sample them read (the sensor is cleared to an alpha of 0 and
added to the screen with ONE, INVSRCALPHA; the sun's glow is the back
buffer's alpha, blurred). Without it a copy's alpha is 1, which made the
sensor an opaque black square and lit the glow behind walls; such a
window leaves these targets out, as it did before. */

#define CORNER_LARGEST 64

static struct
{
	/* the target drawn in the corner: its address and size (data 0: none) */
	unsigned long data, width, height;
	/* its pixels in the window, from the lower left corner */
	unsigned long gl_width, gl_height;
	float scale[2];
	/* the corner holds the target's picture */
	BOOL active;
	/* the window's pixels the corner had */
	GLuint saved;
	unsigned long saved_width, saved_height;
} corner;

/* the pictures of the small targets, for the draws that sample them: the
game draws the sun's glow back and forth between two */
#define CORNER_PICTURES 4

static struct corner_picture
{
	unsigned long data, width, height;
	/* the picture's pixels, and the texture that holds them */
	unsigned long gl_width, gl_height;
	GLuint texture;
	unsigned long texture_width, texture_height;
	BOOL valid;
	unsigned long used;
} corner_pictures[CORNER_PICTURES];

/* the picture of a target: its own, or the one unused for longest */
static struct corner_picture *corner_picture_get(unsigned long data, unsigned long width, unsigned long height)
{
	static unsigned long clock;
	struct corner_picture *picture, *oldest = &corner_pictures[0];
	int index;

	for (index = 0; index < CORNER_PICTURES; index++)
	{
		picture = &corner_pictures[index];
		if (picture->data == data && picture->width == width && picture->height == height)
		{
			picture->used = ++clock;
			return picture;
		}
		if (picture->used < oldest->used)
			oldest = picture;
	}
	oldest->data = data;
	oldest->width = width;
	oldest->height = height;
	oldest->valid = FALSE;
	oldest->used = ++clock;
	return oldest;
}

/* the window as the last draw, clear or presentation found it: a corner
target has no window of its own */
static struct
{
	BOOL valid;
	long x, y;
	unsigned long gl_width, gl_height;
	float scale[2];
} window_geometry;

static void window_geometry_note(const struct xgpu_fixed_target *target)
{
	if (!target->window || !target->gl_width || !target->gl_height)
		return;
	window_geometry.valid = TRUE;
	window_geometry.x = target->window_x;
	window_geometry.y = target->window_y;
	window_geometry.gl_width = target->gl_width;
	window_geometry.gl_height = target->gl_height;
	window_geometry.scale[0] = target->scale[0];
	window_geometry.scale[1] = target->scale[1];
}

static unsigned long power_of_two_above(unsigned long value)
{
	unsigned long result = 1;

	while (result < value)
		result *= 2;
	return result;
}

/* a texture of the corner's size (or more), for a copy of it */
static void corner_texture_make(GLuint *name, unsigned long *width, unsigned long *height)
{
	unsigned long wanted_width = power_of_two_above(corner.gl_width), wanted_height = power_of_two_above(corner.gl_height);

	if (*name && *width == wanted_width && *height == wanted_height)
		return;
	if (!*name)
		glGenTextures(1, name);
	glBindTexture(GL_TEXTURE_2D, *name);
	glTexImage2D(GL_TEXTURE_2D, 0, xgpu_fixed_context.alpha_bits ? GL_RGBA : GL_RGB, (GLsizei)wanted_width,
		(GLsizei)wanted_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	*width = wanted_width;
	*height = wanted_height;
	xgpu_fixed_state_invalidate();
}

static void corner_copy(GLuint name)
{
	glBindTexture(GL_TEXTURE_2D, name);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)corner.gl_width, (GLsizei)corner.gl_height);
	xgpu_fixed_state_invalidate();
	xgpu_fixed_back_buffer_copies++;
}

/* the window's pixels back into the corner: the saved texture drawn over
it, through the state cache so that the next draw finds the state it
expects */
static void corner_restore(void)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static float quad[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };
	static float coordinates[4][2];
	GLint viewport[4];
	struct unit_state *shadow = &state.units[0];
	int unit;

	coordinates[1][0] = coordinates[2][0] = (float)corner.gl_width / (float)corner.saved_width;
	coordinates[2][1] = coordinates[3][1] = (float)corner.gl_height / (float)corner.saved_height;
	viewport[0] = viewport[1] = 0;
	viewport[2] = (GLint)corner.gl_width;
	viewport[3] = (GLint)corner.gl_height;
	if (memcmp(state.viewport, viewport, sizeof(viewport)))
	{
		memcpy(state.viewport, viewport, sizeof(viewport));
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	}
	state_enable(&state.depth_test, GL_DEPTH_TEST, FALSE);
	state_enable(&state.stencil_test, GL_STENCIL_TEST, FALSE);
	state_enable(&state.blend, GL_BLEND, FALSE);
	state_enable(&state.alpha_test, GL_ALPHA_TEST, FALSE);
	state_enable(&state.cull_face, GL_CULL_FACE, FALSE);
	state_enable(&state.offset_fill, GL_POLYGON_OFFSET_FILL, FALSE);
	if (state.polygon_mode != GL_FILL)
	{
		state.polygon_mode = GL_FILL;
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}
	if (state.color_mask != 15)
	{
		state.color_mask = 15;
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	}
	state_projection(NULL);
	state_vertex_pointer(2, GL_FLOAT, 2 * sizeof(float), quad);
	state_color(NULL, white);
	state_unit_enable(0, TRUE);
	state_unit_texture(0, corner.saved);
	state_unit_matrix_full(0, identity_matrix);
	state_unit(0);
	if (shadow->mode != GL_REPLACE)
	{
		shadow->mode = GL_REPLACE;
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	}
	state_unit_coordinates(0, 2, GL_FLOAT, 2 * sizeof(float), coordinates);
	for (unit = 1; unit < xgpu_fixed_context.units; unit++)
	{
		state_unit_enable(unit, FALSE);
		state_unit_coordinates(unit, 0, 0, 0, NULL);
	}
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/* the target's picture is complete: to its texture, and the window's
pixels back */
static void corner_end(void)
{
	if (!corner.active)
		return;
	corner.active = FALSE;
	{
		struct corner_picture *picture = corner_picture_get(corner.data, corner.width, corner.height);

		corner_texture_make(&picture->texture, &picture->texture_width, &picture->texture_height);
		corner_copy(picture->texture);
		picture->gl_width = corner.gl_width;
		picture->gl_height = corner.gl_height;
		picture->valid = TRUE;
	}
	corner_restore();
	xgpu_fixed_back_buffer_drawn();
}

/* Whether an off-screen target is drawn in the corner; mapped is then the
target as the corner: the window, its lower left pixels, no depth. The
first draw or clear begins the session: the corner's pixels are saved. */
static BOOL corner_target(const struct xgpu_fixed_target *target, struct xgpu_fixed_target *mapped)
{
	unsigned long gl_width, gl_height;

	if (target->window || !target->color_data || !target->width || !target->height ||
		target->width > CORNER_LARGEST || target->height > CORNER_LARGEST || !window_geometry.valid ||
		xgpu_fixed_context.alpha_bits <= 0)
	{
		return FALSE;
	}
	gl_width = (unsigned long)floorf((float)target->width * window_geometry.scale[0] + 0.5f);
	gl_height = (unsigned long)floorf((float)target->height * window_geometry.scale[1] + 0.5f);
	if (gl_width < 1 || gl_height < 1 || gl_width > window_geometry.gl_width || gl_height > window_geometry.gl_height ||
		power_of_two_above(gl_width) > (unsigned long)xgpu_fixed_context.maximum_texture_size ||
		power_of_two_above(gl_height) > (unsigned long)xgpu_fixed_context.maximum_texture_size)
	{
		return FALSE;
	}
	if (corner.active && (corner.data != target->color_data || corner.width != target->width || corner.height != target->height))
		corner_end();
	/* (an off-screen target all the same: a texture at its address is the
	target, not texels to upload) */
	xgpu_fixed_off_screen_target(target->color_data);
	corner.data = target->color_data;
	corner.width = target->width;
	corner.height = target->height;
	corner.gl_width = gl_width;
	corner.gl_height = gl_height;
	corner.scale[0] = window_geometry.scale[0];
	corner.scale[1] = window_geometry.scale[1];
	if (!corner.active)
	{
		corner_texture_make(&corner.saved, &corner.saved_width, &corner.saved_height);
		corner_copy(corner.saved);
		corner.active = TRUE;
	}
	*mapped = *target;
	mapped->window = TRUE;
	mapped->depth = FALSE;
	mapped->window_x = 0;
	mapped->window_y = 0;
	mapped->gl_width = gl_width;
	mapped->gl_height = gl_height;
	mapped->scale[0] = corner.scale[0];
	mapped->scale[1] = corner.scale[1];
	return TRUE;
}

/* the texture a draw samples the corner's target through (as the back
buffer's copy: xgpu_fixed_textures.c, back_buffer_copy); FALSE when the
target has not been drawn */
static BOOL corner_sample(const struct xgpu_fixed_draw *draw, int stage, struct xgpu_fixed_texture *texture)
{
	const D3DBaseTexture *resource = draw->textures[stage];
	struct xgpu_texture_description description;
	struct corner_picture *picture = NULL;
	float used_width, used_height;
	int index;

	if (!resource || !resource->Data)
		return FALSE;
	/* (the target being drawn: its picture is complete now) */
	if (corner.active && resource->Data == corner.data)
		corner_end();
	for (index = 0; index < CORNER_PICTURES; index++)
	{
		if (corner_pictures[index].valid && corner_pictures[index].data == resource->Data)
			picture = &corner_pictures[index];
	}
	if (!picture)
		return FALSE;
	xgpu_texture_describe(resource->Format, resource->Size, &description);
	memset(texture, 0, sizeof(*texture));
	texture->type = _xgpu_fixed_texture_off_screen;
	texture->name = picture->texture;
	texture->base = xgpu_fixed_context.alpha_bits ? _xgpu_fixed_base_rgba : _xgpu_fixed_base_rgb;
	/* the picture is the lower left part of the texture, its rows from
	the bottom: t becomes 1 - t */
	used_width = (float)picture->gl_width / (float)picture->texture_width;
	used_height = (float)picture->gl_height / (float)picture->texture_height;
	if (description.linear)
	{
		used_width /= (float)picture->width;
		texture->scale[1] = -used_height / (float)picture->height;
	}
	else
	{
		texture->scale[1] = -used_height;
	}
	texture->scale[0] = used_width;
	texture->offset[1] = used_height;
	return TRUE;
}

/* ---------- drawing */

/* one side of a combine environment: function(source0 operand0, source1 operand1, source2 operand2) */
static void combine_side(struct nv2a_ff_combine *side, int function, int source0, int operand0, int source1, int operand1,
	int source2, int operand2)
{
	memset(side, 0, sizeof(*side));
	side->function = (unsigned char)function;
	side->source[0] = (unsigned char)source0;
	side->operand[0] = (unsigned char)operand0;
	side->source[1] = (unsigned char)source1;
	side->operand[1] = (unsigned char)operand1;
	side->source[2] = (unsigned char)source2;
	side->operand[2] = (unsigned char)operand2;
	side->scale = 1;
}

/* The texture environment of a unit. The plans mean each environment as
OpenGL defines it for a red, green, blue, alpha texture, with the texel the
Xbox samples (a luminance texture's alpha 1, an intensity texture's its
intensity: xgpu_fixed.h). OpenGL 1.1's own environments read the texel by
the texture's base format instead, and differ where the texture has no
alpha of its own (REPLACE takes the fragment's), is an intensity (BLEND and
ADD put it into the alpha) or is not red, green, blue (DECAL is undefined):
those are spelled out with the combine extension, whose TEXTURE source is
the Xbox's texel; without the extension no such texture is uploaded
(xgpu_fixed_textures.c, upload_format_choose). */
static void unit_environment(int unit, const struct nv2a_ff_unit *plan_unit, const float *constant, int base)
{
	static const GLenum functions[] =
	{
		GL_REPLACE, GL_MODULATE, GL_ADD, GL_ADD_SIGNED, GL_INTERPOLATE, GL_SUBTRACT, GL_DOT3_RGB
	};
	static const GLenum sources[] = { GL_TEXTURE, GL_CONSTANT, GL_PRIMARY_COLOR, GL_PREVIOUS };
	static const GLenum operands[] = { GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA };
	struct unit_state *shadow = &state.units[unit];
	struct nv2a_ff_combine sides[2];
	GLenum mode = GL_MODULATE;
	BOOL combine = xgpu_fixed_context.combine;
	int side, argument;

	memset(sides, 0, sizeof(sides));
	switch (plan_unit->environment)
	{
	case _nv2a_ff_environment_replace:
		if (combine && (base == _xgpu_fixed_base_luminance || base == _xgpu_fixed_base_rgb))
		{
			mode = GL_COMBINE;
			combine_side(&sides[0], _nv2a_ff_combine_replace, _nv2a_ff_source_texture, _nv2a_ff_operand_color, 0, 0, 0, 0);
			combine_side(&sides[1], _nv2a_ff_combine_replace, _nv2a_ff_source_texture, _nv2a_ff_operand_alpha, 0, 0, 0, 0);
		}
		else
			mode = GL_REPLACE;
		break;
	case _nv2a_ff_environment_modulate: mode = GL_MODULATE; break;
	case _nv2a_ff_environment_decal:
		if (combine && base != _xgpu_fixed_base_rgba && base != _xgpu_fixed_base_rgb)
		{
			/* the texel over the fragment by the texel's alpha; the fragment's alpha */
			mode = GL_COMBINE;
			combine_side(&sides[0], _nv2a_ff_combine_interpolate, _nv2a_ff_source_texture, _nv2a_ff_operand_color,
				_nv2a_ff_source_previous, _nv2a_ff_operand_color, _nv2a_ff_source_texture, _nv2a_ff_operand_alpha);
			combine_side(&sides[1], _nv2a_ff_combine_replace, _nv2a_ff_source_previous, _nv2a_ff_operand_alpha, 0, 0, 0, 0);
		}
		else
			mode = GL_DECAL;
		break;
	case _nv2a_ff_environment_blend:
		if (combine && base == _xgpu_fixed_base_intensity)
		{
			/* the constant over the fragment by the intensity (which is the texel's alpha); the alphas multiplied */
			mode = GL_COMBINE;
			combine_side(&sides[0], _nv2a_ff_combine_interpolate, _nv2a_ff_source_constant, _nv2a_ff_operand_color,
				_nv2a_ff_source_previous, _nv2a_ff_operand_color, _nv2a_ff_source_texture, _nv2a_ff_operand_alpha);
			combine_side(&sides[1], _nv2a_ff_combine_modulate, _nv2a_ff_source_previous, _nv2a_ff_operand_alpha,
				_nv2a_ff_source_texture, _nv2a_ff_operand_alpha, 0, 0);
		}
		else
			mode = GL_BLEND;
		break;
	case _nv2a_ff_environment_add:
		if (xgpu_fixed_context.environment_add && !(combine && base == _xgpu_fixed_base_intensity))
		{
			mode = GL_ADD;
		}
		else
		{
			/* the combine extension's: colours added, alphas multiplied */
			mode = GL_COMBINE;
			combine_side(&sides[0], _nv2a_ff_combine_add, _nv2a_ff_source_previous, _nv2a_ff_operand_color,
				_nv2a_ff_source_texture, _nv2a_ff_operand_color, 0, 0);
			combine_side(&sides[1], _nv2a_ff_combine_modulate, _nv2a_ff_source_previous, _nv2a_ff_operand_alpha,
				_nv2a_ff_source_texture, _nv2a_ff_operand_alpha, 0, 0);
		}
		break;
	default:
		mode = GL_COMBINE;
		sides[0] = plan_unit->rgb;
		sides[1] = plan_unit->alpha;
		break;
	}
	state_unit(unit);
	if (shadow->mode != mode)
	{
		shadow->mode = mode;
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (GLint)mode);
	}
	if (mode == GL_COMBINE)
	{
		for (side = 0; side < 2; side++)
		{
			const struct nv2a_ff_combine *combine = &sides[side];
			struct nv2a_ff_combine *held = side ? &shadow->alpha : &shadow->rgb;
			int function = combine->function < sizeof(functions) / sizeof(functions[0]) ? combine->function : 0;
			int arguments = function == _nv2a_ff_combine_replace ? 1 : function == _nv2a_ff_combine_interpolate ? 3 : 2;
			int scale = combine->scale == 2 || combine->scale == 4 ? combine->scale : 1;

			if (held->function != function)
			{
				held->function = (unsigned char)function;
				glTexEnvi(GL_TEXTURE_ENV, side ? GL_COMBINE_ALPHA : GL_COMBINE_RGB, (GLint)functions[function]);
			}
			for (argument = 0; argument < arguments; argument++)
			{
				int source = combine->source[argument] & 3;
				int operand = combine->operand[argument] & 3;

				/* (the alpha side reads alphas only) */
				if (side && operand < _nv2a_ff_operand_alpha)
					operand += _nv2a_ff_operand_alpha;
				if (held->source[argument] != source)
				{
					held->source[argument] = (unsigned char)source;
					glTexEnvi(GL_TEXTURE_ENV, (side ? GL_SOURCE0_ALPHA : GL_SOURCE0_RGB) + (GLenum)argument,
						(GLint)sources[source]);
				}
				if (held->operand[argument] != operand)
				{
					held->operand[argument] = (unsigned char)operand;
					glTexEnvi(GL_TEXTURE_ENV, (side ? GL_OPERAND0_ALPHA : GL_OPERAND0_RGB) + (GLenum)argument,
						(GLint)operands[operand]);
				}
			}
			if (held->scale != scale)
			{
				held->scale = (unsigned char)scale;
				glTexEnvi(GL_TEXTURE_ENV, side ? GL_ALPHA_SCALE : GL_RGB_SCALE, scale);
			}
		}
	}
	if (constant && memcmp(shadow->color, constant, sizeof(shadow->color)))
	{
		memcpy(shadow->color, constant, sizeof(shadow->color));
		glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, constant);
	}
}

/* The framebuffer blend of a pass. What the context cannot do is replaced
by the nearest it can (the compiler's plans ask for nothing it lacks; the
built-in plan takes the draw's blend as it is): subtraction darkens,
minimum multiplies, maximum screens, and a constant factor counts as one. */
static void pass_blend(const struct nv2a_ff_pass *pass, const float *blend_color)
{
	GLenum source = pass->source_blend, destination = pass->destination_blend;
	GLenum equation = pass->blend_operation ? blend_equation(pass->blend_operation) : GL_FUNC_ADD;

	state_enable(&state.blend, GL_BLEND, pass->blend_enable != 0);
	if (!pass->blend_enable)
		return;
	if ((equation == GL_FUNC_SUBTRACT || equation == GL_FUNC_REVERSE_SUBTRACT) && !xgpu_fixed_context.blend_subtract)
	{
		source = GL_ZERO;
		destination = GL_ONE_MINUS_SRC_COLOR;
		equation = GL_FUNC_ADD;
	}
	if (equation == GL_MIN && !xgpu_fixed_context.blend_minmax)
	{
		source = GL_DST_COLOR;
		destination = GL_ZERO;
		equation = GL_FUNC_ADD;
	}
	if (equation == GL_MAX && !xgpu_fixed_context.blend_minmax)
	{
		source = GL_ONE_MINUS_DST_COLOR;
		destination = GL_ONE;
		equation = GL_FUNC_ADD;
	}
	if (!xgpu_fixed_context.blend_color)
	{
		if (source == GL_CONSTANT_COLOR || source == GL_CONSTANT_ALPHA)
			source = GL_ONE;
		else if (source == GL_ONE_MINUS_CONSTANT_COLOR || source == GL_ONE_MINUS_CONSTANT_ALPHA)
			source = GL_ZERO;
		if (destination == GL_CONSTANT_COLOR || destination == GL_CONSTANT_ALPHA)
			destination = GL_ONE;
		else if (destination == GL_ONE_MINUS_CONSTANT_COLOR || destination == GL_ONE_MINUS_CONSTANT_ALPHA)
			destination = GL_ZERO;
	}
	if (state.blend_source != source || state.blend_destination != destination)
	{
		state.blend_source = source;
		state.blend_destination = destination;
		glBlendFunc(source, destination);
	}
	if (xgpu_fixed_context.blend_subtract || xgpu_fixed_context.blend_minmax)
	{
		if (state.blend_equation != equation)
		{
			state.blend_equation = equation;
			glBlendEquation(equation);
		}
	}
	if (xgpu_fixed_context.blend_color && memcmp(state.blend_color, blend_color, 4 * sizeof(float)))
	{
		memcpy(state.blend_color, blend_color, 4 * sizeof(float));
		glBlendColor(blend_color[0], blend_color[1], blend_color[2], blend_color[3]);
	}
}

static unsigned char unit_byte(float value)
{
	return (unsigned char)(value <= 0.0f ? 0 : value >= 1.0f ? 255 : (int)(value * 255.0f + 0.5f));
}

/* the stages a pass's units take texture coordinates from */
static unsigned pass_stages(const struct nv2a_ff_pass *pass)
{
	unsigned stages = 0;
	int unit;

	for (unit = 0; unit < pass->unit_count && unit < NV2A_FF_MAXIMUM_UNITS; unit++)
	{
		if (pass->units[unit].stage < NV2A_FF_STAGE_COUNT)
			stages |= 1u << pass->units[unit].stage;
	}
	return stages;
}

/* How much each stage's texture matters to the picture
(nv2a_ff_draw.stage_rank), which the compiler asks when it has to leave
textures out. The device does not know what a shader is for, but a few
things tell: a stage whose coordinates are the lightmap stream's (input
register 8 of the environment's vertex formats) is the lightmap, which a
wall cannot do without; a bump map (palettized or signed texels) adds
least without the lighting it was made for; and the models' shader (three
maps and a cube map on skinned vertices) has its masks in the third map,
which matters more than the detail map in the second. Otherwise earlier
stages matter more, which is also what all zeroes say. */
static void stage_ranks(const struct xgpu_fixed_draw *draw, const struct nv2a_vsh_classification *classification,
	const struct nv2a_pixel_shader_key *key, unsigned char *ranks)
{
	BOOL model = classification->position_class == _nv2a_vsh_class_skinned &&
		key->sampler_type[0] == _xgpu_sampler_2d && key->sampler_type[1] == _xgpu_sampler_2d &&
		key->sampler_type[2] == _xgpu_sampler_2d && key->sampler_type[3] == _xgpu_sampler_cube;
	int scores[NV2A_FF_STAGE_COUNT];
	int stage, other;

	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		const struct nv2a_vsh_output_class *coordinates = &classification->outputs[_nv2a_vsh_output_texcoord0 + stage];
		D3DBaseTexture *texture = draw->textures[stage];

		scores[stage] = (NV2A_FF_STAGE_COUNT - 1 - stage) * 4;
		if (key->sampler_type[stage] != _xgpu_sampler_2d || !texture)
		{
			scores[stage] -= 200;
			continue;
		}
		if (coordinates->class == _nv2a_vsh_class_affine && coordinates->input == 8)
			scores[stage] += 100;
		{
			struct xgpu_texture_description description;

			xgpu_texture_describe(texture->Format, texture->Size, &description);
			/* P8, G8B8 (V8U8), V16U16 */
			if (description.format == 0x0b || description.format == 0x28 || description.format == 0x17 ||
				description.format == 0x33 || description.format == 0x36 || key->color_sign[stage])
			{
				scores[stage] -= 100;
			}
		}
		if (model && stage == 2)
			scores[stage] += 6;
	}
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		ranks[stage] = 0;
		for (other = 0; other < NV2A_FF_STAGE_COUNT; other++)
		{
			if (scores[other] < scores[stage] || (scores[other] == scores[stage] && other > stage))
				ranks[stage]++;
		}
	}
	/* (the usual order has one spelling, for the cache of plans) */
	if (ranks[0] == 3 && ranks[1] == 2 && ranks[2] == 1 && ranks[3] == 0)
		memset(ranks, 0, NV2A_FF_STAGE_COUNT);
}

/* Whether a colour array holds the same colour at every vertex of a small
draw (the HUD's quads, text): the colour itself then, and no array. Larger
draws are not looked at. */
#define UNIFORM_COLOR_VERTICES 64

static BOOL color_array_uniform(const unsigned char *array, const struct vertex_range *range, float *color)
{
	const unsigned char *first = array + range->base * 4;
	unsigned long index;

	if (range->count > UNIFORM_COLOR_VERTICES)
		return FALSE;
	for (index = 1; index < range->count; index++)
	{
		if (memcmp(first + index * 4, first, 4))
			return FALSE;
	}
	for (index = 0; index < 4; index++)
		color[index] = (float)first[index] * (1.0f / 255.0f);
	return TRUE;
}

/* the bias of a primary colour the evaluator has just written (it does
the scale itself when there is no bias) */
static void primary_bias(const struct primary *primary, unsigned char *array, const struct vertex_range *range)
{
	unsigned char *color = array + range->base * 4;
	unsigned long index;
	int channel;

	if (!primary->biased)
		return;
	for (index = 0; index < range->count; index++, color += 4)
	{
		for (channel = 0; channel < 4; channel++)
		{
			color[channel] = unit_byte(primary->bias[channel] +
				primary->scale[channel] * ((float)color[channel] * (1.0f / 255.0f)));
		}
	}
}

void xgpu_fixed_draw(const struct xgpu_fixed_draw *draw)
{
	static unsigned long program_reported = 0xffffffffUL;
	static BOOL fog_reported;
	const struct xgpu_fixed_target *target = &draw->target;
	/* a draw into a corner target: the target as the corner, and the draw
	with the window as its target for the textures that sample the frame */
	struct xgpu_fixed_target mapped;
	struct xgpu_fixed_draw window_draw;
	DWORD *rs = D3D__RenderState;
	BOOL has_depth, has_stencil, depth_test;
	struct fixed_program *fixed_program;
	struct nv2a_vsh_cpu_program *program;
	const struct xgpu_fixed_element *elements[XGPU_VERTEX_ATTRIBUTE_COUNT] = { NULL };
	/* the lifted path: GL_PROJECTION, the position's stream, the stages' coordinates */
	BOOL lifted = FALSE;
	float projection[16];
	GLint position_size = 0;
	struct lifted_coordinates lifted_coordinates[NV2A_FF_STAGE_COUNT];
	const struct nv2a_vsh_classification *classification;
	struct nv2a_pixel_shader_key key;
	struct plan_key plan_key;
	struct nv2a_ff_constants constants;
	struct plan_entry *entry;
	struct plan_variant *variant;
	const struct nv2a_ff_plan *plan;
	struct xgpu_fixed_texture textures[NV2A_FF_STAGE_COUNT];
	struct xgpu_fixed_texture unit_textures[NV2A_FF_MAXIMUM_PASSES][NV2A_FF_MAXIMUM_UNITS];
	struct primary *primaries;
	/* for each pass, which of arrays.colors holds its primary colour (-1:
	none), and the primary each of those was written for */
	int pass_color[NV2A_FF_MAXIMUM_PASSES];
	const struct primary *color_primaries[NV2A_FF_MAXIMUM_PASSES];
	int color_count = 0;
	/* those arrays that hold one colour, and the colour */
	BOOL color_uniform[NV2A_FF_MAXIMUM_PASSES];
	float color_values[NV2A_FF_MAXIMUM_PASSES][4];
	struct primary_fixed fixed;
	int texture_types[NV2A_FF_STAGE_COUNT];
	BOOL texture_fetched[NV2A_FF_STAGE_COUNT] = { FALSE };
	struct vertex_range range;
	struct evaluation evaluation;
	unsigned stages_wanted = 0;
	BOOL need_v0 = FALSE, need_v1 = FALSE;
	BOOL locked = FALSE, lockable;
	float blend_color[4];
	GLenum mode;
	int stage, pass_index, unit;

	if (!draw->count)
		return;
	if (!target->window)
	{
		if (!corner_target(target, &mapped))
		{
			if (target->color_data)
				off_screen_report(target);
			stats.skipped_target++;
			return;
		}
		target = &mapped;
		window_draw = *draw;
		window_draw.target.window = TRUE;
		window_draw.target.depth = FALSE;
		window_draw.target.window_x = window_geometry.x;
		window_draw.target.window_y = window_geometry.y;
		window_draw.target.gl_width = window_geometry.gl_width;
		window_draw.target.gl_height = window_geometry.gl_height;
		window_draw.target.scale[0] = window_geometry.scale[0];
		window_draw.target.scale[1] = window_geometry.scale[1];
		draw = &window_draw;
	}
	else
	{
		corner_end();
		window_geometry_note(target);
	}
	has_depth = target->depth && xgpu_fixed_context.depth_bits > 0;
	has_stencil = target->depth && xgpu_fixed_context.stencil_bits > 0;
	depth_test = has_depth && rs[D3DRS_ZENABLE];

	stats_lap(NULL);
	/* ----- the vertex program */
	if (!*draw->program && draw->instructions)
		*draw->program = fixed_program_new(draw);
	fixed_program = *draw->program;
	if (!fixed_program)
	{
		stats.skipped_program++;
		return;
	}
	program = fixed_program->program;
	classification = nv2a_vsh_cpu_classification(program);
	for (unit = 0; unit < (int)draw->element_count; unit++)
		elements[draw->elements[unit].reg] = &draw->elements[unit];
	/* (a program whose position is lifted, fed by a stream OpenGL reads:
	the game's positions are floats) */
	if (fixed_program->lift && elements[classification->position_input])
	{
		GLenum type;
		BOOL normalized;

		double rows[4][4];

		if (element_gl_type(elements[classification->position_input]->type, &type, &position_size, &normalized) &&
			type == GL_FLOAT && lifted_rows(fixed_program, draw, &position_size, rows))
		{
			lifted = TRUE;
			lifted_projection(draw, rows, projection);
		}
	}

	/* ----- the pixel state, as the other renderers' shader key has it */
	memset(&key, 0, sizeof(key));
	memcpy(key.combiner_state, rs, sizeof(key.combiner_state));
	memset(&key.combiner_state[D3DRS_PSCONSTANT0_0], 0, 16 * sizeof(DWORD));
	key.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT0] = 0;
	key.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
	key.texture_modes = rs[D3DRS_PSTEXTUREMODES];
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		unsigned long texture_mode = (rs[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f;

		texture_types[stage] = _xgpu_fixed_texture_none;
		if (texture_mode == 0x11)
			key.sampler_type[stage] = _xgpu_sampler_2d;
		else if (texture_mode != 0 && texture_mode != 0x04 && texture_mode != 0x05)
		{
			texture_types[stage] = xgpu_fixed_texture_type(draw, stage);
			key.sampler_type[stage] = texture_types[stage] == _xgpu_fixed_texture_none ? _xgpu_sampler_none :
				texture_types[stage] == _xgpu_fixed_texture_cube ? _xgpu_sampler_cube :
				texture_types[stage] == _xgpu_fixed_texture_volume ? _xgpu_sampler_3d : _xgpu_sampler_2d;
		}
		key.alpha_kill[stage] = D3D__TextureState[stage][D3DTSS_ALPHAKILL] == D3DTALPHAKILL_ENABLE;
		key.color_sign[stage] = (unsigned char)((D3D__TextureState[stage][D3DTSS_COLORSIGN] >> 28) & 0xf);
	}
	key.alpha_test_function = rs[D3DRS_ALPHATESTENABLE] ? rs[D3DRS_ALPHAFUNC] : 0;
	key.fog_enable = rs[D3DRS_FOGENABLE] != 0;
	key.fog_table_mode = (unsigned char)rs[D3DRS_FOGTABLEMODE];

	memset(&plan_key, 0, sizeof(plan_key));
	NV2A_FF_STATE_FROM_KEY(&plan_key.state, &key);
	plan_key.draw.blend_enable = rs[D3DRS_ALPHABLENDENABLE] != 0;
	plan_key.draw.source_blend = plan_key.draw.blend_enable ? rs[D3DRS_SRCBLEND] : _nv2a_ff_blend_one;
	plan_key.draw.destination_blend = plan_key.draw.blend_enable ? rs[D3DRS_DESTBLEND] : _nv2a_ff_blend_zero;
	plan_key.draw.blend_operation = plan_key.draw.blend_enable ? blend_equation(rs[D3DRS_BLENDOP]) :
		_nv2a_ff_blend_operation_add;
	plan_key.draw.write_color = (rs[D3DRS_COLORWRITEENABLE] &
		(D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE)) != 0;
	plan_key.draw.write_alpha = (rs[D3DRS_COLORWRITEENABLE] & D3DCOLORWRITEENABLE_ALPHA) != 0;
	plan_key.draw.depth_write = depth_test && rs[D3DRS_ZWRITEENABLE] && rs[D3DRS_ZFUNC] != D3DCMP_NEVER;
	stage_ranks(draw, classification, &key, plan_key.draw.stage_rank);

	for (stage = 0; stage < 8; stage++)
	{
		color_to_vec4(rs[D3DRS_PSCONSTANT0_0 + stage], constants.c0[stage]);
		color_to_vec4(rs[D3DRS_PSCONSTANT1_0 + stage], constants.c1[stage]);
	}
	color_to_vec4(rs[D3DRS_PSFINALCOMBINERCONSTANT0], constants.final_c0);
	color_to_vec4(rs[D3DRS_PSFINALCOMBINERCONSTANT1], constants.final_c1);
	color_to_vec4(rs[D3DRS_FOGCOLOR], constants.fog_color);
	constants.fog_parameters[0] = dword_to_float(rs[D3DRS_FOGSTART]);
	constants.fog_parameters[1] = dword_to_float(rs[D3DRS_FOGEND]);
	constants.fog_parameters[2] = dword_to_float(rs[D3DRS_FOGDENSITY]);
	constants.alpha_reference = (float)(rs[D3DRS_ALPHAREF] & 0xff);
	color_to_vec4(rs[D3DRS_BLENDCOLOR], constants.blend_color);
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		constants.bump_luminance[stage][0] = dword_to_float(D3D__TextureState[stage][D3DTSS_BUMPENVLSCALE]);
		constants.bump_luminance[stage][1] = dword_to_float(D3D__TextureState[stage][D3DTSS_BUMPENVLOFFSET]);
	}
	memcpy(blend_color, constants.blend_color, sizeof(blend_color));

	entry = plan_entry_get(&plan_key);
	if (!entry)
		return;
	variant = plan_get(entry, &constants);
	plan = &variant->plan;
	plan_analyze(variant, &plan_key.state, &constants);

	stats_lap(&stats.plan_time);
	/* ----- the textures the plan's units bind. All of them before any
	unit is set up: an upload, a copy of the back buffer and a derived
	texture each bind on whatever unit is active. */
	for (pass_index = 0; pass_index < plan->pass_count; pass_index++)
	{
		const struct nv2a_ff_pass *pass = &plan->passes[pass_index];

		stages_wanted |= pass_stages(pass);
		for (unit = 0; unit < pass->unit_count && unit < NV2A_FF_MAXIMUM_UNITS; unit++)
		{
			const struct nv2a_ff_unit *plan_unit = &pass->units[unit];
			struct xgpu_fixed_texture *texture = &unit_textures[pass_index][unit];

			stage = plan_unit->stage;
			if (stage >= NV2A_FF_STAGE_COUNT)
			{
				xgpu_fixed_white_texture(texture);
				continue;
			}
			if (!texture_fetched[stage])
			{
				/* a draw that would sample what was never drawn is left
				out with it: the game's shadows, glows and screen effects */
				if (texture_types[stage] == _xgpu_fixed_texture_off_screen ?
					!corner_sample(draw, stage, &textures[stage]) : !xgpu_fixed_texture_get(draw, stage, &textures[stage]))
				{
					if (texture_types[stage] == _xgpu_fixed_texture_off_screen &&
						xgpu_fixed_off_screen_sampled(draw->textures[stage]->Data))
					{
						platform_log("gl1: what samples the off-screen target %08lx is left out (vertex program %lu, "
							"stage %d)", (unsigned long)draw->textures[stage]->Data, draw->program_id, stage);
					}
					stats.skipped_texture++;
					return;
				}
				texture_fetched[stage] = TRUE;
			}
			*texture = textures[stage];
			if (plan_unit->bake != NV2A_FF_BAKE_NONE && plan_unit->bake < plan->bake_count &&
				texture->type == _xgpu_fixed_texture_2d && !variant->fallback)
			{
				const D3DCOLOR *palette = draw->palettes[stage] && draw->palettes[stage]->Data ?
					(const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(draw->palettes[stage]->Data) : NULL;

				xgpu_fixed_bake_get(texture, palette, plan, plan_unit->bake, &constants);
			}
		}
	}

	stats_lap(&stats.texture_time);
	/* ----- the vertices */
	if (draw->indices)
	{
		unsigned long minimum = 0xffff, maximum = 0, index;

		for (index = 0; index < draw->count; index++)
		{
			if (draw->indices[index] < minimum)
				minimum = draw->indices[index];
			if (draw->indices[index] > maximum)
				maximum = draw->indices[index];
		}
		range.first = minimum;
		range.count = maximum - minimum + 1;
		/* the game's indices are drawn as they are */
		range.base = minimum;
	}
	else
	{
		range.first = draw->first;
		range.count = draw->count;
		/* (OpenGL reads a lifted draw's arrays from the stream's own first
		vertex: the evaluator's arrays are numbered the same) */
		range.base = lifted ? draw->first : 0;
	}
	if (!array_reserve(&arrays.position, &arrays.position_size, range.base + range.count, 4 * sizeof(float)) ||
		!array_reserve(&arrays.color, &arrays.color_size, range.base + range.count, 4) ||
		!array_reserve(&arrays.v0, &arrays.v0_size, range.base + range.count, 4) ||
		!array_reserve(&arrays.v1, &arrays.v1_size, range.base + range.count, 4) ||
		!array_reserve(&arrays.fog, &arrays.fog_size, range.base + range.count, sizeof(float)))
	{
		return;
	}
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		if ((stages_wanted & (1u << stage)) && !array_reserve(&arrays.coordinates[stage],
			&arrays.coordinates_size[stage], range.base + range.count, 4 * sizeof(float)))
		{
			return;
		}
	}

	/* the primaries: with the colours and fog that are the same at every
	vertex of this draw put in, where a primary reads one */
	primaries = variant->primaries;
	memset(&fixed, 0, sizeof(fixed));
	if (!variant->fallback)
	{
		fixed.mask = fixed_inputs(draw, classification) & plan_depends(variant);
		if (fixed.mask && fixed_values(draw, fixed_program, &plan_key.state, &constants, &fixed) &&
			plan_analyze_fixed(variant, &plan_key.state, &constants, &fixed))
		{
			primaries = variant->fixed_primaries;
		}
		else
		{
			memset(&fixed, 0, sizeof(fixed));
		}
	}

	/* ----- one run of the program for everything the passes read: the
	position, the coordinates of every stage a unit samples, an array for
	each distinct primary colour the evaluator can write, and the program's
	own colours and fog where a primary is computed here. (A run costs the
	same whatever it writes: the program's arithmetic is most of it.) */
	evaluation_clear(&evaluation);
	/* (a lifted draw's positions are measured for a visibility test alone) */
	evaluation.position = !lifted || visibility.active;
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		lifted_coordinates[stage].lifted = FALSE;
		if (!(stages_wanted & (1u << stage)))
			continue;
		if (!lifted || !lifted_stage(fixed_program, draw, elements, stage, &lifted_coordinates[stage]))
			evaluation_stage(&evaluation, stage);
	}
	/* (the built-in plan with a program that writes no colour: white) */
	if (variant->fallback)
		primaries[0].kind = classification->outputs[_nv2a_vsh_output_diffuse].written ? _primary_output : _primary_none;
	for (pass_index = 0; pass_index < plan->pass_count; pass_index++)
	{
		const struct primary *primary = &primaries[pass_index];

		pass_color[pass_index] = -1;
		if (primary->kind == _primary_output)
		{
			int slot;

			/* (passes whose primaries are the same share an array) */
			for (slot = 0; slot < color_count; slot++)
			{
				if (!memcmp(color_primaries[slot], primary, sizeof(*primary)))
					break;
			}
			if (slot == color_count)
			{
				if (!array_reserve(&arrays.colors[slot], &arrays.colors_size[slot], range.base + range.count, 4))
					return;
				color_primaries[color_count++] = primary;
				evaluation_color(&evaluation, primary, arrays.colors[slot]);
			}
			pass_color[pass_index] = slot;
		}
		else if (primary->kind == _primary_general)
		{
			need_v0 = need_v0 || primary->uses_v0;
			need_v1 = need_v1 || primary->uses_v1;
			evaluation.fog = evaluation.fog || primary->uses_fog;
		}
	}
	if (need_v0)
		evaluation_vertex_color(&evaluation, arrays.v0, _nv2a_vsh_output_diffuse);
	if (need_v1)
		evaluation_vertex_color(&evaluation, arrays.v1, _nv2a_vsh_output_specular);
	if ((evaluation.position || evaluation.stages[0] >= 0 || evaluation.color_count || evaluation.fog) &&
		!evaluate(draw, program, &range, &evaluation))
	{
		/* (the last one reported is remembered: there is none in the game) */
		if (program_reported != draw->program_id)
		{
			program_reported = draw->program_id;
			platform_log("gl1: vertex program %lu cannot be run; its draws are left out", draw->program_id);
		}
		stats.skipped_program++;
		return;
	}
	for (unit = 0; unit < color_count; unit++)
	{
		primary_bias(color_primaries[unit], arrays.colors[unit], &range);
		color_uniform[unit] = color_array_uniform(arrays.colors[unit], &range, color_values[unit]);
	}

	stats_lap(&stats.vertex_time);
	/* ----- state every pass shares */
	apply_raster_state(draw, target, has_depth, has_stencil);
	if (visibility.active)
		visibility_measure(draw, &range, state.viewport, depth_test);
	if (lifted)
	{
		const struct xgpu_fixed_element *position = elements[classification->position_input];

		state_projection(projection);
		state_vertex_pointer(position_size, GL_FLOAT, (GLsizei)position->stride, position->data);
	}
	else
	{
		state_projection(NULL);
		state_vertex_pointer(4, GL_FLOAT, 4 * sizeof(float), arrays.position);
	}
	mode = primitive_mode(draw->primitive);

	/* With compiled vertex arrays the driver may transform the vertices
	once for all passes. Only the positions stay as they are from pass to
	pass, so only they are locked: the colour and the coordinate arrays
	are off when the lock is made and each pass turns its own on after it
	(the way Quake III's renderer uses the extension, which is the use the
	drivers of the time were written for). A draw whose indices pick a few
	vertices out of a long range is not locked: the driver would transform
	the whole range. */
	lockable = xgpu_fixed_context.compiled_vertex_array && plan->pass_count > 1 && range.count <= 2 * draw->count;
	if (lockable)
	{
		if (state.color_array != 0)
		{
			state.color_array = 0;
			glDisableClientState(GL_COLOR_ARRAY);
			memset(state.color, 0xff, sizeof(state.color));
		}
		for (unit = 0; unit < xgpu_fixed_context.units; unit++)
			state_unit_coordinates(unit, 0, 0, 0, NULL);
		glLockArraysEXT((GLint)range.base, (GLsizei)range.count);
		locked = TRUE;
	}

	stats.draws++;
	stats.vertices += range.count;
	if (lifted)
		stats.lifted_draws++;
	if (variant->fallback)
		stats.fallback_draws++;
	else if (plan->status != _nv2a_ff_plan_exact)
		stats.approximate_draws++;
	if (stats_timed && !variant->fallback && plan->status != _nv2a_ff_plan_exact &&
		!(variant->bound ? entry->described_bound : entry->described))
	{
		char text[512];

		/* each state's approximate plan once (of its plans for constants,
		the first): what the picture lacks */
		if (variant->bound)
			entry->described_bound = TRUE;
		else
			entry->described = TRUE;
		plan_approximations_describe(plan, text, sizeof(text));
		platform_log("gl1: state %08lx%s is drawn in %d pass%s without: %s (errors of %.0f/255 on average, %.0f at most)",
			nv2a_ff_state_hash(&plan_key.state), variant->bound ? " (for its constants of the moment)" : "",
			plan->pass_count, plan->pass_count == 1 ? "" : "es", *text ? text : "nothing named",
			plan->mean_error, plan->maximum_error);
	}
	if (draw->trace)
	{
		char text[2048];

		if (variant->fallback)
			strcpy(text, "the built-in plan");
		else
			nv2a_ff_plan_describe(plan, text, sizeof(text));
		platform_log("gl1 draw: vs %lu type %d count %lu vertices %lu..%lu%s state %08lx%s%s, %d pass%s:\n%s",
			draw->program_id, (int)draw->primitive, draw->count, range.first, range.first + range.count - 1,
			lifted ? " (lifted)" : "", nv2a_ff_state_hash(&plan_key.state), variant->bound ? " (for these constants)" : "",
			plan->status == _nv2a_ff_plan_exact ? "" : " approximate", plan->pass_count,
			plan->pass_count == 1 ? "" : "es", text);
	}

	/* ----- the passes */
	for (pass_index = 0; pass_index < plan->pass_count; pass_index++)
	{
		const struct nv2a_ff_pass *pass = &plan->passes[pass_index];
		const struct primary *primary = &primaries[pass_index];
		int unit_count = pass->unit_count < xgpu_fixed_context.units ? pass->unit_count : xgpu_fixed_context.units;
		unsigned char color_mask;

		/* the primary colour */
		if (primary->kind == _primary_output && pass_color[pass_index] >= 0)
		{
			/* (one colour for the draw where the array holds one) */
			if (color_uniform[pass_color[pass_index]])
				state_color(NULL, color_values[pass_color[pass_index]]);
			else
				state_color(arrays.colors[pass_color[pass_index]], NULL);
		}
		else if (primary->kind == _primary_general)
		{
			const unsigned char *v0 = arrays.v0 + range.base * 4, *v1 = arrays.v1 + range.base * 4;
			unsigned char *color = arrays.color + range.base * 4;
			struct nv2a_ff_values values;
			struct timespec primary_start;
			unsigned long index;

			stats.general_primaries++;
			if (!primary->polynomial && !primary->program)
				stats.interpreted_primaries++;
			if (stats_timed)
				clock_gettime(CLOCK_MONOTONIC, &primary_start);
			memset(&values, 0, sizeof(values));
			values.constants = &constants;
			values.fog_factor = 1.0f;
			if (fixed.mask & (1u << _input_fog))
				values.fog_factor = fixed.fog;
			for (index = 0; index < range.count; index++, color += 4)
			{
				float vertex_v0[4], vertex_v1[4], result[4];
				int channel;

				/* (neighbouring vertices often have the same colours: the
				last result stands for them) */
				if (index && (!primary->uses_v0 || !memcmp(&v0[index * 4], &v0[index * 4 - 4], 4)) &&
					(!primary->uses_v1 || !memcmp(&v1[index * 4], &v1[index * 4 - 4], 4)) &&
					(!primary->uses_fog || arrays.fog[range.base + index] == arrays.fog[range.base + index - 1]))
				{
					memcpy(color, color - 4, 4);
					continue;
				}
				/* (what the primary does not change with is the same at
				every vertex, or not read) */
				for (channel = 0; channel < 4; channel++)
				{
					vertex_v0[channel] = primary->uses_v0 ? (float)v0[index * 4 + channel] * (1.0f / 255.0f) :
						fixed.v0[channel];
					vertex_v1[channel] = primary->uses_v1 ? (float)v1[index * 4 + channel] * (1.0f / 255.0f) :
						fixed.v1[channel];
				}
				if (primary->uses_fog)
					values.fog_factor = fog_factor_from_coordinate(&plan_key.state, &constants,
						arrays.fog[range.base + index]);
				if (primary->polynomial)
				{
					for (channel = 0; channel < 4; channel++)
					{
						float variables[NUMBER_OF_PRIMARY_VARIABLES];

						primary_variables(channel, vertex_v0, vertex_v1, values.fog_factor, variables);
						color[channel] = unit_byte(primary_polynomial(primary, channel, variables));
					}
					continue;
				}
				if (primary->program)
				{
					primary_program_run(primary->program, vertex_v0, vertex_v1, values.fog_factor, result);
				}
				else
				{
					values.v0 = vertex_v0;
					values.v1 = vertex_v1;
					nv2a_ff_plan_primary(plan, pass_index, &values, result);
				}
				for (channel = 0; channel < 4; channel++)
					color[channel] = unit_byte(result[channel]);
			}
			if (stats_timed)
			{
				struct timespec primary_end;

				clock_gettime(CLOCK_MONOTONIC, &primary_end);
				stats.primary_time += microseconds_between(&primary_start, &primary_end);
			}
			state_color(arrays.color, NULL);
		}
		else if (primary->kind == _primary_constant)
		{
			state_color(NULL, primary->constant);
		}
		else
		{
			static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

			state_color(NULL, white);
		}

		/* the units */
		for (unit = 0; unit < unit_count; unit++)
		{
			const struct nv2a_ff_unit *plan_unit = &pass->units[unit];
			const struct xgpu_fixed_texture *texture = &unit_textures[pass_index][unit];
			float constant[4];
			BOOL reads_constant = !variant->fallback && (plan_unit->constant_rgb != NV2A_FF_EXPRESSION_NONE ||
				plan_unit->constant_alpha != NV2A_FF_EXPRESSION_NONE);

			stage = plan_unit->stage;
			state_unit_enable(unit, TRUE);
			state_unit_texture(unit, texture->name);
			state_unit(unit);
			xgpu_fixed_texture_parameters(texture, stage < NV2A_FF_STAGE_COUNT ? stage : 0);
			if (reads_constant)
				nv2a_ff_plan_constant(plan, pass_index, unit, &constants, constant);
			unit_environment(unit, plan_unit, reads_constant ? constant : NULL, texture->base);
			if (stage < NV2A_FF_STAGE_COUNT && lifted_coordinates[stage].lifted)
			{
				const struct lifted_coordinates *coordinates = &lifted_coordinates[stage];
				float matrix[16];

				texture_matrix_compose(texture, coordinates->matrix, matrix);
				state_unit_matrix_full(unit, matrix);
				state_unit_coordinates(unit, coordinates->size, coordinates->type, coordinates->stride, coordinates->pointer);
			}
			else
			{
				state_unit_matrix(unit, texture->scale, texture->offset);
				if (stage < NV2A_FF_STAGE_COUNT)
				{
					state_unit_coordinates(unit, stage_projective(classification, stage) ? 4 : 2, GL_FLOAT, 4 * sizeof(float),
						arrays.coordinates[stage]);
				}
				else
					state_unit_coordinates(unit, 0, 0, 0, NULL);
			}
		}
		for (; unit < xgpu_fixed_context.units; unit++)
		{
			state_unit_enable(unit, FALSE);
			state_unit_coordinates(unit, 0, 0, 0, NULL);
		}

		/* alpha test */
		state_enable(&state.alpha_test, GL_ALPHA_TEST, pass->alpha_test_function != 0);
		if (pass->alpha_test_function)
		{
			float reference = constants.alpha_reference / 255.0f;

			if (pass->alpha_reference != NV2A_FF_EXPRESSION_NONE && !variant->fallback)
			{
				struct nv2a_ff_values values;
				float value[3];

				memset(&values, 0, sizeof(values));
				values.constants = &constants;
				nv2a_ff_plan_evaluate(plan, pass->alpha_reference, &values, value);
				reference = value[0] < 0.0f ? 0.0f : value[0] > 1.0f ? 1.0f : value[0];
			}
			if (state.alpha_function != pass->alpha_test_function || state.alpha_reference != reference)
			{
				state.alpha_function = pass->alpha_test_function;
				state.alpha_reference = reference;
				glAlphaFunc(pass->alpha_test_function, reference);
			}
		}
		if (pass->fog_enable && !fog_reported)
		{
			fog_reported = TRUE;
			platform_log("gl1: a plan asks for OpenGL's own fog, which is not used: such draws go without fog");
		}

		/* blend, colour mask, depth */
		pass_blend(pass, blend_color);
		{
			DWORD write = rs[D3DRS_COLORWRITEENABLE];

			color_mask = (unsigned char)((pass->write_color ?
				((write & D3DCOLORWRITEENABLE_RED) ? 1 : 0) | ((write & D3DCOLORWRITEENABLE_GREEN) ? 2 : 0) |
				((write & D3DCOLORWRITEENABLE_BLUE) ? 4 : 0) : 0) |
				(pass->write_alpha && (write & D3DCOLORWRITEENABLE_ALPHA) ? 8 : 0));
		}
		if (state.color_mask != color_mask)
		{
			state.color_mask = color_mask;
			glColorMask((color_mask & 1) != 0, (color_mask & 2) != 0, (color_mask & 4) != 0, (color_mask & 8) != 0);
		}
		if (depth_test)
		{
			/* a later pass of a plan draws where the first one did */
			GLenum function = pass->depth_equal ? GL_EQUAL : rs[D3DRS_ZFUNC] ? (GLenum)rs[D3DRS_ZFUNC] : GL_NEVER;

			if (state.depth_function != function)
			{
				state.depth_function = function;
				glDepthFunc(function);
			}
		}
		{
			unsigned char mask = depth_test && rs[D3DRS_ZWRITEENABLE] && !pass->depth_equal ? 1 : 0;

			if (state.depth_mask != mask)
			{
				state.depth_mask = mask;
				glDepthMask(mask ? GL_TRUE : GL_FALSE);
			}
		}

		if (draw->indices)
			glDrawElements(mode, (GLsizei)draw->count, GL_UNSIGNED_SHORT, draw->indices);
		else
			glDrawArrays(mode, (GLint)range.base, (GLsizei)draw->count);
		stats.passes++;
	}
	if (locked)
		glUnlockArraysEXT();
	if (rs[D3DRS_COLORWRITEENABLE])
		xgpu_fixed_back_buffer_drawn();
	stats_lap(&stats.pass_time);
}

/* ---------- presentation */

void xgpu_fixed_present(const struct xgpu_fixed_target *target)
{
	static int statistics = -1;

	corner_end();
	window_geometry_note(target);
	visibility_count(target);
	if (statistics < 0)
	{
		statistics = config_boolean("debug.gpu_stats");
		stats_timed = statistics != 0;
		clock_gettime(CLOCK_MONOTONIC, &stats_frames_clock);
	}
	stats.frames++;
	if (statistics)
	{
		struct timespec frames_now;
		long frame_time;

		/* every few seconds, whatever the frame rate */
		clock_gettime(CLOCK_MONOTONIC, &frames_now);
		if (frames_now.tv_sec - stats_frames_clock.tv_sec < STATISTICS_SECONDS)
			return;
		frame_time = (long)(((frames_now.tv_sec - stats_frames_clock.tv_sec) * 1000000LL +
			(frames_now.tv_nsec - stats_frames_clock.tv_nsec) / 1000) / (long)stats.frames);

		platform_log("gl1: %lu frames, %ld.%ld ms a frame; in the draws: %lu.%lu ms plans, %lu.%lu ms textures, "
			"%lu.%lu ms vertices, %lu.%lu ms passes (OpenGL); of these %lu.%lu ms in the evaluator (%lu runs over "
			"%lu vertices a frame) and %lu.%lu ms in colours computed vertex by vertex", stats.frames,
			frame_time / 1000, frame_time / 100 % 10,
			stats.plan_time / stats.frames / 1000, stats.plan_time / stats.frames / 100 % 10,
			stats.texture_time / stats.frames / 1000, stats.texture_time / stats.frames / 100 % 10,
			stats.vertex_time / stats.frames / 1000, stats.vertex_time / stats.frames / 100 % 10,
			stats.pass_time / stats.frames / 1000, stats.pass_time / stats.frames / 100 % 10,
			stats.evaluator_time / stats.frames / 1000, stats.evaluator_time / stats.frames / 100 % 10,
			stats.evaluator_runs / stats.frames, stats.evaluator_vertices / stats.frames,
			stats.primary_time / stats.frames / 1000, stats.primary_time / stats.frames / 100 % 10);
		clock_gettime(CLOCK_MONOTONIC, &stats_frames_clock);
		platform_log("gl1: %lu draws, %lu passes, %lu vertices a frame; of %lu draws %lu lifted (OpenGL transforms them), "
			"%lu approximate, %lu built-in, "
			"%lu with colours computed vertex by vertex (%lu of them by the plan's expressions); left out "
			"%lu off-screen, %lu sampling off-screen, "
			"%lu no program; %lu plans made, %lu textures uploaded, %lu derived, %lu back buffer copies, "
			"%lu visibility tests",
			stats.draws / stats.frames, stats.passes / stats.frames, stats.vertices / stats.frames, stats.draws,
			stats.lifted_draws, stats.approximate_draws, stats.fallback_draws, stats.general_primaries, stats.interpreted_primaries,
			stats.skipped_target,
			stats.skipped_texture, stats.skipped_program, stats.plans_compiled, xgpu_fixed_texture_uploads,
			xgpu_fixed_texture_bakes, xgpu_fixed_back_buffer_copies, stats.visibility_tests);
		memset(&stats, 0, sizeof(stats));
		xgpu_fixed_texture_uploads = xgpu_fixed_texture_bakes = xgpu_fixed_back_buffer_copies = 0;
	}
}

void xgpu_fixed_frame_begin(void)
{
	bound_compiles_this_frame = 0;
	/* the swap leaves the back buffer undefined */
	xgpu_fixed_back_buffer_drawn();
	xgpu_fixed_textures_frame_begin();
	xgpu_fixed_state_invalidate();
}

#endif
