# khronos

OpenGL headers for the Windows 98/ME build (`HALO_WIN9X`), which has no SDL
to provide them (`port/linux/src/gl.h` includes these there; every other
build keeps SDL's or the system's). Its renderers load OpenGL from
`opengl32.dll` at run time, so these give only the types, the enumerants
(OpenGL 1.1's fixed-function ones included, for the light renderer) and the
prototypes the function pointers take their types from; nothing is linked.

| File | What | Origin | Licence |
|---|---|---|---|
| `GL/glext.h` | OpenGL 1.2 and later, and the extensions (`GL_GLEXT_VERSION` 20190326) | Khronos, https://github.com/KhronosGroup/OpenGL-Registry (`api/GL/glext.h`), as mingw-w64 ships it | MIT-style, in the file (`LICENSE.txt`) |
| `KHR/khrplatform.h` | the sized types `glext.h` uses | Khronos, https://github.com/KhronosGroup/EGL-Registry (`api/KHR/khrplatform.h`), as mingw-w64 ships it | MIT-style, in the file (`LICENSE.txt`) |
| `GL/gl.h` | OpenGL 1.1, which Khronos publishes no header for | mingw-w64's `mingw-w64-headers/include/GL/gl.h` | public domain (`LICENSE.txt`) |

The three files are copied unchanged from MSYS2's `mingw-w64-x86_64-headers`
package (`/mingw64/include`). `glext.h` includes `<KHR/khrplatform.h>`, so
this directory has to be on the include path of the units that include
`gl.h`. `GL/gl.h` includes `<windows.h>` unless `WINGDIAPI` and `APIENTRY`
are both defined; `port/linux/src/gl.h` defines `WINGDIAPI` around it, and
the XDK headers define `APIENTRY`.
