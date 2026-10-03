/*
GL.H

OpenGL entry points used by the renderer, resolved at run time through
SDL_GL_GetProcAddress once the context exists (gl_functions_load).
*/

#ifndef __HALO_LINUX_GL_H
#define __HALO_LINUX_GL_H

#ifdef HALO_XBOX
/* the Xbox build has no OpenGL: xgpu.h only needs the type names */
typedef unsigned int GLuint;
typedef unsigned int GLenum;
#else

/* prototypes are declared only to give each pointer its exact type */
#define GL_GLEXT_PROTOTYPES 1
/* the XDK defines APIENTRY as __stdcall; OpenGL on Linux uses cdecl (on
Windows it is __stdcall too, and SDL would include windows.h without it) */
#pragma push_macro("APIENTRY")
#ifndef _WIN32
#undef APIENTRY
#endif
#ifdef HALO_ANDROID
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#define GLAPIENTRY GL_APIENTRY
#elif defined(HALO_WIN9X)
/* Windows 98/ME has no SDL: OpenGL 1.1 (with its fixed-function names) and
Khronos' extension header, from port/third_party/khronos, whose directory
must be on the include path for glext.h's <KHR/khrplatform.h>. WINGDIAPI
and APIENTRY defined keep gl.h from including the host's windows.h */
#pragma push_macro("WINGDIAPI")
#ifndef WINGDIAPI
#define WINGDIAPI
#endif
#include "../../third_party/khronos/GL/gl.h"
#include "../../third_party/khronos/GL/glext.h"
#pragma pop_macro("WINGDIAPI")
#define GLAPIENTRY APIENTRY
#else
#include <SDL3/SDL_opengl.h>
#endif
#pragma pop_macro("APIENTRY")

#ifdef HALO_ANDROID
/* OpenGL ES 3.2 (port/android/README.md); tools/android_gl_stubs.py reads
this list to generate the guest's entry points */
/* ANDROID_GL_FUNCTIONS_BEGIN */
#define GL_FUNCTIONS(X) \
	X(glGetString) \
	X(glGetIntegerv) \
	X(glCopyImageSubData) \
	X(glGenerateMipmap) \
	X(glGetError) \
	X(glEnable) \
	X(glDisable) \
	X(glViewport) \
	X(glDepthRangef) \
	X(glScissor) \
	X(glClearColor) \
	X(glClearDepthf) \
	X(glClearStencil) \
	X(glClear) \
	X(glColorMask) \
	X(glDepthMask) \
	X(glDepthFunc) \
	X(glStencilFunc) \
	X(glStencilOp) \
	X(glStencilMask) \
	X(glBlendFunc) \
	X(glBlendEquation) \
	X(glBlendColor) \
	X(glCullFace) \
	X(glFrontFace) \
	X(glPolygonOffset) \
	X(glLineWidth) \
	X(glPixelStorei) \
	X(glReadPixels) \
	X(glFinish) \
	X(glFlush) \
	X(glGenTextures) \
	X(glDeleteTextures) \
	X(glBindTexture) \
	X(glActiveTexture) \
	X(glTexImage2D) \
	X(glTexImage3D) \
	X(glTexSubImage2D) \
	X(glCompressedTexImage2D) \
	X(glCompressedTexImage3D) \
	X(glTexParameteri) \
	X(glTexParameteriv) \
	X(glTexParameterf) \
	X(glTexParameterfv) \
	X(glGenSamplers) \
	X(glBindSampler) \
	X(glSamplerParameteri) \
	X(glSamplerParameterf) \
	X(glSamplerParameterfv) \
	X(glGenFramebuffers) \
	X(glDeleteFramebuffers) \
	X(glBindFramebuffer) \
	X(glFramebufferTexture2D) \
	X(glCheckFramebufferStatus) \
	X(glBlitFramebuffer) \
	X(glDrawBuffers) \
	X(glReadBuffer) \
	X(glInvalidateFramebuffer) \
	X(glGenBuffers) \
	X(glDeleteBuffers) \
	X(glBindBuffer) \
	X(glBufferData) \
	X(glBufferSubData) \
	X(glBindBufferBase) \
	X(glBindBufferRange) \
	X(glGenVertexArrays) \
	X(glBindVertexArray) \
	X(glEnableVertexAttribArray) \
	X(glDisableVertexAttribArray) \
	X(glVertexAttribPointer) \
	X(glVertexAttribIPointer) \
	X(glVertexAttrib4fv) \
	X(glVertexAttribI4ui) \
	X(glDrawArrays) \
	X(glDrawElements) \
	X(glDrawElementsBaseVertex) \
	X(glCreateShader) \
	X(glShaderSource) \
	X(glCompileShader) \
	X(glGetShaderiv) \
	X(glGetShaderInfoLog) \
	X(glDeleteShader) \
	X(glCreateProgram) \
	X(glAttachShader) \
	X(glBindAttribLocation) \
	X(glLinkProgram) \
	X(glGetProgramiv) \
	X(glGetProgramInfoLog) \
	X(glUseProgram) \
	X(glGetUniformLocation) \
	X(glUniform1i) \
	X(glUniform1iv) \
	X(glUniform1f) \
	X(glUniform4fv) \
	X(glUniform2f) \
	X(glGenQueries) \
	X(glBeginQuery) \
	X(glEndQuery) \
	X(glGetQueryObjectuiv)
/* ANDROID_GL_FUNCTIONS_END */
#else
/* The desktop's entry points, in three lists (gl_functions.c).

OpenGL 1.1's, which every renderer uses and opengl32.dll itself exports on
Windows: */
#define GL_FUNCTIONS_BASE(X) \
	X(glGetString) \
	X(glGetIntegerv) \
	X(glGetTexImage) \
	X(glGetError) \
	X(glEnable) \
	X(glDisable) \
	X(glViewport) \
	X(glDepthRange) \
	X(glScissor) \
	X(glClearColor) \
	X(glClearDepth) \
	X(glClearStencil) \
	X(glClear) \
	X(glColorMask) \
	X(glDepthMask) \
	X(glDepthFunc) \
	X(glStencilFunc) \
	X(glStencilOp) \
	X(glStencilMask) \
	X(glBlendFunc) \
	X(glCullFace) \
	X(glFrontFace) \
	X(glPolygonMode) \
	X(glPolygonOffset) \
	X(glLineWidth) \
	X(glPixelStorei) \
	X(glReadPixels) \
	X(glFinish) \
	X(glFlush) \
	X(glGenTextures) \
	X(glDeleteTextures) \
	X(glBindTexture) \
	X(glTexImage2D) \
	X(glTexSubImage2D) \
	X(glCopyTexSubImage2D) \
	X(glTexParameteri) \
	X(glTexParameteriv) \
	X(glTexParameterf) \
	X(glTexParameterfv) \
	X(glDrawBuffer) \
	X(glReadBuffer) \
	X(glDrawArrays) \
	X(glDrawElements)

/* OpenGL 1.2 to 2.0's, which the renderers with shaders use (gl4 and gl2,
xgpu.h); a driver without them leaves the fixed-function renderer: */
#define GL_FUNCTIONS_SHADERS(X) \
	X(glBlendEquation) \
	X(glBlendColor) \
	X(glActiveTexture) \
	X(glTexImage3D) \
	X(glCompressedTexImage2D) \
	X(glCompressedTexImage3D) \
	X(glDrawBuffers) \
	X(glGenBuffers) \
	X(glDeleteBuffers) \
	X(glBindBuffer) \
	X(glBufferData) \
	X(glBufferSubData) \
	X(glEnableVertexAttribArray) \
	X(glDisableVertexAttribArray) \
	X(glVertexAttribPointer) \
	X(glVertexAttrib4fv) \
	X(glCreateShader) \
	X(glShaderSource) \
	X(glCompileShader) \
	X(glGetShaderiv) \
	X(glGetShaderInfoLog) \
	X(glDeleteShader) \
	X(glCreateProgram) \
	X(glAttachShader) \
	X(glBindAttribLocation) \
	X(glLinkProgram) \
	X(glGetProgramiv) \
	X(glGetProgramInfoLog) \
	X(glDeleteProgram) \
	X(glUseProgram) \
	X(glGetUniformLocation) \
	X(glUniform1i) \
	X(glUniform1iv) \
	X(glUniform1f) \
	X(glUniform4fv) \
	X(glUniform2f) \
	X(glGenQueries) \
	X(glBeginQuery) \
	X(glEndQuery) \
	X(glGetQueryObjectuiv)

/* And what came after OpenGL 2.0: all of it there for gl4, each of it done
without by gl2 (xgpu_capabilities, xgpu.h). The second name is the entry
point of the extension that came before the core function, with the same
arguments, or 0: EXT_framebuffer_object and EXT_framebuffer_blit. A pointer
that is not null does not mean the context has the function (GLX answers
for every name): the version and the extension list decide. */
#define GL_FUNCTIONS_OPTIONAL(X) \
	X(glCopyImageSubData, 0) \
	X(glGenerateMipmap, "glGenerateMipmapEXT") \
	X(glClipControl, 0) \
	X(glGenSamplers, 0) \
	X(glBindSampler, 0) \
	X(glSamplerParameteri, 0) \
	X(glSamplerParameterf, 0) \
	X(glSamplerParameterfv, 0) \
	X(glGenFramebuffers, "glGenFramebuffersEXT") \
	X(glDeleteFramebuffers, "glDeleteFramebuffersEXT") \
	X(glBindFramebuffer, "glBindFramebufferEXT") \
	X(glFramebufferTexture2D, "glFramebufferTexture2DEXT") \
	X(glCheckFramebufferStatus, "glCheckFramebufferStatusEXT") \
	X(glGenRenderbuffers, "glGenRenderbuffersEXT") \
	X(glDeleteRenderbuffers, "glDeleteRenderbuffersEXT") \
	X(glBindRenderbuffer, "glBindRenderbufferEXT") \
	X(glRenderbufferStorage, "glRenderbufferStorageEXT") \
	X(glFramebufferRenderbuffer, "glFramebufferRenderbufferEXT") \
	X(glBlitFramebuffer, "glBlitFramebufferEXT") \
	X(glBufferStorage, 0) \
	X(glMapBufferRange, 0) \
	X(glBindBufferBase, 0) \
	X(glGenVertexArrays, 0) \
	X(glBindVertexArray, 0) \
	X(glVertexAttribIPointer, 0) \
	X(glVertexAttribI4ui, 0) \
	X(glDrawElementsBaseVertex, 0) \
	X(glBindFragDataLocation, 0) \
	X(glDebugMessageCallback, 0)
#endif

#define GL_DECLARE_FUNCTION(name) extern __typeof__(&name) halo_##name;
#ifdef HALO_ANDROID
GL_FUNCTIONS(GL_DECLARE_FUNCTION)
#else
#define GL_DECLARE_OPTIONAL_FUNCTION(name, older) extern __typeof__(&name) halo_##name;
GL_FUNCTIONS_BASE(GL_DECLARE_FUNCTION)
GL_FUNCTIONS_SHADERS(GL_DECLARE_FUNCTION)
GL_FUNCTIONS_OPTIONAL(GL_DECLARE_OPTIONAL_FUNCTION)
#undef GL_DECLARE_OPTIONAL_FUNCTION
#endif
#undef GL_DECLARE_FUNCTION

/* call sites use the ordinary names; gl_functions.c, which defines the
pointers, sees the declarations without these aliases */
#ifndef GL_FUNCTIONS_DEFINE
#ifdef HALO_ANDROID
#define glGetString halo_glGetString
#define glGetIntegerv halo_glGetIntegerv
#define glCopyImageSubData halo_glCopyImageSubData
#define glGenerateMipmap halo_glGenerateMipmap
#define glGetError halo_glGetError
#define glEnable halo_glEnable
#define glDisable halo_glDisable
#define glViewport halo_glViewport
#define glDepthRangef halo_glDepthRangef
#define glScissor halo_glScissor
#define glClearColor halo_glClearColor
#define glClearDepthf halo_glClearDepthf
#define glClearStencil halo_glClearStencil
#define glClear halo_glClear
#define glColorMask halo_glColorMask
#define glDepthMask halo_glDepthMask
#define glDepthFunc halo_glDepthFunc
#define glStencilFunc halo_glStencilFunc
#define glStencilOp halo_glStencilOp
#define glStencilMask halo_glStencilMask
#define glBlendFunc halo_glBlendFunc
#define glBlendEquation halo_glBlendEquation
#define glBlendColor halo_glBlendColor
#define glCullFace halo_glCullFace
#define glFrontFace halo_glFrontFace
#define glPolygonOffset halo_glPolygonOffset
#define glLineWidth halo_glLineWidth
#define glPixelStorei halo_glPixelStorei
#define glReadPixels halo_glReadPixels
#define glFinish halo_glFinish
#define glFlush halo_glFlush
#define glGenTextures halo_glGenTextures
#define glDeleteTextures halo_glDeleteTextures
#define glBindTexture halo_glBindTexture
#define glActiveTexture halo_glActiveTexture
#define glTexImage2D halo_glTexImage2D
#define glTexImage3D halo_glTexImage3D
#define glTexSubImage2D halo_glTexSubImage2D
#define glCompressedTexImage2D halo_glCompressedTexImage2D
#define glCompressedTexImage3D halo_glCompressedTexImage3D
#define glTexParameteri halo_glTexParameteri
#define glTexParameteriv halo_glTexParameteriv
#define glTexParameterf halo_glTexParameterf
#define glTexParameterfv halo_glTexParameterfv
#define glGenSamplers halo_glGenSamplers
#define glBindSampler halo_glBindSampler
#define glSamplerParameteri halo_glSamplerParameteri
#define glSamplerParameterf halo_glSamplerParameterf
#define glSamplerParameterfv halo_glSamplerParameterfv
#define glGenFramebuffers halo_glGenFramebuffers
#define glDeleteFramebuffers halo_glDeleteFramebuffers
#define glBindFramebuffer halo_glBindFramebuffer
#define glFramebufferTexture2D halo_glFramebufferTexture2D
#define glCheckFramebufferStatus halo_glCheckFramebufferStatus
#define glBlitFramebuffer halo_glBlitFramebuffer
#define glDrawBuffers halo_glDrawBuffers
#define glReadBuffer halo_glReadBuffer
#define glInvalidateFramebuffer halo_glInvalidateFramebuffer
#define glGenBuffers halo_glGenBuffers
#define glDeleteBuffers halo_glDeleteBuffers
#define glBindBuffer halo_glBindBuffer
#define glBufferData halo_glBufferData
#define glBufferSubData halo_glBufferSubData
#define glBindBufferBase halo_glBindBufferBase
#define glBindBufferRange halo_glBindBufferRange
#define glGenVertexArrays halo_glGenVertexArrays
#define glBindVertexArray halo_glBindVertexArray
#define glEnableVertexAttribArray halo_glEnableVertexAttribArray
#define glDisableVertexAttribArray halo_glDisableVertexAttribArray
#define glVertexAttribPointer halo_glVertexAttribPointer
#define glVertexAttribIPointer halo_glVertexAttribIPointer
#define glVertexAttrib4fv halo_glVertexAttrib4fv
#define glVertexAttribI4ui halo_glVertexAttribI4ui
#define glDrawArrays halo_glDrawArrays
#define glDrawElements halo_glDrawElements
#define glDrawElementsBaseVertex halo_glDrawElementsBaseVertex
#define glCreateShader halo_glCreateShader
#define glShaderSource halo_glShaderSource
#define glCompileShader halo_glCompileShader
#define glGetShaderiv halo_glGetShaderiv
#define glGetShaderInfoLog halo_glGetShaderInfoLog
#define glDeleteShader halo_glDeleteShader
#define glCreateProgram halo_glCreateProgram
#define glAttachShader halo_glAttachShader
#define glBindAttribLocation halo_glBindAttribLocation
#define glLinkProgram halo_glLinkProgram
#define glGetProgramiv halo_glGetProgramiv
#define glGetProgramInfoLog halo_glGetProgramInfoLog
#define glUseProgram halo_glUseProgram
#define glGetUniformLocation halo_glGetUniformLocation
#define glUniform1i halo_glUniform1i
#define glUniform1iv halo_glUniform1iv
#define glUniform1f halo_glUniform1f
#define glUniform4fv halo_glUniform4fv
#define glUniform2f halo_glUniform2f
#define glGenQueries halo_glGenQueries
#define glBeginQuery halo_glBeginQuery
#define glEndQuery halo_glEndQuery
#define glGetQueryObjectuiv halo_glGetQueryObjectuiv
#else
#define glGetString halo_glGetString
#define glGetIntegerv halo_glGetIntegerv
#define glGetTexImage halo_glGetTexImage
#define glCopyImageSubData halo_glCopyImageSubData
#define glGenerateMipmap halo_glGenerateMipmap
#define glGetError halo_glGetError
#define glEnable halo_glEnable
#define glDisable halo_glDisable
#define glViewport halo_glViewport
#define glDepthRange halo_glDepthRange
#define glScissor halo_glScissor
#define glClearColor halo_glClearColor
#define glClearDepth halo_glClearDepth
#define glClearStencil halo_glClearStencil
#define glClear halo_glClear
#define glColorMask halo_glColorMask
#define glDepthMask halo_glDepthMask
#define glDepthFunc halo_glDepthFunc
#define glStencilFunc halo_glStencilFunc
#define glStencilOp halo_glStencilOp
#define glStencilMask halo_glStencilMask
#define glBlendFunc halo_glBlendFunc
#define glBlendEquation halo_glBlendEquation
#define glBlendColor halo_glBlendColor
#define glCullFace halo_glCullFace
#define glFrontFace halo_glFrontFace
#define glPolygonMode halo_glPolygonMode
#define glPolygonOffset halo_glPolygonOffset
#define glLineWidth halo_glLineWidth
#define glPixelStorei halo_glPixelStorei
#define glReadPixels halo_glReadPixels
#define glFinish halo_glFinish
#define glFlush halo_glFlush
#define glClipControl halo_glClipControl
#define glGenTextures halo_glGenTextures
#define glDeleteTextures halo_glDeleteTextures
#define glBindTexture halo_glBindTexture
#define glActiveTexture halo_glActiveTexture
#define glTexImage2D halo_glTexImage2D
#define glTexImage3D halo_glTexImage3D
#define glTexSubImage2D halo_glTexSubImage2D
#define glCompressedTexImage2D halo_glCompressedTexImage2D
#define glCompressedTexImage3D halo_glCompressedTexImage3D
#define glTexParameteri halo_glTexParameteri
#define glTexParameteriv halo_glTexParameteriv
#define glTexParameterf halo_glTexParameterf
#define glTexParameterfv halo_glTexParameterfv
#define glGenSamplers halo_glGenSamplers
#define glBindSampler halo_glBindSampler
#define glSamplerParameteri halo_glSamplerParameteri
#define glSamplerParameterf halo_glSamplerParameterf
#define glSamplerParameterfv halo_glSamplerParameterfv
#define glGenFramebuffers halo_glGenFramebuffers
#define glDeleteFramebuffers halo_glDeleteFramebuffers
#define glBindFramebuffer halo_glBindFramebuffer
#define glFramebufferTexture2D halo_glFramebufferTexture2D
#define glCheckFramebufferStatus halo_glCheckFramebufferStatus
#define glBlitFramebuffer halo_glBlitFramebuffer
#define glDrawBuffers halo_glDrawBuffers
#define glGenBuffers halo_glGenBuffers
#define glDeleteBuffers halo_glDeleteBuffers
#define glBindBuffer halo_glBindBuffer
#define glBufferData halo_glBufferData
#define glBufferSubData halo_glBufferSubData
#define glBufferStorage halo_glBufferStorage
#define glMapBufferRange halo_glMapBufferRange
#define glBindBufferBase halo_glBindBufferBase
#define glGenVertexArrays halo_glGenVertexArrays
#define glBindVertexArray halo_glBindVertexArray
#define glEnableVertexAttribArray halo_glEnableVertexAttribArray
#define glDisableVertexAttribArray halo_glDisableVertexAttribArray
#define glVertexAttribPointer halo_glVertexAttribPointer
#define glVertexAttribIPointer halo_glVertexAttribIPointer
#define glVertexAttrib4fv halo_glVertexAttrib4fv
#define glVertexAttribI4ui halo_glVertexAttribI4ui
#define glDrawArrays halo_glDrawArrays
#define glDrawElements halo_glDrawElements
#define glDrawElementsBaseVertex halo_glDrawElementsBaseVertex
#define glCreateShader halo_glCreateShader
#define glShaderSource halo_glShaderSource
#define glCompileShader halo_glCompileShader
#define glGetShaderiv halo_glGetShaderiv
#define glGetShaderInfoLog halo_glGetShaderInfoLog
#define glDeleteShader halo_glDeleteShader
#define glCreateProgram halo_glCreateProgram
#define glAttachShader halo_glAttachShader
#define glBindAttribLocation halo_glBindAttribLocation
#define glBindFragDataLocation halo_glBindFragDataLocation
#define glLinkProgram halo_glLinkProgram
#define glGetProgramiv halo_glGetProgramiv
#define glGetProgramInfoLog halo_glGetProgramInfoLog
#define glUseProgram halo_glUseProgram
#define glGetUniformLocation halo_glGetUniformLocation
#define glUniform1i halo_glUniform1i
#define glUniform1iv halo_glUniform1iv
#define glUniform1f halo_glUniform1f
#define glUniform4fv halo_glUniform4fv
#define glUniform2f halo_glUniform2f
#define glGenQueries halo_glGenQueries
#define glBeginQuery halo_glBeginQuery
#define glEndQuery halo_glEndQuery
#define glGetQueryObjectuiv halo_glGetQueryObjectuiv
#define glDebugMessageCallback halo_glDebugMessageCallback
#define glCopyTexSubImage2D halo_glCopyTexSubImage2D
#define glDrawBuffer halo_glDrawBuffer
#define glReadBuffer halo_glReadBuffer
#define glDeleteProgram halo_glDeleteProgram
#define glGenRenderbuffers halo_glGenRenderbuffers
#define glDeleteRenderbuffers halo_glDeleteRenderbuffers
#define glBindRenderbuffer halo_glBindRenderbuffer
#define glRenderbufferStorage halo_glRenderbufferStorage
#define glFramebufferRenderbuffer halo_glFramebufferRenderbuffer

#endif
#endif

#ifdef HALO_ANDROID
/* returns FALSE (and logs) if a required function is missing */
int gl_functions_load(void);
#else
/* resolves the lists above. FALSE (and logs) if one of OpenGL 1.1's is
missing; the others are left null where the driver has none */
int gl_functions_load(void);
/* whether every one of OpenGL 2.0's (GL_FUNCTIONS_SHADERS) was found; if
not, *missing is the first that was not */
int gl_functions_shaders(const char **missing);
/* takes the extensions' entry points (the second names of
GL_FUNCTIONS_OPTIONAL) in place of the core ones, where the driver has
them: for a context that lists the extension and not the core feature */
void gl_functions_use_extension_names(void);
#endif

#endif /* HALO_XBOX */

#endif
