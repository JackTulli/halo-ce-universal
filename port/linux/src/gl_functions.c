/*
GL_FUNCTIONS.C

Run-time resolution of the OpenGL entry points listed in gl.h.
*/

#include "platform.h"
#define GL_FUNCTIONS_DEFINE
#include "gl.h"

#ifdef HALO_WIN9X
/* Windows 98/ME: WGL's, with opengl32.dll's own exports for OpenGL 1.1
(port/win9x/platform/w9x_gl.c) */
#include "../../win9x/platform/w9x_backend.h"
#define SDL_GL_GetProcAddress(name) w9x_gl_proc_address(name)
#else
#include <SDL3/SDL.h>
#endif

#define GL_DEFINE_FUNCTION(name) __typeof__(&name) halo_##name;
#ifdef HALO_ANDROID
GL_FUNCTIONS(GL_DEFINE_FUNCTION)

int gl_functions_load(void)
{
	int success = TRUE;

#define GL_LOAD_FUNCTION(name) \
	halo_##name = (__typeof__(halo_##name))SDL_GL_GetProcAddress(#name); \
	if (!halo_##name) \
	{ \
		platform_log("OpenGL function %s is unavailable", #name); \
		success = FALSE; \
	}
	GL_FUNCTIONS(GL_LOAD_FUNCTION)
#undef GL_LOAD_FUNCTION
	return success;
}
#else
/* the desktop's three lists (gl.h) */
#define GL_DEFINE_OPTIONAL_FUNCTION(name, older) __typeof__(&name) halo_##name;
GL_FUNCTIONS_BASE(GL_DEFINE_FUNCTION)
GL_FUNCTIONS_SHADERS(GL_DEFINE_FUNCTION)
GL_FUNCTIONS_OPTIONAL(GL_DEFINE_OPTIONAL_FUNCTION)

/* the first of OpenGL 2.0's functions that was not found, or NULL */
static const char *missing_shader_function;

/* an extension's entry point (GL_FUNCTIONS_OPTIONAL's second names) */
static void *extension_address(const char *older)
{
	return older ? (void *)SDL_GL_GetProcAddress(older) : NULL;
}

int gl_functions_load(void)
{
	int success = TRUE;

#define GL_LOAD_FUNCTION(name) \
	halo_##name = (__typeof__(halo_##name))SDL_GL_GetProcAddress(#name); \
	if (!halo_##name) \
	{ \
		platform_log("OpenGL function %s is unavailable", #name); \
		success = FALSE; \
	}
	GL_FUNCTIONS_BASE(GL_LOAD_FUNCTION)
#undef GL_LOAD_FUNCTION

	/* a driver without these has dozens of them missing: the renderer
	says so once (gl_functions_shaders) */
	missing_shader_function = NULL;
#define GL_LOAD_FUNCTION(name) \
	halo_##name = (__typeof__(halo_##name))SDL_GL_GetProcAddress(#name); \
	if (!halo_##name && !missing_shader_function) \
		missing_shader_function = #name;
	GL_FUNCTIONS_SHADERS(GL_LOAD_FUNCTION)
#undef GL_LOAD_FUNCTION

#define GL_LOAD_FUNCTION(name, older) \
	halo_##name = (__typeof__(halo_##name))SDL_GL_GetProcAddress(#name); \
	if (!halo_##name) \
		halo_##name = (__typeof__(halo_##name))extension_address(older);
	GL_FUNCTIONS_OPTIONAL(GL_LOAD_FUNCTION)
#undef GL_LOAD_FUNCTION
	return success;
}

int gl_functions_shaders(const char **missing)
{
	if (missing)
		*missing = missing_shader_function;
	return missing_shader_function == NULL;
}

void gl_functions_use_extension_names(void)
{
#define GL_LOAD_FUNCTION(name, older) \
	if (extension_address(older)) \
		halo_##name = (__typeof__(halo_##name))extension_address(older);
	GL_FUNCTIONS_OPTIONAL(GL_LOAD_FUNCTION)
#undef GL_LOAD_FUNCTION
}
#endif
