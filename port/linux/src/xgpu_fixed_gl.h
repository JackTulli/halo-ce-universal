/*
XGPU_FIXED_GL.H

The OpenGL entry points only the light renderer calls (xgpu_fixed.h): the
fixed-function ones of OpenGL 1.1, and those of the extensions it can use.
They are resolved when the renderer starts (xgpu_fixed.c), as gl.h's are
when the context is made, and called through pointers under their own
names.

OpenGL 1.1's are opengl32.dll's own exports on Windows and are there in any
context that is not a core profile one. The others are null without their
extension; the entry points gl.h already has that came from extensions of
that time (glActiveTexture, glBlendEquation, glBlendColor,
glCompressedTexImage2D) are filled in from the extensions' names where the
driver knows only those.
*/

#ifndef __HALO_LINUX_XGPU_FIXED_GL_H
#define __HALO_LINUX_XGPU_FIXED_GL_H

#include "gl.h"

#define GL_FUNCTIONS_FIXED(X) \
	X(glMatrixMode) \
	X(glLoadIdentity) \
	X(glLoadMatrixf) \
	X(glEnableClientState) \
	X(glDisableClientState) \
	X(glVertexPointer) \
	X(glTexCoordPointer) \
	X(glColorPointer) \
	X(glColor4f) \
	X(glTexEnvi) \
	X(glTexEnvfv) \
	X(glAlphaFunc) \
	X(glShadeModel)

/* the second name is OpenGL 1.3's for the same entry point, or 0 */
#define GL_FUNCTIONS_FIXED_OPTIONAL(X) \
	X(glClientActiveTextureARB, "glClientActiveTexture") \
	X(glLockArraysEXT, 0) \
	X(glUnlockArraysEXT, 0)

#define GL_DECLARE_FIXED_FUNCTION(name) extern __typeof__(&name) halo_##name;
#define GL_DECLARE_FIXED_OPTIONAL_FUNCTION(name, other) extern __typeof__(&name) halo_##name;
GL_FUNCTIONS_FIXED(GL_DECLARE_FIXED_FUNCTION)
GL_FUNCTIONS_FIXED_OPTIONAL(GL_DECLARE_FIXED_OPTIONAL_FUNCTION)
#undef GL_DECLARE_FIXED_FUNCTION
#undef GL_DECLARE_FIXED_OPTIONAL_FUNCTION

/* call sites use the ordinary names, as with gl.h's (xgpu_fixed.c defines
the pointers by their halo_ names) */
#define glMatrixMode halo_glMatrixMode
#define glLoadIdentity halo_glLoadIdentity
#define glLoadMatrixf halo_glLoadMatrixf
#define glEnableClientState halo_glEnableClientState
#define glDisableClientState halo_glDisableClientState
#define glVertexPointer halo_glVertexPointer
#define glTexCoordPointer halo_glTexCoordPointer
#define glColorPointer halo_glColorPointer
#define glColor4f halo_glColor4f
#define glTexEnvi halo_glTexEnvi
#define glTexEnvfv halo_glTexEnvfv
#define glAlphaFunc halo_glAlphaFunc
#define glShadeModel halo_glShadeModel
#define glClientActiveTextureARB halo_glClientActiveTextureARB
#define glLockArraysEXT halo_glLockArraysEXT
#define glUnlockArraysEXT halo_glUnlockArraysEXT

/* ---------- enumerants of the extensions, where the headers lack them */

#ifndef GL_BGRA
#define GL_BGRA 0x80e1
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#endif
#ifndef GL_UNSIGNED_SHORT_5_5_5_1
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812f
#endif
#ifndef GL_TEXTURE_MIN_LOD
#define GL_TEXTURE_MIN_LOD 0x813a
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813d
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84c0
#endif
#ifndef GL_MAX_TEXTURE_UNITS
#define GL_MAX_TEXTURE_UNITS 0x84e2
#endif
#ifndef GL_COMBINE
#define GL_COMBINE 0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_COMBINE_ALPHA 0x8572
#define GL_RGB_SCALE 0x8573
#define GL_ADD_SIGNED 0x8574
#define GL_INTERPOLATE 0x8575
#define GL_CONSTANT 0x8576
#define GL_PRIMARY_COLOR 0x8577
#define GL_PREVIOUS 0x8578
#define GL_SOURCE0_RGB 0x8580
#define GL_SOURCE0_ALPHA 0x8588
#define GL_OPERAND0_RGB 0x8590
#define GL_OPERAND0_ALPHA 0x8598
#endif
#ifndef GL_SUBTRACT
#define GL_SUBTRACT 0x84e7
#endif
#ifndef GL_DOT3_RGB
#define GL_DOT3_RGB 0x86ae
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83f1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83f2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83f3
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#define GL_MIN 0x8007
#define GL_MAX 0x8008
#define GL_FUNC_SUBTRACT 0x800a
#define GL_FUNC_REVERSE_SUBTRACT 0x800b
#endif
#ifndef GL_CONSTANT_COLOR
#define GL_CONSTANT_COLOR 0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_CONSTANT_ALPHA 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#endif

#endif
