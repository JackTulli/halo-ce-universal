/*
D3D8_NV2A.C

The Xbox Direct3D 8 device for the Xbox build: the game's calls become the
NV2A's push buffer methods, as the January build's runtime issued them
(libs/d3d8, the reconstruction of that runtime, gives each encoding), over
nxdk's pbkit, which runs the GPU, the push buffer and the display's flips
(nx_gpu.c).

The game keeps its own state in D3D__RenderState and D3D__TextureState
through the SDK's inline functions (port/include/xdk/xdk_d3d8.h): simple
render states arrive here already encoded as a method, deferred ones are
gathered and applied before a draw, the rest have functions of their own.
Every vertex shader is a program (the game has no fixed-function
geometry), and every pixel shader is a register combiner definition.

Resources (textures, vertex and index buffers, palettes) are the shared
port/linux/src/d3d8_resources.c: their Data is a physical offset, their
Format the texture format register, exactly what the hardware reads.
*/

#include "platform.h"
#include "nx_win32.h"

#include <stdlib.h>
#include <string.h>

/* ---------- state the XDK's inline functions share (xdk_d3d8.h) */

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;

/* ---------- the NV2A's methods (nxdk's nv_regs.h names them so) */

#define NV097_NO_OPERATION 0x0100
#define NV097_WAIT_FOR_IDLE 0x0110
#define NV097_SET_CONTEXT_DMA_A 0x0184
#define NV097_SET_CONTEXT_DMA_B 0x0188
#define NV097_SET_CONTEXT_DMA_COLOR 0x0194
#define NV097_SET_CONTEXT_DMA_ZETA 0x0198
#define NV097_SET_CONTEXT_DMA_SEMAPHORE 0x01A4
#define NV097_SET_CONTEXT_DMA_REPORT 0x01A8
#define NV097_SET_CONTEXT_DMA_VERTEX_A 0x019C
#define NV097_SET_CONTEXT_DMA_VERTEX_B 0x01A0
#define NV097_SET_SEMAPHORE_OFFSET 0x1D6C
#define NV097_BACK_END_WRITE_SEMAPHORE_RELEASE 0x1D70
#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL 0x0204
#define NV097_SET_SURFACE_FORMAT 0x0208
#define NV097_SET_SURFACE_PITCH 0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET 0x0210
#define NV097_SET_SURFACE_ZETA_OFFSET 0x0214
#define NV097_SET_COMBINER_ALPHA_ICW 0x0260
#define NV097_SET_COMBINER_SPECULAR_FOG_CW0 0x0288
#define NV097_SET_CONTROL0 0x0290
#define NV097_SET_FOG_MODE 0x029C
#define NV097_SET_FOG_GEN_MODE 0x02A0
#define NV097_SET_FOG_ENABLE 0x02A4
#define NV097_SET_CULL_FACE_ENABLE 0x0308
#define NV097_SET_DEPTH_TEST_ENABLE 0x030C
#define NV097_SET_LIGHTING_ENABLE 0x0314
#define NV097_SET_POINT_PARAMS_ENABLE 0x0318
#define NV097_SET_LINE_SMOOTH_ENABLE 0x0320
#define NV097_SET_SKIN_MODE 0x0328
#define NV097_SET_STENCIL_TEST_ENABLE 0x032C
#define NV097_SET_STENCIL_OP_FAIL 0x0370
#define NV097_SET_LINE_WIDTH 0x0380
#define NV097_SET_FRONT_POLYGON_MODE 0x038C
#define NV097_SET_CLIP_MIN 0x0394
#define NV097_SET_CULL_FACE 0x039C
#define NV097_SET_FRONT_FACE 0x03A0
#define NV097_SET_NORMALIZATION_ENABLE 0x03A4
#define NV097_SET_SPECULAR_ENABLE 0x03B8
#define NV097_SET_TEXGEN_S 0x03C0
#define NV097_SET_TEXTURE_MATRIX_ENABLE 0x0420
#define NV097_SET_POINT_SIZE 0x043C
#define NV097_SET_FOG_PARAMS 0x09C0
#define NV097_SET_VIEWPORT_OFFSET 0x0A20
#define NV097_SET_POINT_PARAMS 0x0A30
#define NV097_SET_COMBINER_FACTOR0 0x0A60
#define NV097_SET_COLOR_KEY_COLOR 0x0AE0
#define NV097_SET_VIEWPORT_SCALE 0x0AF0
#define NV097_SET_TRANSFORM_PROGRAM 0x0B00
#define NV097_SET_TRANSFORM_CONSTANT 0x0B80
#define NV097_SET_VERTEX4F 0x1518
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760
#define NV097_SET_LOGIC_OP_ENABLE 0x17BC
#define NV097_SET_LOGIC_OP 0x17C0
#define NV097_SET_TWO_SIDE_LIGHT_EN 0x17C4
#define NV097_CLEAR_REPORT_VALUE 0x17C8
#define NV097_SET_ZPASS_PIXEL_COUNT_ENABLE 0x17CC
#define NV097_GET_REPORT 0x17D0
#define NV097_SET_SHADER_CLIP_PLANE_MODE 0x17F8
#define NV097_SET_BEGIN_END 0x17FC
#define NV097_INVALIDATE_VERTEX_CACHE_FILE 0x1710
#define NV097_ARRAY_ELEMENT16 0x1800
#define NV097_ARRAY_ELEMENT32 0x1808
#define NV097_DRAW_ARRAYS 0x1810
#define NV097_INLINE_ARRAY 0x1818
#define NV097_SET_VERTEX_DATA2F_M 0x1880
#define NV097_SET_VERTEX_DATA2S 0x1900
#define NV097_SET_VERTEX_DATA4UB 0x1940
#define NV097_SET_VERTEX_DATA4F_M 0x1A00
#define NV097_SET_TEXTURE_OFFSET 0x1B00
#define NV097_SET_TEXTURE_ADDRESS 0x1B08
#define NV097_SET_TEXTURE_CONTROL0 0x1B0C
#define NV097_SET_TEXTURE_CONTROL1 0x1B10
#define NV097_SET_TEXTURE_FILTER 0x1B14
#define NV097_SET_TEXTURE_IMAGE_RECT 0x1B1C
#define NV097_SET_TEXTURE_PALETTE 0x1B20
#define NV097_SET_TEXTURE_BORDER_COLOR 0x1B24
#define NV097_SET_TEXTURE_SET_BUMP_ENV_MAT 0x1B28
#define NV097_SET_ANTI_ALIASING_CONTROL 0x1D7C
#define NV097_SET_OCCLUDE_ZSTENCIL_EN 0x1D84
#define NV097_SET_ZSTENCIL_CLEAR_VALUE 0x1D8C
#define NV097_SET_COLOR_CLEAR_VALUE 0x1D90
#define NV097_CLEAR_SURFACE 0x1D94
#define NV097_SET_CLEAR_RECT_HORIZONTAL 0x1D98
#define NV097_SET_COMBINER_FINAL_CONSTANT 0x1E20
#define NV097_SET_COMBINER_COLOR_OCW 0x1E40
#define NV097_SET_COMBINER_CONTROL 0x1E60
#define NV097_SET_SHADOW_DEPTH_FUNC 0x1E6C
#define NV097_SET_SHADER_STAGE_PROGRAM 0x1E70
#define NV097_SET_DOT_RGBMAPPING 0x1E74
#define NV097_SET_SHADER_OTHER_STAGE_INPUT 0x1E78
#define NV097_SET_TRANSFORM_EXECUTION_MODE 0x1E94
#define NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN 0x1E98
#define NV097_SET_TRANSFORM_PROGRAM_LOAD 0x1E9C
#define NV097_SET_TRANSFORM_PROGRAM_START 0x1EA0
#define NV097_SET_TRANSFORM_CONSTANT_LOAD 0x1EA4

#define TEXTURE_STAGE_STRIDE 64

/* values the SDK headers do not carry: no logic operation, and the
texture coordinate index's generation flags (DirectX 8's, plus the Xbox's
object and sphere map) */
#define D3DLOGICOP_NONE 0
#define D3DTSS_TCI_PASSTHRU 0x00000000
#define D3DTSS_TCI_CAMERASPACENORMAL 0x00010000
#define D3DTSS_TCI_CAMERASPACEPOSITION 0x00020000
#define D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR 0x00030000
#define D3DTSS_TCI_OBJECT 0x00040000

/* the header word: dwords, then the method; bit 30 sends every dword to
the same method */
#define METHOD(count, method) (((unsigned long)(count) << 18) | (unsigned long)(method))
#define METHOD_SAME(count, method) (METHOD(count, method) | 0x40000000UL)

/* ---------- the device */

#define VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd' */

#define VERTEX_SLOTS 16
#define SLOT_DISABLED 0x02 /* floats, none: nothing is fetched */
/* the report page's last slot holds the fence (FENCE_OFFSET); the rest
are the visibility tests', a half of 127 for each frame parity
(QUERY_SLOTS; a frame's flares number far fewer) */
#define VISIBILITY_REPORTS (NX_GPU_REPORT_BYTES / 16 - 1)
#define QUERY_SLOTS (VISIBILITY_REPORTS / 2)
#define FENCE_OFFSET (NX_GPU_REPORT_BYTES - 16)

enum
{
	DIRTY_FOG = 1,
	DIRTY_TEXTURE_STATE = 2,
	DIRTY_SHADER_STAGES = 4,
	DIRTY_VERTEX_FORMAT = 8,
	DIRTY_VERTEX_OFFSETS = 16,
	DIRTY_POINT_PARAMS = 32,
};

struct vertex_slot
{
	unsigned long stream;
	unsigned long offset;
	unsigned long size_and_type;
};

struct vertex_shader_object
{
	unsigned long signature;
	unsigned long type; /* the function's low word: 0x2078 a plain program */
	unsigned long instruction_count;
	unsigned long *program; /* 4 dwords an instruction */
	struct vertex_slot slots[VERTEX_SLOTS];
	/* the declaration's constants, already as push buffer methods */
	unsigned long constant_dwords;
	unsigned long *constants;
};

struct stream_source
{
	unsigned long data; /* physical, 0 when unset */
	unsigned long stride;
	D3DVertexBuffer *buffer; /* for its fence (mark_bound_resources) */
};

struct visibility_report
{
	unsigned long time_stamp_low, time_stamp_high;
	unsigned long samples;
	unsigned long status; /* 0xffffffff until the GPU writes the report */
};

static struct
{
	BOOL created;
	D3DPRESENT_PARAMETERS presentation;
	D3DSurface back_buffer; /* Data follows pbkit's flips */
	/* every address the back buffer has had: pbkit's frame buffers, which
	the game's own headers for "the back buffer" keep (frame_buffer_seen) */
	unsigned long frame_buffers[4];
	int frame_buffer_count;
	/* the screen the game draws: its columns, and how many of the buffer's
	each one is (a widescreen display: 854 columns in a 720-column buffer,
	which the television stretches back); viewports, clears and visibility
	counts on the screen's targets are scaled by it, and the game's
	coordinates shift right by ui_offset columns while it centres the menus
	(d3d8_nv2a_set_ui_offset). device.viewport stays in the game's columns.
	At 720p the game's 480 lines are scaled the same way (scale_y, 1.5) */
	unsigned long logical_width, logical_height;
	float scale_x, scale_y;
	long ui_offset;
	/* the device's own, in physical memory like every other surface (the
	game's screen effects copy the frame into it as a colour surface:
	rasterizer_xbox.c makes its render_primary_copy headers from the depth
	surface's address); depth is addressed through the all-memory window */
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	float z_scale;
	D3DVIEWPORT8 viewport;
	D3DMATRIX transforms[D3DTS_MAX];
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];
	struct vertex_shader_object *vertex_shader;
	struct stream_source streams[16];
	unsigned long index_base;
	unsigned long applied_index_base;
	const D3DPIXELSHADERDEF *pixel_shader;
	BOOL pixel_shader_uses_specular_fog;
	BOOL pixel_shader_adjusts_texture_modes;
	unsigned long shader_stage_program;
	unsigned long dirty;
	unsigned long constant_mode;
	struct visibility_report *reports; /* the GPU's report page (nx_gpu.c) */
	unsigned long push_buffer_size;
} device;


static D3DDevice *device_pointer(void)
{
	static int device_object;

	return (D3DDevice *)&device_object;
}

/* ---------- the push buffer */

/* The commands go into nx_gpu.c's ring: push_begin reserves room for a
block, push_end ends it. Both are inline checks against the ring's fast
path; NX_GPU_VALIDATE (a debugging build) also walks every block's headers
and its reservation, which costs a read of the ring's write-combined memory
for every header. */
#ifdef NX_GPU_VALIDATE
static unsigned long *push_start;
static unsigned long push_reserved;
#endif

static inline unsigned long *push_begin(unsigned long dwords)
{
#ifdef NX_GPU_VALIDATE
	push_start = nx_gpu_reserve(dwords);
	push_reserved = dwords;
	return push_start;
#else
	return nx_gpu_reserve(dwords);
#endif
}

#ifdef NX_GPU_VALIDATE
/* walks a block's headers: a count that does not land on the block's end,
or a header with reserved bits, is a bug here that the GPU would choke on */
static void validate_block(unsigned long *start, unsigned long *end, void *caller)
{
	static int reports;
	unsigned long *p = start;
	char text[160];

	while (p < end)
	{
		unsigned long word = *p;

		if ((word & 0xE0030003UL) != 0 && (word & 0xE0030003UL) != 0x40000000UL)
		{
			if (reports++ < 8)
			{
				snprintf(text, sizeof(text), "halo-xbox: bad GPU header %08lx at word %lu of %lu from %p (first %08lx)\n",
					word, (unsigned long)(p - start), (unsigned long)(end - start), caller, *start);
				nx_print(text);
			}
			return;
		}
		p += 1 + ((word >> 18) & 0x7FF);
	}
	if (p != end && reports++ < 8)
	{
		snprintf(text, sizeof(text), "halo-xbox: GPU block of %lu words overruns by %lu from %p (first %08lx)\n",
			(unsigned long)(end - start), (unsigned long)(p - end), caller, *start);
		nx_print(text);
	}
}
#endif

static inline void push_end(unsigned long *end)
{
#ifdef NX_GPU_VALIDATE
	if (push_start)
	{
		validate_block(push_start, end, __builtin_return_address(0));
		if ((unsigned long)(end - push_start) > push_reserved)
		{
			/* a block longer than its reservation: past the ring's tail
			it would overwrite what follows the push buffer */
			static int reports;

			if (reports++ < 8)
				platform_log("push of %lu dwords over a reservation of %lu from %p (first %08lx)", (unsigned long)(end - push_start),
					push_reserved, __builtin_return_address(0), *push_start);
		}
	}
	push_start = NULL;
#endif
	nx_gpu_end(end);
}

static unsigned long *push1(unsigned long *p, unsigned long method, unsigned long value)
{
	p[0] = METHOD(1, method);
	p[1] = value;
	return p + 2;
}

static unsigned long *push2(unsigned long *p, unsigned long method, unsigned long a, unsigned long b)
{
	p[0] = METHOD(2, method);
	p[1] = a;
	p[2] = b;
	return p + 3;
}

/* (the builtin: -fno-builtin would make memcpy a call) */
static inline unsigned long float_bits(float value)
{
	unsigned long bits;

	__builtin_memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static inline float bits_float(unsigned long bits)
{
	float value;

	__builtin_memcpy(&value, &bits, sizeof(value));
	return value;
}

static unsigned long *push4f(unsigned long *p, unsigned long method, float a, float b, float c, float d)
{
	p[0] = METHOD(4, method);
	p[1] = float_bits(a);
	p[2] = float_bits(b);
	p[3] = float_bits(c);
	p[4] = float_bits(d);
	return p + 5;
}

/* a single method and value, submitted */
static void emit1(unsigned long method, unsigned long value)
{
	unsigned long *p = push_begin(2);

	push_end(push1(p, method, value));
}

/* D3DCOLOR (ARGB) to the hardware's ABGR */
static unsigned long swap_rgb(unsigned long color)
{
	return (color & 0xff00ff00UL) | ((color & 0x00ff0000UL) >> 16) | ((color & 0x000000ffUL) << 16);
}

/* ---------- fences

The console's GPU runs a frame or two behind the CPU. As the retail
runtime did, the GPU writes a counting time into memory when it reaches a
fence in the push buffer (NV097_BACK_END_WRITE_SEMAPHORE_RELEASE: after
everything before it has drawn); each resource a draw reads carries in its
Lock word the time of the fence after that use (mark_bound_resources), and
a lock that must not overwrite what the GPU still reads waits for that
fence only (d3d8_resources.c). Before, such a lock drained the whole GPU:
two or three times a frame, for the game's per-frame vertex buffers. The
fence's word is the report page's last slot; Present pushes one a frame. */

static unsigned long fence_time; /* the last fence pushed */
static volatile unsigned long *fence_done; /* the last one the GPU passed */

static void push_fence(void)
{
	unsigned long *p = push_begin(6);

	p = push1(p, NV097_BACK_END_WRITE_SEMAPHORE_RELEASE, ++fence_time);
	/* and, as the runtime's fence, two harmless writes after it */
	p = push1(p, NV097_SET_COLOR_CLEAR_VALUE, 0);
	p = push1(p, NV097_SET_COLOR_CLEAR_VALUE, 0);
	push_end(p);
}

/* a resource the next draw reads: its use ends at the next fence */
static inline void mark_resource(void *resource)
{
	if (resource)
		((unsigned long *)resource)[2] = fence_time + 1;
}

static void mark_bound_resources(void)
{
	int index;

	for (index = 0; index < 16; index++)
		mark_resource(device.streams[index].buffer);
	for (index = 0; index < D3DTSS_MAXSTAGES; index++)
	{
		mark_resource(device.textures[index]);
		mark_resource(device.palettes[index]);
	}
}

/* the fence a resource waits for, pushed if it has not been; 0 for none.
A Lock word beyond the next fence is none of the device's (a header from
the map file, an index buffer the device never marks, whose indices go
into the push buffer): nothing on the GPU reads that resource */
static unsigned long resource_fence(void *resource)
{
	unsigned long wanted = resource ? ((unsigned long *)resource)[2] : fence_time + 1;

	if ((long)(wanted - (fence_time + 1)) > 0)
		return 0;
	if (wanted && (long)(wanted - fence_time) > 0)
	{
		push_fence();
		nx_gpu_flush();
	}
	return wanted;
}

/* waits until the GPU has passed fence wanted: a short spin, then asleep a
millisecond at a time. Spinning for the GPU held the processor from the
threads of the game's priority, the network stack's among them (with the
frame rate unlocked the CPU runs ahead of the GPU, and waits often) */
static void wait_for_fence(unsigned long wanted)
{
	unsigned long spins = 0, slept = 0;
	unsigned long long yielding_since = 0;

	nx_gpu_flush();
	while ((long)(*fence_done - wanted) < 0)
	{
		if (spins < 2000)
		{
			spins++;
			__asm__ __volatile__("pause");
			continue;
		}
		/* then the processor to the other threads for 3 ms (Present waits
		every frame, mostly less than that): a sleep wakes on the kernel's
		1 ms tick, up to 2 ms after the GPU got there */
		if (!yielding_since)
			yielding_since = nx_microseconds();
		if (nx_microseconds() - yielding_since < 3000)
		{
			nxb_SwitchToThread();
			continue;
		}
		nxb_Sleep(1);
		if (++slept == 5000)
		{
			platform_log("GPU fence %lu not reached in 5 s (the GPU is at %lu)", wanted, *fence_done);
			break;
		}
	}
}

/* d3d8_resources.c: waits until the GPU has passed the fence after the
resource's last use (NULL: everything pushed so far) */
void halo_xbox_gpu_wait_for_resource(void *resource)
{
	unsigned long wanted;

	if (!device.created)
		return;
	if (!fence_done)
	{
		nx_gpu_drain();
		return;
	}
	wanted = resource_fence(resource);
	if (!wanted)
		return;
	wait_for_fence(wanted);
	if (resource)
		((unsigned long *)resource)[2] = 0;
}

/* d3d8_resources.c: whether the GPU may still read the resource (the
texture cache spins on this before it reuses a texture's memory) */
BOOL halo_xbox_gpu_resource_busy(void *resource)
{
	unsigned long wanted;

	if (!device.created || !fence_done || !resource)
		return FALSE;
	wanted = resource_fence(resource);
	if (wanted && (long)(*fence_done - wanted) < 0)
	{
		/* (the texture cache asks again and again until it is not: a turn
		for the other threads between the questions) */
		nxb_SwitchToThread();
		return TRUE;
	}
	return FALSE;
}

/* the game's own fences (rasterizer_xbox_decals.c: a decal's vertices are
free to reuse once the GPU has passed the fence after its last draw).
halo_xbox_gpu_fence_after_draws: the fence that follows the draws pushed so
far (pushed at the next push_fence: a lock's, the frame's end, or
halo_xbox_gpu_push_fence); 0 without fences */
unsigned long halo_xbox_gpu_fence_after_draws(void)
{
	return device.created && fence_done ? fence_time + 1 : 0;
}

void halo_xbox_gpu_push_fence(void)
{
	if (device.created && fence_done)
		push_fence();
}

BOOL halo_xbox_gpu_fence_passed(unsigned long fence)
{
	if (!device.created || !fence_done || !fence || (long)(fence - (fence_time + 1)) > 0)
		return TRUE;
	return (long)(*fence_done - fence) >= 0;
}

void halo_xbox_gpu_wait_fence(unsigned long fence)
{
	if (halo_xbox_gpu_fence_passed(fence))
		return;
	if ((long)(fence - fence_time) > 0)
		push_fence();
	wait_for_fence(fence);
}

/* a vertex buffer is about to be rewritten (d3d8_resources.c): the GPU's
cache of fetched vertices forgets what it holds, from here in the stream */
void halo_xbox_gpu_invalidate_vertex_cache(void)
{
	if (device.created)
		emit1(NV097_INVALIDATE_VERTEX_CACHE_FILE, 0);
}

/* ---------- formats */

static unsigned long format_of(const D3DSurface *surface)
{
	return (surface->Format & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT;
}

static BOOL format_is_linear(unsigned long format)
{
	switch (format)
	{
	case D3DFMT_LIN_A1R5G5B5: case D3DFMT_LIN_R5G6B5: case D3DFMT_LIN_A8R8G8B8: case D3DFMT_LIN_L8:
	case D3DFMT_LIN_R8B8: case D3DFMT_LIN_G8B8: case D3DFMT_LIN_AL8: case D3DFMT_LIN_X1R5G5B5:
	case D3DFMT_LIN_A4R4G4B4: case D3DFMT_LIN_X8R8G8B8: case D3DFMT_LIN_A8: case D3DFMT_LIN_A8L8:
	case D3DFMT_LIN_D24S8: case D3DFMT_LIN_F24S8: case D3DFMT_LIN_D16: case D3DFMT_LIN_F16:
	case D3DFMT_LIN_L16: case D3DFMT_LIN_V16U16: case D3DFMT_LIN_R6G5B5: case D3DFMT_LIN_R5G5B5A1:
	case D3DFMT_LIN_R4G4B4A4: case D3DFMT_LIN_A8B8G8R8: case D3DFMT_LIN_B8G8R8A8: case D3DFMT_LIN_R8G8B8A8:
		return TRUE;
	default:
		return FALSE;
	}
}

static BOOL format_is_float_depth(unsigned long format)
{
	return format == D3DFMT_F16 || format == D3DFMT_F24S8 || format == D3DFMT_LIN_F16 || format == D3DFMT_LIN_F24S8;
}

static unsigned long bits_per_pixel(unsigned long format)
{
	switch (format)
	{
	case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_LIN_A8R8G8B8: case D3DFMT_LIN_X8R8G8B8:
	case D3DFMT_A8B8G8R8: case D3DFMT_B8G8R8A8: case D3DFMT_R8G8B8A8: case D3DFMT_LIN_A8B8G8R8:
	case D3DFMT_LIN_B8G8R8A8: case D3DFMT_LIN_R8G8B8A8: case D3DFMT_D24S8: case D3DFMT_F24S8:
	case D3DFMT_LIN_D24S8: case D3DFMT_LIN_F24S8: case D3DFMT_V16U16: case D3DFMT_LIN_V16U16:
		return 32;
	case D3DFMT_L8: case D3DFMT_A8: case D3DFMT_AL8: case D3DFMT_P8: case D3DFMT_LIN_L8: case D3DFMT_LIN_A8:
	case D3DFMT_LIN_AL8:
		return 8;
	default:
		return 16;
	}
}

static unsigned long surface_width(const D3DSurface *surface)
{
	if (surface->Size)
		return (surface->Size & D3DSIZE_WIDTH_MASK) + 1;
	return 1UL << ((surface->Format & D3DFORMAT_USIZE_MASK) >> D3DFORMAT_USIZE_SHIFT);
}

static unsigned long surface_height(const D3DSurface *surface)
{
	if (surface->Size)
		return ((surface->Size & D3DSIZE_HEIGHT_MASK) >> D3DSIZE_HEIGHT_SHIFT) + 1;
	return 1UL << ((surface->Format & D3DFORMAT_VSIZE_MASK) >> D3DFORMAT_VSIZE_SHIFT);
}

static unsigned long surface_pitch(const D3DSurface *surface)
{
	if (surface->Size)
		return (((surface->Size & D3DSIZE_PITCH_MASK) >> D3DSIZE_PITCH_SHIFT) + 1) * D3DTEXTURE_PITCH_ALIGNMENT;
	return surface_width(surface) * bits_per_pixel(format_of(surface)) / 8;
}

static unsigned long texture_pitch(const D3DBaseTexture *texture)
{
	if (texture->Size)
		return (((texture->Size & D3DSIZE_PITCH_MASK) >> D3DSIZE_PITCH_SHIFT) + 1) * D3DTEXTURE_PITCH_ALIGNMENT;
	return (1UL << ((texture->Format & D3DFORMAT_USIZE_MASK) >> D3DFORMAT_USIZE_SHIFT)) *
		bits_per_pixel((texture->Format & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT) / 8;
}

static unsigned long log2_floor(unsigned long value)
{
	unsigned long result = 0;

	while (value > 1)
	{
		value >>= 1;
		result++;
	}
	return result;
}

/* The depth buffer a target can be drawn with: any for a linear target; for
a swizzled one (a texture the game renders into) only one of the target's
bytes per pixel, since the console lays a swizzled colour and depth surface
out alike and refuses the pair otherwise (a 16-bit 128x128 swizzled target
with the 32-bit depth buffer stopped it: invalid data on SET_SURFACE_FORMAT,
0x07070223; xemu does not check). Such a target is drawn without depth. */
static D3DSurface *usable_depth(const D3DSurface *target, D3DSurface *depth)
{
	if (!depth || !target || format_is_linear(format_of(target)))
		return depth;
	if (bits_per_pixel(format_of(depth)) != bits_per_pixel(format_of(target)))
		return NULL;
	return depth;
}

/* the depth buffer of the current target, as usable_depth has it */
static D3DSurface *bound_depth(void)
{
	return usable_depth(device.render_target, device.depth_stencil);
}

/* the surface format register: the colour and depth formats, linear or
swizzled (with the swizzled sizes). Without a depth buffer the depth format
is still the target's size (16 bits for a 16- or 8-bit target), which a
swizzled target requires. */
static unsigned long surface_format_word(const D3DSurface *target, const D3DSurface *depth)
{
	unsigned long format = format_of(target);
	unsigned long color, zeta = bits_per_pixel(format) == 32 ? 2 : 1, word;

	switch (format)
	{
	case D3DFMT_X1R5G5B5: case D3DFMT_LIN_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_LIN_A1R5G5B5:
		color = 2; break;
	case D3DFMT_R5G6B5: case D3DFMT_LIN_R5G6B5:
		color = 3; break;
	case D3DFMT_X8R8G8B8: case D3DFMT_LIN_X8R8G8B8:
		color = 5; break;
	case D3DFMT_L8: case D3DFMT_A8: case D3DFMT_LIN_L8: case D3DFMT_LIN_A8:
		color = 9; break;
	case D3DFMT_G8B8: case D3DFMT_LIN_G8B8:
		color = 0xA; break;
	default:
		color = 8; break;
	}
	if (depth)
	{
		unsigned long depth_format = format_of(depth);

		zeta = depth_format == D3DFMT_D16 || depth_format == D3DFMT_LIN_D16 || depth_format == D3DFMT_F16 ||
			depth_format == D3DFMT_LIN_F16 ? 1 : 2;
	}
	word = color | (zeta << 4);
	if (format_is_linear(format))
		word |= 0x100;
	else
		word |= 0x200 | (log2_floor(surface_width(target)) << 16) | (log2_floor(surface_height(target)) << 24);
	return word;
}

static float depth_scale(const D3DSurface *depth)
{
	unsigned long format = depth ? format_of(depth) : D3DFMT_D24S8;

	if (format == D3DFMT_D16 || format == D3DFMT_LIN_D16)
		return 65535.0f;
	if (format_is_float_depth(format))
		return 1.0f;
	return 16777215.0f;
}

/* ---------- vertical blank

The game times its frames on the display's vertical blank: a callback the
runtime called from an interrupt, a count of presented frames it reads
(main/d3d_intimacy.cpp), and a wait. A thread waits on pbkit's vertical
blank for them. */

static D3DCALLBACK vertical_blank_callback;
static volatile unsigned long vertical_blank_count;
static volatile unsigned int flip_count;
static volatile long pending_flips;
static HANDLE vertical_blank_thread;
/* set after each vertical blank's callback, for the threads that wait on
the count the callback keeps (auto-reset: a wait for the next one) */
static HANDLE vertical_blank_event;
/* the same, for the renderer's wait for a frame buffer to leave the screen
alone (resolve_pending_present): a flip is counted at the blank, before the
thread wakes */
static HANDLE flip_event;

static void thread_wait_for_vertical_blank(void)
{
	if (nx_gpu_started())
	{
		/* not the flushing wait: the ring is the renderer's alone (a
		flush from here raced its writes and the ring's wrap) */
		nx_gpu_wait_for_vblank_only();
	}
	else
	{
		DWORD before = nxb_GetTickCount();

		nx_wait_for_vblank();
		if (nxb_GetTickCount() - before < 2)
			nxb_Sleep(16);
	}
}

static void vertical_blank_start(void);
/* Present's leftovers, done at the next frame's first draw (below) */
static int present_pending;
static void resolve_pending_present(void);

static void wait_for_next_vertical_blank(void)
{
	unsigned long count = vertical_blank_count;

	vertical_blank_start();
	while (vertical_blank_count == count)
	{
		if (vertical_blank_event)
			nxb_WaitForSingleObject(vertical_blank_event, 20);
		else
			nxb_Sleep(1);
	}
}

/* the game's frame pacing waits for the counter its vertical blank
callback keeps (main.c, main_rasterizer_throttle): asleep until the next
callback rather than spinning on the clock, which held the processor from
the other threads of the game's priority (the network's among them) */
void halo_xbox_wait_vertical_blank(unsigned long milliseconds)
{
	vertical_blank_start();
	if (vertical_blank_event)
		nxb_WaitForSingleObject(vertical_blank_event, milliseconds);
	else
		nxb_Sleep(1);
}

static DWORD WINAPI vertical_blank_thread_main(LPVOID unused)
{
	unsigned long seen = 0;
	int counting = 0;

	(void)unused;
	for (;;)
	{
		unsigned long blanks = 1;

		thread_wait_for_vertical_blank();
		/* pbkit pulses its event at each blank, and a pulse that comes while
		this thread is not waiting is lost: its own count says how many
		blanks there were (a few at most are made up for) */
		if (nx_gpu_started())
		{
			unsigned long now = nx_gpu_vblank_count();

			if (counting && now - seen >= 1 && now - seen <= 4)
				blanks = now - seen;
			seen = now;
			counting = 1;
		}
		while (blanks--)
		{
			D3DCALLBACK callback;

			vertical_blank_count++;
			/* a presented frame becomes visible at the next vertical blank */
			if (pending_flips > 0)
			{
				pending_flips--;
				flip_count++;
			}
			callback = vertical_blank_callback;
			if (callback)
				callback(0);
		}
		if (vertical_blank_event)
			nxb_SetEvent(vertical_blank_event);
		if (flip_event)
			nxb_SetEvent(flip_event);
	}
	return 0;
}

static void vertical_blank_start(void)
{
	if (!vertical_blank_thread)
	{
		if (!vertical_blank_event)
			vertical_blank_event = nxb_CreateEventA(NULL, FALSE, FALSE, NULL);
		if (!flip_event)
			flip_event = nxb_CreateEventA(NULL, FALSE, FALSE, NULL);
		vertical_blank_thread = CreateThread(NULL, 16384, vertical_blank_thread_main, NULL, 0, NULL);
		if (!vertical_blank_thread)
			platform_log("cannot start the vertical blank thread");
		else
			/* above the game's thread (which may be boosted just after its
			own wait on the blank), as the retail callback ran in the
			interrupt */
			SetThreadPriority(vertical_blank_thread, THREAD_PRIORITY_HIGHEST);
	}
}

/* replaces main/d3d_intimacy.cpp, which reads the counter out of the Xbox
Direct3D runtime's private device structure */
volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	PLATFORM_TRACE();
	vertical_blank_callback = callback;
	vertical_blank_start();
}

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	PLATFORM_TRACE();
	wait_for_next_vertical_blank();
}

/* ---------- the viewport and the targets */

/* the viewport of a vertex program: the screen transform the program's
last instructions apply (c[-38] and c[-37], the constants these registers
load) and the depth range */
static long physical_x(long x);
static long physical_y(long y);
static BOOL screen_target(const D3DSurface *target);
static unsigned long target_logical_width(const D3DSurface *target);
static unsigned long target_logical_height(const D3DSurface *target);

static unsigned long *push_viewport(unsigned long *p)
{
	long left = physical_x((long)device.viewport.X);
	long right = physical_x((long)(device.viewport.X + device.viewport.Width));
	long top = physical_y((long)device.viewport.Y);
	long bottom = physical_y((long)(device.viewport.Y + device.viewport.Height));
	float x = (float)left + 0.53125f;
	float y = (float)top + 0.53125f;
	float half_width = (float)(right - left) * 0.5f;
	float half_height = (float)(bottom - top) * -0.5f;
	float z_range = (device.viewport.MaxZ - device.viewport.MinZ) * device.z_scale;
	float z_offset = device.z_scale * device.viewport.MinZ;

	p = push4f(p, NV097_SET_VIEWPORT_OFFSET, half_width + x, -half_height + y, z_offset, 0.0f);
	p = push4f(p, NV097_SET_VIEWPORT_SCALE, half_width, half_height, z_range, 0.0f);
	p[0] = METHOD(2, NV097_SET_CLIP_MIN);
	p[1] = float_bits(0.0f);
	p[2] = float_bits(device.z_scale);
	return p + 3;
}

static unsigned long control0_word(void)
{
	unsigned long control = 0x00100001;

	if (D3D__RenderState[D3DRS_YUVENABLE])
		control |= 0x10000000;
	if (D3D__RenderState[D3DRS_ZENABLE] == D3DZB_USEW)
		control |= 0x00010000;
	if (bound_depth() && format_is_float_depth(format_of(bound_depth())))
		control |= 0x1000;
	return control;
}

static unsigned long *push_occlude_zstencil(unsigned long *p)
{
	unsigned long control = 0;

	if (D3D__RenderState[D3DRS_STENCILCULLENABLE])
		control |= 2;
	if (D3D__RenderState[D3DRS_OCCLUSIONCULLENABLE] &&
		(!D3D__RenderState[D3DRS_STENCILENABLE] || D3D__RenderState[D3DRS_STENCILFAIL] == D3DSTENCILOP_KEEP))
		control |= 1;
	return push1(p, NV097_SET_OCCLUDE_ZSTENCIL_EN, control);
}

/* the surface registers for the current target and depth buffer (the
runtime brackets each with idle waits, and repeats the set) */
/* the colour and depth DMA windows, bound once: nothing else rebinds them
since pbkit's flip leaves its own alone (tools/xbox_pbkit_patch.py, version
6), and xemu takes each binding for a surface change (it resolves both
surfaces again at the next draw) */
static BOOL dma_contexts_bound;

static void apply_render_target(void)
{
	D3DSurface *target = device.render_target;
	D3DSurface *depth;
	unsigned long width, height, pitch, depth_pitch;
	unsigned long *p;
	int repeat;

	if (target->Data < 0x61000)
	{
		/* a target in the kernel's or the executable's memory, below the
		game state at physical 0x61000 (SetRenderTarget maps the game's
		offset-0 header to the back buffer; anything else here is a
		surface without memory): the back buffer instead. Keeping the
		previous one, as before, left nothing pushed, and after a flip
		(Present re-targets here) pbkit's colour offset 0 stood: the
		kernel's memory, the a30 triple fault's way */
		static int reports;

		if (reports++ < 8)
			platform_log("render target %p at physical %08lx (format %08lx size %08lx): refused; the back buffer instead",
				(void *)target, (unsigned long)target->Data, (unsigned long)target->Format, (unsigned long)target->Size);
		device.render_target = target = &device.back_buffer;
	}
	depth = bound_depth();
	width = surface_width(target);
	height = surface_height(target);
	pitch = surface_pitch(target);
	depth_pitch = depth ? surface_pitch(depth) : pitch;
	p = push_begin(80);

	if (!dma_contexts_bound)
	{
		p = push1(p, NV097_SET_CONTEXT_DMA_COLOR, NX_GPU_MEMORY_CONTEXT_COLOR);
		p = push1(p, NV097_SET_CONTEXT_DMA_ZETA, NX_GPU_MEMORY_CONTEXT_COLOR);
		dma_contexts_bound = TRUE;
	}
	for (repeat = 0; repeat < 2; repeat++)
	{
		p = push1(p, NV097_NO_OPERATION, 0);
		p = push1(p, NV097_SET_SURFACE_PITCH, (pitch & 0xffff) | (depth_pitch << 16));
		p = push1(p, NV097_NO_OPERATION, 0);
		p = push1(p, NV097_WAIT_FOR_IDLE, 0);
		p = push1(p, NV097_NO_OPERATION, 0);
		p = push1(p, NV097_SET_SURFACE_COLOR_OFFSET, target->Data);
		p = push1(p, NV097_NO_OPERATION, 0);
		p = push1(p, NV097_WAIT_FOR_IDLE, 0);
		p = push1(p, NV097_NO_OPERATION, 0);
		/* without a depth buffer, the device's own: nothing is written to it
		with the depth and stencil tests off, and offset 0 of the window is
		the kernel */
		p = push1(p, NV097_SET_SURFACE_ZETA_OFFSET, depth ? depth->Data : device.depth_buffer.Data);
		p = push1(p, NV097_NO_OPERATION, 0);
		p = push1(p, NV097_WAIT_FOR_IDLE, 0);
	}
	p = push2(p, NV097_SET_SURFACE_CLIP_HORIZONTAL, width << 16, height << 16);
	p = push1(p, NV097_SET_CONTROL0, control0_word());
	p = push1(p, NV097_SET_DEPTH_TEST_ENABLE, D3D__RenderState[D3DRS_ZENABLE] && depth);
	p = push1(p, NV097_SET_STENCIL_TEST_ENABLE, D3D__RenderState[D3DRS_STENCILENABLE] && depth);
	p = push1(p, NV097_SET_SURFACE_FORMAT, surface_format_word(target, depth));
	push_end(p);
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	D3DSurface *target = device.render_target;
	unsigned long width = target_logical_width(target), height = target_logical_height(target);
	unsigned long left = viewport->X, top = viewport->Y;
	unsigned long right = viewport->X + viewport->Width, bottom = viewport->Y + viewport->Height;
	unsigned long *p;

	PLATFORM_TRACE();
	/* the whole of a screen target in its own pixels, which the game asks
	for after GetDesc (rasterizer_set_target, the screen effects' copy of
	the frame): the whole screen in the game's columns and lines. Taken as
	the game's own units it was 720 of 854 columns on a widescreen display,
	scaled to 607 of the buffer's 720, and the scope's copy of the frame
	missed its right part */
	if (screen_target(target) && left == 0 && top == 0 &&
		right == surface_width(target) && bottom == surface_height(target))
	{
		right = width;
		bottom = height;
	}
	if (right > width)
		right = width;
	if (bottom > height)
		bottom = height;
	if (left > right)
		left = right;
	if (top > bottom)
		top = bottom;
	device.viewport.X = left;
	device.viewport.Y = top;
	device.viewport.Width = right - left;
	device.viewport.Height = bottom - top;
	device.viewport.MinZ = viewport->MinZ;
	device.viewport.MaxZ = viewport->MaxZ;
	p = push_begin(16);
	push_end(push_viewport(p));
}

/* the back buffer's address now, remembered (Present, and the start) */
static void frame_buffer_seen(unsigned long address)
{
	int index;

	for (index = 0; index < device.frame_buffer_count; index++)
	{
		if (device.frame_buffers[index] == address)
			return;
	}
	if (device.frame_buffer_count < (int)(sizeof(device.frame_buffers) / sizeof(device.frame_buffers[0])))
		device.frame_buffers[device.frame_buffer_count++] = address;
}

/* whether address is one of pbkit's frame buffers: a header of the game's
that holds it means "the back buffer". The retail runtime flipped between
two buffers, and the game caches their addresses in its own texture and
surface headers (rasterizer_set_target_as_texture, one per frame parity);
pbkit rotates three, so a cached address is the buffer of two frames ago
or the one on the screen as often as the current one */
static BOOL is_frame_buffer(unsigned long address)
{
	int index;

	for (index = 0; index < device.frame_buffer_count; index++)
	{
		if (device.frame_buffers[index] == address)
			return TRUE;
	}
	return FALSE;
}

/* the screen's targets: the back buffer, and the depth buffer's memory the
game copies the frame into (the screen effects) */
static BOOL screen_target(const D3DSurface *target)
{
	return target == &device.back_buffer || is_frame_buffer(target->Data) ||
		(device.depth_buffer.Data && target->Data == device.depth_buffer.Data);
}

/* a column the game draws, on the current target's pixels */
static long physical_x(long x)
{
	if (device.render_target && screen_target(device.render_target))
		return (long)((float)(x + device.ui_offset) * device.scale_x + 0.5f);
	return x;
}

/* a line the game draws, on the current target's pixels (720p) */
static long physical_y(long y)
{
	if (device.scale_y != 1.0f && device.render_target && screen_target(device.render_target))
		return (long)((float)y * device.scale_y + 0.5f);
	return y;
}

/* the columns and lines of the current target the game may draw on */
static unsigned long target_logical_width(const D3DSurface *target)
{
	return screen_target(target) ? device.logical_width : surface_width(target);
}

static unsigned long target_logical_height(const D3DSurface *target)
{
	return screen_target(target) && device.logical_height ? device.logical_height : surface_height(target);
}

void d3d8_nv2a_set_ui_offset(long columns)
{
	if (device.ui_offset == columns)
		return;
	device.ui_offset = columns;
	if (device.render_target)
	{
		unsigned long *p = push_begin(16);

		push_end(push_viewport(p));
	}
}

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	D3DVIEWPORT8 viewport;

	PLATFORM_TRACE();
	resolve_pending_present();
	/* the game keeps a surface header of its own for the back buffer,
	640x480 A8R8G8B8 at offset 0 (its cinematics render through it): on
	the retail runtime the colour DMA window put offset 0 at the frame
	buffer; here the window spans memory and offset 0 is the kernel, which
	a30's opening then painted over (interrupt table included: a triple
	fault). It means the real back buffer. */
	if (render_target && render_target != &device.back_buffer &&
		(render_target->Data == 0 || is_frame_buffer(render_target->Data)))
		render_target = &device.back_buffer;
	if (render_target)
		device.render_target = render_target;
	if (device.scale_y != 1.0f)
		device.dirty |= DIRTY_POINT_PARAMS; /* (sizes differ between the screen and other targets) */
	device.depth_stencil = depth_stencil;
	device.z_scale = depth_scale(bound_depth());
	apply_render_target();
	viewport.X = 0;
	viewport.Y = 0;
	viewport.Width = target_logical_width(device.render_target);
	viewport.Height = target_logical_height(device.render_target);
	viewport.MinZ = 0.0f;
	viewport.MaxZ = 1.0f;
	D3DDevice_SetViewport(&viewport);
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	PLATFORM_TRACE();
	resolve_pending_present();
	(void)back_buffer;
	(void)type;
	/* like Direct3D, the caller gets a reference it must release */
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	PLATFORM_TRACE();
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

/* ---------- render states */

void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	unsigned long *p = push_begin(2);

	PLATFORM_TRACE();
	p[0] = method;
	p[1] = value;
	push_end(p + 2);
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	PLATFORM_TRACE();
	if ((unsigned long)state >= D3DRS_MAX)
		return;
	/* the value it has already: applied, or its group already dirty */
	if (D3D__RenderState[state] == value)
		return;
	D3D__RenderState[state] = value;
	switch (state)
	{
	case D3DRS_FOGENABLE: case D3DRS_FOGTABLEMODE: case D3DRS_FOGSTART: case D3DRS_FOGEND:
	case D3DRS_FOGDENSITY: case D3DRS_RANGEFOGENABLE: case D3DRS_SPECULARENABLE:
		device.dirty |= DIRTY_FOG;
		break;
	case D3DRS_WRAP0: case D3DRS_WRAP1: case D3DRS_WRAP2: case D3DRS_WRAP3:
		device.dirty |= DIRTY_TEXTURE_STATE;
		break;
	case D3DRS_POINTSIZE: case D3DRS_POINTSIZE_MIN: case D3DRS_POINTSIZE_MAX: case D3DRS_POINTSPRITEENABLE:
	case D3DRS_POINTSCALEENABLE: case D3DRS_POINTSCALE_A: case D3DRS_POINTSCALE_B: case D3DRS_POINTSCALE_C:
		device.dirty |= DIRTY_POINT_PARAMS;
		break;
	default:
		break;
	}
}

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	PLATFORM_TRACE();
	D3DDevice_SetRenderState(state, value);
}

void WINAPI D3DDevice_SetRenderState_CullMode(DWORD value)
{
	unsigned long *p = push_begin(4);

	if (value == D3DCULL_NONE)
	{
		p = push1(p, NV097_SET_CULL_FACE_ENABLE, FALSE);
	}
	else
	{
		p = push1(p, NV097_SET_CULL_FACE_ENABLE, TRUE);
		p = push1(p, NV097_SET_CULL_FACE, value == D3D__RenderState[D3DRS_FRONTFACE] ? 0x404 : 0x405);
	}
	push_end(p);
	D3D__RenderState[D3DRS_CULLMODE] = value;
}

void WINAPI D3DDevice_SetRenderState_FrontFace(DWORD value)
{
	emit1(NV097_SET_FRONT_FACE, value);
	D3D__RenderState[D3DRS_FRONTFACE] = value;
	D3DDevice_SetRenderState_CullMode(D3D__RenderState[D3DRS_CULLMODE]);
}

void WINAPI D3DDevice_SetRenderState_ZEnable(DWORD value)
{
	DWORD previous = D3D__RenderState[D3DRS_ZENABLE];

	emit1(NV097_SET_DEPTH_TEST_ENABLE, value != D3DZB_FALSE && bound_depth() != NULL);
	D3D__RenderState[D3DRS_ZENABLE] = value;
	if (previous == D3DZB_USEW || value == D3DZB_USEW)
		emit1(NV097_SET_CONTROL0, control0_word());
}

void WINAPI D3DDevice_SetRenderState_StencilEnable(DWORD value)
{
	unsigned long *p = push_begin(4);

	p = push_occlude_zstencil(p);
	p = push1(p, NV097_SET_STENCIL_TEST_ENABLE, value && bound_depth() != NULL);
	push_end(p);
	D3D__RenderState[D3DRS_STENCILENABLE] = value;
}

void WINAPI D3DDevice_SetRenderState_StencilFail(DWORD value)
{
	unsigned long *p = push_begin(4);

	p = push_occlude_zstencil(p);
	p = push1(p, NV097_SET_STENCIL_OP_FAIL, value);
	push_end(p);
	D3D__RenderState[D3DRS_STENCILFAIL] = value;
}

void WINAPI D3DDevice_SetRenderState_FillMode(DWORD value)
{
	DWORD back = D3D__RenderState[D3DRS_TWOSIDEDLIGHTING] ? D3D__RenderState[D3DRS_BACKFILLMODE] : value;
	unsigned long *p = push_begin(3);

	push_end(push2(p, NV097_SET_FRONT_POLYGON_MODE, value, back));
	D3D__RenderState[D3DRS_FILLMODE] = value;
}

void WINAPI D3DDevice_SetRenderState_BackFillMode(DWORD value)
{
	D3D__RenderState[D3DRS_BACKFILLMODE] = value;
	D3DDevice_SetRenderState_FillMode(D3D__RenderState[D3DRS_FILLMODE]);
}

void WINAPI D3DDevice_SetRenderState_TwoSidedLighting(DWORD value)
{
	D3D__RenderState[D3DRS_TWOSIDEDLIGHTING] = value;
	emit1(NV097_SET_TWO_SIDE_LIGHT_EN, value);
	D3DDevice_SetRenderState_FillMode(D3D__RenderState[D3DRS_FILLMODE]);
}

void WINAPI D3DDevice_SetRenderState_FogColor(DWORD value)
{
	emit1(0x02A8, swap_rgb(value)); /* NV097_SET_FOG_COLOR */
	D3D__RenderState[D3DRS_FOGCOLOR] = value;
}

void WINAPI D3DDevice_SetRenderState_TextureFactor(DWORD value)
{
	/* the combiner factors; a pixel shader sets its own constants */
	if (!device.pixel_shader)
	{
		unsigned long *p = push_begin(17);
		int i;

		p[0] = METHOD(16, NV097_SET_COMBINER_FACTOR0);
		for (i = 0; i < 16; i++)
			p[1 + i] = value;
		push_end(p + 17);
	}
	D3D__RenderState[D3DRS_TEXTUREFACTOR] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value)
{
	float bias = -(float)value;
	float scale = bias * 0.25f;
	BOOL enabled = value != 0;

	/* (the game sets it for every model part: unchanged, nothing to push) */
	if (D3D__RenderState[D3DRS_ZBIAS] == value &&
		D3D__RenderState[D3DRS_POLYGONOFFSETZSLOPESCALE] == float_bits(scale) &&
		D3D__RenderState[D3DRS_POLYGONOFFSETZOFFSET] == float_bits(bias) &&
		D3D__RenderState[D3DRS_POINTOFFSETENABLE] == (DWORD)enabled &&
		D3D__RenderState[D3DRS_WIREFRAMEOFFSETENABLE] == (DWORD)enabled &&
		D3D__RenderState[D3DRS_SOLIDOFFSETENABLE] == (DWORD)enabled)
		return;
	D3DDevice_SetRenderStateNotInline(D3DRS_POLYGONOFFSETZSLOPESCALE, float_bits(scale));
	D3DDevice_SetRenderStateNotInline(D3DRS_POLYGONOFFSETZOFFSET, float_bits(bias));
	D3DDevice_SetRenderStateNotInline(D3DRS_POINTOFFSETENABLE, enabled);
	D3DDevice_SetRenderStateNotInline(D3DRS_WIREFRAMEOFFSETENABLE, enabled);
	D3DDevice_SetRenderStateNotInline(D3DRS_SOLIDOFFSETENABLE, enabled);
	D3D__RenderState[D3DRS_ZBIAS] = value;
}

void WINAPI D3DDevice_SetRenderState_LogicOp(DWORD value)
{
	unsigned long *p = push_begin(3);

	if (value == D3DLOGICOP_NONE)
		p = push1(p, NV097_SET_LOGIC_OP_ENABLE, FALSE);
	else
		p = push2(p, NV097_SET_LOGIC_OP_ENABLE, TRUE, value);
	push_end(p);
	D3D__RenderState[D3DRS_LOGICOP] = value;
}

void WINAPI D3DDevice_SetRenderState_EdgeAntiAlias(DWORD value)
{
	unsigned long *p = push_begin(3);

	push_end(push2(p, NV097_SET_LINE_SMOOTH_ENABLE, value, value));
	D3D__RenderState[D3DRS_EDGEANTIALIAS] = value;
}

void WINAPI D3DDevice_SetRenderState_LineWidth(DWORD value)
{
	long scaled = (long)(bits_float(value) * 8.0f + 0.5f);

	if (scaled < 0)
		scaled = 0;
	if (scaled > 511)
		scaled = 511;
	emit1(NV097_SET_LINE_WIDTH, (unsigned long)scaled);
	D3D__RenderState[D3DRS_LINEWIDTH] = value;
}

void WINAPI D3DDevice_SetRenderState_ShadowFunc(DWORD value)
{
	/* NV097_SET_SHADOW_DEPTH_FUNC is 0x1E6C (the runtime's, nxdk's
	SET_SHADOW_COMPARE_FUNC); 0x1D74, which this sent before, is among the
	semaphore methods, which xemu ignores and the console does not */
	emit1(NV097_SET_SHADOW_DEPTH_FUNC, value - D3DCMP_NEVER);
	D3D__RenderState[D3DRS_SHADOWFUNC] = value;
}

void WINAPI D3DDevice_SetRenderState_NormalizeNormals(DWORD value)
{
	emit1(NV097_SET_NORMALIZATION_ENABLE, value);
	D3D__RenderState[D3DRS_NORMALIZENORMALS] = value;
}

void WINAPI D3DDevice_SetRenderState_VertexBlend(DWORD value)
{
	emit1(NV097_SET_SKIN_MODE, value);
	D3D__RenderState[D3DRS_VERTEXBLEND] = value;
}

void WINAPI D3DDevice_SetRenderState_PSTextureModes(DWORD value)
{
	device.shader_stage_program = value;
	device.dirty |= DIRTY_SHADER_STAGES;
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = value;
}

void WINAPI D3DDevice_SetRenderState_YuvEnable(DWORD value)
{
	D3D__RenderState[D3DRS_YUVENABLE] = value;
	emit1(NV097_SET_CONTROL0, control0_word());
}

void WINAPI D3DDevice_SetRenderState_OcclusionCullEnable(DWORD value)
{
	unsigned long *p = push_begin(2);

	D3D__RenderState[D3DRS_OCCLUSIONCULLENABLE] = value;
	push_end(push_occlude_zstencil(p));
}

void WINAPI D3DDevice_SetRenderState_StencilCullEnable(DWORD value)
{
	unsigned long *p = push_begin(2);

	D3D__RenderState[D3DRS_STENCILCULLENABLE] = value;
	push_end(push_occlude_zstencil(p));
}

void WINAPI D3DDevice_SetRenderState_MultiSampleAntiAlias(DWORD value)
{
	D3D__RenderState[D3DRS_MULTISAMPLEANTIALIAS] = value;
	emit1(NV097_SET_ANTI_ALIASING_CONTROL, (value & 0xf) | (D3D__RenderState[D3DRS_MULTISAMPLEMASK] << 16));
}

void WINAPI D3DDevice_SetRenderState_MultiSampleMask(DWORD value)
{
	D3D__RenderState[D3DRS_MULTISAMPLEMASK] = value;
	emit1(NV097_SET_ANTI_ALIASING_CONTROL,
		(D3D__RenderState[D3DRS_MULTISAMPLEANTIALIAS] & 0xf) | (value << 16));
}

/* the states the console's registers do not need, or the port does not
have: kept for the game to read back */
#define STORED_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) \
	{ \
		D3D__RenderState[state] = value; \
	}

STORED_RENDER_STATE(MultiSampleType, D3DRS_MULTISAMPLETYPE)
STORED_RENDER_STATE(Dxt1NoiseEnable, D3DRS_DXT1NOISEENABLE)
STORED_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
STORED_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
STORED_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

/* ---------- texture stage states */

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	PLATFORM_TRACE();
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX && D3D__TextureState[stage][type] != value)
	{
		D3D__TextureState[stage][type] = value;
		device.dirty |= DIRTY_TEXTURE_STATE;
	}
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	unsigned long mode;
	unsigned long *p;

	PLATFORM_TRACE();
	if (stage >= D3DTSS_MAXSTAGES)
		return;
	D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
	switch (value & 0xffff0000)
	{
	case D3DTSS_TCI_PASSTHRU: mode = 0; break;
	case D3DTSS_TCI_CAMERASPACENORMAL: mode = 0x8511; break;
	case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR: mode = 0x8512; break;
	case D3DTSS_TCI_CAMERASPACEPOSITION: mode = 0x2400; break;
	case D3DTSS_TCI_OBJECT: mode = 0x2401; break;
	default: mode = 0x2402; break;
	}
	p = push_begin(4);
	p[0] = METHOD(3, NV097_SET_TEXGEN_S + stage * 16);
	p[1] = mode;
	p[2] = mode;
	p[3] = mode;
	push_end(p + 4);
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	PLATFORM_TRACE();
	if (stage >= D3DTSS_MAXSTAGES)
		return;
	emit1(NV097_SET_TEXTURE_BORDER_COLOR + stage * TEXTURE_STAGE_STRIDE, value);
	D3D__TextureState[stage][D3DTSS_BORDERCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	PLATFORM_TRACE();
	if (stage >= D3DTSS_MAXSTAGES)
		return;
	emit1(NV097_SET_COLOR_KEY_COLOR + stage * 4, value);
	D3D__TextureState[stage][D3DTSS_COLORKEYCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	unsigned long unit = device.pixel_shader ? stage : stage + 1;

	PLATFORM_TRACE();
	if (stage >= D3DTSS_MAXSTAGES || (unsigned long)type < D3DTSS_BUMPENVMAT00 ||
		(unsigned long)type > D3DTSS_BUMPENVLOFFSET)
		return;
	if (unit & 3)
		emit1(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + unit * TEXTURE_STAGE_STRIDE + 4 * (type - D3DTSS_BUMPENVMAT00), value);
	D3D__TextureState[stage][type] = value;
}


static unsigned long draw_calls, stale_query_answers; /* per frame, for the frame statistics */
/* the arrays stay off across consecutive immediate-mode draws (a string's
glyphs): a buffer draw's formats turn them back on */
static BOOL immediate_arrays_disabled;

/* where the CPU's frame goes: cycles inside the device's busiest entry
points, for the frame statistics (rdtsc: no register access, no exit) */
static inline unsigned long long cycle_counter(void)
{
	unsigned int low, high;

	__asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
	return ((unsigned long long)high << 32) | low;
}

enum { TIMER_DRAW, TIMER_CONSTANTS, TIMER_IMMEDIATE, TIMER_TEXTURES, TIMER_SHADERS, TIMER_COUNT };
static unsigned long long timer_cycles[TIMER_COUNT], immediate_from, frame_from_cycles;
/* the statistics' timers: two rdtsc around every draw, texture, shader and
constant call cost 0.3-0.8 ms a frame on the console, so they are built
only for a measuring build (-DNX_DEVICE_TIMERS); the frame line then prints
0% for them */
#ifdef NX_DEVICE_TIMERS
#define TIMED_BEGIN unsigned long long timed_from = cycle_counter()
#define TIMED_END(which) (timer_cycles[which] += cycle_counter() - timed_from)
#define IMMEDIATE_BEGIN() (immediate_from = cycle_counter())
#else
#define TIMED_BEGIN do { } while (0)
#define TIMED_END(which) do { } while (0)
#define IMMEDIATE_BEGIN() do { } while (0)
#endif

static unsigned long long present_wait_microseconds, stats_from;

/* ---------- textures */

static unsigned long texture_filter_word(DWORD stage, const D3DBaseTexture *texture);

/* What each stage's address, control and filter registers hold: a draw's
state (apply_texture_state) sends only the ones that change. Every texture
change dirtied all four stages' twelve registers, which went out again
before nearly every draw. ~0 is "not known" (no register holds it). */
static unsigned long stage_address[D3DTSS_MAXSTAGES], stage_control0[D3DTSS_MAXSTAGES],
	stage_filter[D3DTSS_MAXSTAGES];
static unsigned long stage_program_sent;
/* and what the vertex array registers hold (apply_vertex_state): the
format block goes out only when one changed; after it the offsets all go
out again, as before (in case the chip takes a format write as resetting
them), otherwise only those that changed */
static unsigned long vertex_format_sent[16];
static unsigned long vertex_offset_sent[16];

static void forget_vertex_offsets(void)
{
	memset(vertex_offset_sent, 0xff, sizeof(vertex_offset_sent));
}

static void forget_texture_registers(void)
{
	memset(stage_address, 0xff, sizeof(stage_address));
	memset(stage_control0, 0xff, sizeof(stage_control0));
	memset(stage_filter, 0xff, sizeof(stage_filter));
	stage_program_sent = ~0UL;
	memset(vertex_format_sent, 0xff, sizeof(vertex_format_sent));
	forget_vertex_offsets();
}

/* A texture of one level: a linear one (the back buffer, the screen
effects' copies: Size set), or a header whose format gives no more. The
NV2A refuses a filter between levels (a minification filter with a mip
filter) for such a texture with an invalid data error on
NV097_SET_TEXTURE_FILTER, which stops the console; the game asks for one
anyway, and xemu does not check. */
static BOOL texture_single_level(const D3DBaseTexture *texture)
{
	unsigned long format = texture->Format;

	return texture->Size || format_is_linear((format & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT) ||
		((format & D3DFORMAT_MIPMAP_MASK) >> D3DFORMAT_MIPMAP_SHIFT) <= 1;
}

static void SetTexture_body(DWORD stage, D3DBaseTexture *texture)
{
	unsigned long *p;
	BOOL single_level;

	PLATFORM_TRACE();
	if (stage >= D3DTSS_MAXSTAGES)
		return;
	device.textures[stage] = texture;
	device.dirty |= DIRTY_TEXTURE_STATE | DIRTY_SHADER_STAGES;
	if (!texture)
	{
		emit1(NV097_SET_TEXTURE_CONTROL0 + stage * TEXTURE_STAGE_STRIDE, 0);
		stage_control0[stage] = 0;
		return;
	}
	single_level = texture_single_level(texture);
	{
		/* a header with no mip levels (the cache's fonts): one level, as
		the hardware took it */
		unsigned long format = texture->Format;
		unsigned long data = texture->Data;

		if (!(format & D3DFORMAT_MIPMAP_MASK))
			format |= 1UL << D3DFORMAT_MIPMAP_SHIFT;
		/* the game's headers for the back buffer (the screen effects,
		active camouflage) hold whichever of pbkit's buffers they first
		saw: the current one is meant */
		if (data && data != device.back_buffer.Data && is_frame_buffer(data))
			data = device.back_buffer.Data;
		p = push_begin(10);
		/* the filter and the format agree at every step: a filter of one
		level before a texture of one level, one between levels after a
		texture that has them */
		if (single_level)
		{
			stage_filter[stage] = texture_filter_word(stage, texture);
			p = push1(p, NV097_SET_TEXTURE_FILTER + stage * TEXTURE_STAGE_STRIDE, stage_filter[stage]);
		}
		p = push2(p, NV097_SET_TEXTURE_OFFSET + stage * TEXTURE_STAGE_STRIDE, data, format);
	}
	if (texture->Size)
	{
		unsigned long width = (texture->Size & D3DSIZE_WIDTH_MASK) + 1;
		unsigned long height = ((texture->Size & D3DSIZE_HEIGHT_MASK) >> D3DSIZE_HEIGHT_SHIFT) + 1;

		p = push1(p, NV097_SET_TEXTURE_CONTROL1 + stage * TEXTURE_STAGE_STRIDE, texture_pitch(texture) << 16);
		p = push1(p, NV097_SET_TEXTURE_IMAGE_RECT + stage * TEXTURE_STAGE_STRIDE, (width << 16) | height);
	}
	if (!single_level)
	{
		stage_filter[stage] = texture_filter_word(stage, texture);
		p = push1(p, NV097_SET_TEXTURE_FILTER + stage * TEXTURE_STAGE_STRIDE, stage_filter[stage]);
	}
	push_end(p);
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	TIMED_BEGIN;
	SetTexture_body(stage, texture);
	TIMED_END(TIMER_TEXTURES);
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	PLATFORM_TRACE();
	if (stage >= D3DTSS_MAXSTAGES)
		return;
	device.palettes[stage] = palette;
	if (palette)
		emit1(NV097_SET_TEXTURE_PALETTE + stage * TEXTURE_STAGE_STRIDE, palette->Data | (palette->Common >> 28));
}

static unsigned long address_mode(unsigned long mode)
{
	return mode >= 1 && mode <= 5 ? mode : 1;
}

static unsigned long minification_code(unsigned long minify, unsigned long mip)
{
	BOOL linear = minify != D3DTEXF_POINT && minify != D3DTEXF_NONE;

	switch (mip)
	{
	case D3DTEXF_POINT: return linear ? 4 : 3;
	case D3DTEXF_LINEAR: case D3DTEXF_ANISOTROPIC: return linear ? 6 : 5;
	default: return linear ? 2 : 1;
	}
}

/* the stage's filter register: its level-of-detail bias, minification and
magnification filters and colour signs; between levels only for a texture
that has them (texture_single_level), none bound counting as one level */
static unsigned long texture_filter_word(DWORD stage, const D3DBaseTexture *texture)
{
	const DWORD *state = D3D__TextureState[stage];
	BOOL single_level = !texture || texture_single_level(texture);
	long bias = (long)(bits_float(state[D3DTSS_MIPMAPLODBIAS]) * 256.0f);
	unsigned long filter, magnify;

	if (bias < -4096)
		bias = -4096;
	if (bias > 4095)
		bias = 4095;
	filter = ((unsigned long)bias & 0x1fff) |
		(minification_code(state[D3DTSS_MINFILTER], single_level ? D3DTEXF_NONE : state[D3DTSS_MIPFILTER]) << 16);
	/* the convolution kernel (bits 13-15) is quincunx (1) or Gaussian (2)
	whatever the filters: the console refuses 0 with an invalid data error
	(0x01010000 and 0x02040000 stopped it; xemu reads the field only for a
	convolution filter); the runtime's values have it set (0x01012000) */
	switch (state[D3DTSS_MAGFILTER])
	{
	case D3DTEXF_POINT: magnify = 1; filter |= 1UL << 13; break;
	case D3DTEXF_QUINCUNX: magnify = 4; filter |= 1UL << 13; break;
	case D3DTEXF_GAUSSIANCUBIC: magnify = 4; filter |= 2UL << 13; break;
	default: magnify = 2; filter |= 1UL << 13; break;
	}
	filter |= magnify << 24;
	filter |= state[D3DTSS_COLORSIGN] & 0xf0000000UL;
	return filter;
}

/* the texture registers the stage states decide: addressing, the level
range, filtering; the runtime applied them before a draw */
static void apply_texture_state(void)
{
	unsigned long *p = push_begin(4 * 8);
	unsigned long stage;

	device.dirty &= ~DIRTY_TEXTURE_STATE;
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		const DWORD *state = D3D__TextureState[stage];
		const D3DBaseTexture *texture = device.textures[stage];
		unsigned long wrap = D3D__RenderState[D3DRS_WRAP0 + stage];
		unsigned long address = address_mode(state[D3DTSS_ADDRESSU]) | (address_mode(state[D3DTSS_ADDRESSV]) << 8) |
			(address_mode(state[D3DTSS_ADDRESSW]) << 16);
		unsigned long control0 = 0, filter;
		unsigned long anisotropy = state[D3DTSS_MAXANISOTROPY];

		if (wrap & 1)
			address |= 1UL << 4;
		if (wrap & 2)
			address |= 1UL << 12;
		if (wrap & 4)
			address |= 1UL << 20;
		if (wrap & 8)
			address |= 1UL << 24;
		if (texture)
		{
			/* the level range, in levels: the finest level the game allows
			in bits 18-29, no coarse limit in bits 6-17 (xemu reads them so) */
			control0 = 0x40000000UL | ((state[D3DTSS_MAXMIPLEVEL] & 0xfff) << 18) | (0xfffUL << 6) |
				(state[D3DTSS_COLORKEYOP] & 3) | (state[D3DTSS_ALPHAKILL] & 4);
			/* anisotropy filters between levels too */
			if (state[D3DTSS_MINFILTER] == D3DTEXF_ANISOTROPIC && anisotropy > 1 && !texture_single_level(texture))
				control0 |= (anisotropy >= 8 ? 3UL : anisotropy >= 4 ? 2UL : 1UL) << 4;
		}
		if (stage_address[stage] != address)
		{
			stage_address[stage] = address;
			p = push1(p, NV097_SET_TEXTURE_ADDRESS + stage * TEXTURE_STAGE_STRIDE, address);
		}
		if (stage_control0[stage] != control0)
		{
			stage_control0[stage] = control0;
			p = push1(p, NV097_SET_TEXTURE_CONTROL0 + stage * TEXTURE_STAGE_STRIDE, control0);
		}
		filter = texture_filter_word(stage, texture);
		if (stage_filter[stage] != filter)
		{
			stage_filter[stage] = filter;
			p = push1(p, NV097_SET_TEXTURE_FILTER + stage * TEXTURE_STAGE_STRIDE, filter);
		}
	}
	push_end(p);
}

/* ---------- pixel shaders (register combiner definitions) */

/* the shader stage program: the pixel shader's texture modes, which a
shader that asks for it has adjusted to the textures bound */
static void apply_shader_stages(void)
{
	unsigned long program = device.shader_stage_program;
	unsigned long result = 0;
	int stage;

	device.dirty &= ~DIRTY_SHADER_STAGES;
	if (device.pixel_shader_adjusts_texture_modes)
	{
		for (stage = 3; stage >= 0; stage--)
		{
			unsigned long mode = (program >> (stage * 5)) & 0x1f;
			const D3DBaseTexture *texture = device.textures[stage];

			if (!texture && mode != PS_TEXTUREMODES_PASSTHRU && mode != PS_TEXTUREMODES_CLIPPLANE &&
				mode != PS_TEXTUREMODES_DOT_ZW && mode != PS_TEXTUREMODES_DOTPRODUCT)
			{
				mode = PS_TEXTUREMODES_NONE;
			}
			else if (mode >= PS_TEXTUREMODES_PROJECT2D && mode <= PS_TEXTUREMODES_CUBEMAP)
			{
				unsigned long format = (texture->Format & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT;

				if (texture->Format & D3DFORMAT_CUBEMAP)
					mode = PS_TEXTUREMODES_CUBEMAP;
				else if ((texture->Format & D3DFORMAT_DIMENSION_MASK) == (3 << D3DFORMAT_DIMENSION_SHIFT) ||
					(format >= D3DFMT_D24S8 && format <= D3DFMT_LIN_F16))
					mode = PS_TEXTUREMODES_PROJECT3D;
				else
					mode = PS_TEXTUREMODES_PROJECT2D;
			}
			else if (mode == PS_TEXTUREMODES_DOT_STR_3D || mode == PS_TEXTUREMODES_DOT_STR_CUBE)
			{
				mode = (texture->Format & D3DFORMAT_CUBEMAP) ? PS_TEXTUREMODES_DOT_STR_CUBE : PS_TEXTUREMODES_DOT_STR_3D;
			}
			result = (result << 5) | mode;
		}
	}
	else
	{
		result = program;
	}
	if (result != stage_program_sent)
	{
		stage_program_sent = result;
		emit1(NV097_SET_SHADER_STAGE_PROGRAM, result);
	}
}

static void SetPixelShaderProgram_body(D3DPIXELSHADERDEF *definition)
{
	unsigned long *p;

	PLATFORM_TRACE();
	if (!definition)
	{
		device.pixel_shader = NULL;
		device.pixel_shader_uses_specular_fog = FALSE;
		device.pixel_shader_adjusts_texture_modes = FALSE;
		device.dirty |= DIRTY_SHADER_STAGES | DIRTY_FOG;
		emit1(NV097_SET_SHADER_OTHER_STAGE_INPUT, 0x00210000);
		return;
	}
	device.pixel_shader = definition;
	device.pixel_shader_uses_specular_fog = (definition->PSFinalCombinerInputsABCD | definition->PSFinalCombinerInputsEFG) != 0;
	device.pixel_shader_adjusts_texture_modes = (definition->PSFinalCombinerConstants & 0x100) != 0;
	device.shader_stage_program = definition->PSTextureModes;
	device.dirty |= DIRTY_SHADER_STAGES | DIRTY_FOG;
	p = push_begin(72);
	p[0] = METHOD(8, NV097_SET_COMBINER_ALPHA_ICW);
	memcpy(p + 1, definition->PSAlphaInputs, 8 * sizeof(unsigned long));
	p += 9;
	/* the constants, the alpha outputs and the colour inputs are one run
	of registers, as of the definition */
	p[0] = METHOD(32, NV097_SET_COMBINER_FACTOR0);
	memcpy(p + 1, definition->PSConstant0, 32 * sizeof(unsigned long));
	p += 33;
	p = push1(p, NV097_SET_SHADER_CLIP_PLANE_MODE, definition->PSCompareMode);
	p[0] = METHOD(2, NV097_SET_COMBINER_FINAL_CONSTANT);
	p[1] = definition->PSFinalCombinerConstant0;
	p[2] = definition->PSFinalCombinerConstant1;
	p += 3;
	p[0] = METHOD(9, NV097_SET_COMBINER_COLOR_OCW);
	memcpy(p + 1, definition->PSRGBOutputs, 9 * sizeof(unsigned long));
	p += 10;
	p[0] = METHOD(2, NV097_SET_DOT_RGBMAPPING);
	p[1] = definition->PSDotMapping;
	p[2] = definition->PSInputTexture;
	p += 3;
	if (device.pixel_shader_uses_specular_fog)
	{
		p[0] = METHOD(2, NV097_SET_COMBINER_SPECULAR_FOG_CW0);
		p[1] = definition->PSFinalCombinerInputsABCD;
		p[2] = definition->PSFinalCombinerInputsEFG;
		p += 3;
	}
	push_end(p);
	/* the game reads the shader's words back as render states */
	memcpy(D3D__RenderState, definition, D3DRS_PS_MAX * sizeof(DWORD));
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = definition->PSTextureModes;
}

void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	TIMED_BEGIN;
	SetPixelShaderProgram_body(definition);
	TIMED_END(TIMER_SHADERS);
}

/* ---------- fog and the specular/fog combiner */

static void apply_fog(void)
{
	unsigned long *p = push_begin(16);
	unsigned long control0;

	device.dirty &= ~DIRTY_FOG;
	if (D3D__RenderState[D3DRS_FOGENABLE])
	{
		DWORD table_mode = D3D__RenderState[D3DRS_FOGTABLEMODE];
		float start = bits_float(D3D__RenderState[D3DRS_FOGSTART]);
		float end = bits_float(D3D__RenderState[D3DRS_FOGEND]);
		float density = bits_float(D3D__RenderState[D3DRS_FOGDENSITY]);
		unsigned long generator = D3D__RenderState[D3DRS_RANGEFOGENABLE] ? 1 : 2;
		unsigned long fog_mode;
		float bias, scale;

		if (table_mode == D3DFOG_NONE)
		{
			bias = 1.0f;
			scale = 1.0f;
			fog_mode = 0x2601;
			generator = 0;
		}
		else if (table_mode == D3DFOG_LINEAR)
		{
			float linear_scale = end != start ? 1.0f / (end - start) : 8192.0f;

			bias = 1.0f + end * linear_scale;
			scale = -linear_scale;
			fog_mode = 0x2601;
		}
		else if (table_mode == D3DFOG_EXP)
		{
			bias = 1.5f;
			scale = -density * (1.0f / (2.0f * 5.5452f));
			fog_mode = 0x800;
		}
		else
		{
			bias = 1.5f;
			scale = -density * (1.0f / (2.0f * 2.354824834249885f));
			fog_mode = 0x801;
		}
		p = push2(p, NV097_SET_FOG_GEN_MODE, generator, TRUE);
		p = push1(p, NV097_SET_FOG_MODE, fog_mode);
		p[0] = METHOD(3, NV097_SET_FOG_PARAMS);
		p[1] = float_bits(bias);
		p[2] = float_bits(scale);
		p[3] = 0;
		p += 4;
		control0 = 0x13000300 | (D3D__RenderState[D3DRS_SPECULARENABLE] ? 0x000e0000 : 0x000c0000);
	}
	else
	{
		p = push1(p, NV097_SET_FOG_ENABLE, FALSE);
		control0 = D3D__RenderState[D3DRS_SPECULARENABLE] ? 0xe : 0xc;
	}
	if (!device.pixel_shader || !device.pixel_shader_uses_specular_fog)
		p = push2(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, control0, 0x1c80);
	push_end(p);
}

static void apply_point_parameters(void)
{
	float size = bits_float(D3D__RenderState[D3DRS_POINTSIZE]);
	float minimum = bits_float(D3D__RenderState[D3DRS_POINTSIZE_MIN]);
	float maximum = bits_float(D3D__RenderState[D3DRS_POINTSIZE_MAX]);
	BOOL scale_enable = D3D__RenderState[D3DRS_POINTSCALEENABLE];
	/* sizes are in the game's pixels: at 720p the screen's are 1.5 of them
	(SetRenderTarget marks these dirty when the target changes) */
	float pixel_scale = device.scale_y > 0.0f && device.render_target && screen_target(device.render_target) ?
		device.scale_y : 1.0f;
	unsigned long fixed_size;
	unsigned long *p = push_begin(16);

	device.dirty &= ~DIRTY_POINT_PARAMS;
	minimum *= pixel_scale;
	maximum *= pixel_scale;
	if (!scale_enable)
	{
		size *= pixel_scale;
		if (size < minimum)
			size = minimum;
		if (size > maximum)
			size = maximum;
		if (size > 64.0f)
			size = 64.0f;
	}
	else
	{
		/* (the size is the viewport's height times the point's: the
		height alone carries the scale) */
		float height = (float)device.viewport.Height * pixel_scale;
		float a = bits_float(D3D__RenderState[D3DRS_POINTSCALE_A]);
		float b = bits_float(D3D__RenderState[D3DRS_POINTSCALE_B]);
		float c = bits_float(D3D__RenderState[D3DRS_POINTSCALE_C]);
		float delta = maximum - minimum;
		float factor = size * height != 0.0f ? delta / (size * height) : 0.0f;

		factor *= factor;
		p[0] = METHOD(8, NV097_SET_POINT_PARAMS);
		p[1] = float_bits(a * factor);
		p[2] = float_bits(b * factor);
		p[3] = float_bits(c * factor);
		p[4] = float_bits(delta);
		p[5] = float_bits(delta);
		p[6] = float_bits(delta);
		p[7] = float_bits(delta != 0.0f ? -minimum / delta : 0.0f);
		p[8] = float_bits(minimum);
		p += 9;
	}
	p = push2(p, NV097_SET_POINT_PARAMS_ENABLE, scale_enable, D3D__RenderState[D3DRS_POINTSPRITEENABLE]);
	fixed_size = (unsigned long)(8.0f * size + 0.5f);
	if (fixed_size > 0x1ff)
		fixed_size = 0x1ff;
	p = push1(p, NV097_SET_POINT_SIZE, fixed_size);
	push_end(p);
}

/* everything gathered since the last draw */
static void apply_deferred_state(void)
{
	if (present_pending)
		resolve_pending_present();
	if (device.dirty & DIRTY_POINT_PARAMS)
		apply_point_parameters();
	if (device.dirty & DIRTY_SHADER_STAGES)
		apply_shader_stages();
	if (device.dirty & DIRTY_TEXTURE_STATE)
		apply_texture_state();
	if (device.dirty & DIRTY_FOG)
		apply_fog();
}

/* ---------- vertex shaders */

/* the element sizes of a data type: units by the count nibble (7 means
three), bytes per unit by the type nibble */
static const unsigned char units_of_element[8] = { 0, 1, 2, 3, 4, 5, 6, 3 };
static const unsigned char bytes_per_unit[8] = { 1, 2, 4, 0, 1, 2, 4, 0 };

static unsigned long element_bytes(unsigned long size_and_type)
{
	return units_of_element[(size_and_type >> 4) & 7] * bytes_per_unit[size_and_type & 7];
}

static void parse_declaration(struct vertex_shader_object *object, const DWORD *declaration)
{
	unsigned long stream = 0;
	unsigned long offset = 0;
	unsigned long constant_dwords = 0;
	const DWORD *cursor;
	int pass;

	for (pass = 0; pass < 2; pass++)
	{
		unsigned long *out = pass ? object->constants : NULL;
		unsigned long last_address = 0xffffffff;

		if (pass && constant_dwords)
		{
			object->constants = malloc(constant_dwords * sizeof(unsigned long));
			out = object->constants;
			if (!out)
				break;
			object->constant_dwords = constant_dwords;
		}
		stream = 0;
		offset = 0;
		for (cursor = declaration; cursor && *cursor != D3DVSD_END(); cursor++)
		{
			DWORD token = *cursor;
			unsigned long token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

			switch (token_type)
			{
			case D3DVSD_TOKEN_STREAM:
				stream = token & D3DVSD_STREAMNUMBERMASK;
				offset = 0;
				break;
			case D3DVSD_TOKEN_STREAMDATA:
				if (token & D3DVSD_DATALOADTYPEMASK)
				{
					unsigned long count = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;

					offset += (token & 0x08000000) ? count : count * 4;
				}
				else if (!pass)
				{
					unsigned long reg = token & D3DVSD_VERTEXREGMASK;
					unsigned long size_and_type = (token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT;

					if (reg < VERTEX_SLOTS)
					{
						object->slots[reg].stream = stream & 15;
						object->slots[reg].offset = offset;
						object->slots[reg].size_and_type = size_and_type;
					}
					offset += element_bytes(size_and_type);
				}
				else
				{
					offset += element_bytes((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
				}
				break;
			case D3DVSD_TOKEN_TESSELLATOR:
				if (!pass && (token & D3DVSD_VERTEXREGMASK) < VERTEX_SLOTS)
					object->slots[token & D3DVSD_VERTEXREGMASK].size_and_type = SLOT_DISABLED;
				break;
			case D3DVSD_TOKEN_CONSTMEM:
				{
					unsigned long count = (token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT;
					unsigned long address = token & 0x7f;
					unsigned long batch;

					if (address != last_address + 1)
					{
						if (out)
						{
							*out++ = METHOD(1, NV097_SET_TRANSFORM_CONSTANT_LOAD);
							*out++ = address + 96;
						}
						constant_dwords += pass ? 0 : 2;
					}
					last_address = address + count - 1;
					while (count)
					{
						batch = count > 8 ? 8 : count;
						if (out)
						{
							*out++ = METHOD(4 * batch, NV097_SET_TRANSFORM_CONSTANT);
							memcpy(out, cursor + 1, 4 * batch * sizeof(unsigned long));
							out += 4 * batch;
						}
						constant_dwords += pass ? 0 : 4 * batch + 1;
						cursor += 4 * batch;
						count -= batch;
					}
				}
				break;
			case D3DVSD_TOKEN_EXT:
				cursor += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
				break;
			default:
				break;
			}
		}
		if (!constant_dwords)
			break;
	}
}

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));
	int slot;

	PLATFORM_TRACE();
	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	for (slot = 0; slot < VERTEX_SLOTS; slot++)
		object->slots[slot].size_and_type = SLOT_DISABLED;
	if (function)
	{
		/* header: program type in the low word, instruction count in the high */
		object->type = function[0] & 0xffff;
		object->instruction_count = function[0] >> 16;
		object->program = malloc(object->instruction_count * 4 * sizeof(DWORD));
		if (!object->program)
		{
			free(object);
			return E_OUTOFMEMORY;
		}
		memcpy(object->program, function + 1, object->instruction_count * 4 * sizeof(DWORD));
	}
	parse_declaration(object, declaration);
	/* odd values are FVF codes; programmable shader handles are even */
	*handle = (DWORD)object;
	return S_OK;
}

static struct vertex_shader_object *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader_object *object = (struct vertex_shader_object *)handle;

	if (!handle || (handle & 1) || object->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	PLATFORM_TRACE();
	if (object)
	{
		if (device.vertex_shader == object)
			device.vertex_shader = NULL;
		object->signature = 0;
		free(object->program);
		free(object->constants);
		free(object);
	}
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);
	unsigned long dwords, done = 0;
	unsigned long *p;

	PLATFORM_TRACE();
	if (!object || !object->program)
		return;
	dwords = object->instruction_count * 4;
	p = push_begin(dwords + dwords / 32 + object->constant_dwords + 4);
	p = push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, address);
	while (done < dwords)
	{
		unsigned long batch = dwords - done > 32 ? 32 : dwords - done;

		p[0] = METHOD(batch, NV097_SET_TRANSFORM_PROGRAM);
		memcpy(p + 1, object->program + done, batch * sizeof(unsigned long));
		p += batch + 1;
		done += batch;
	}
	if (object->constant_dwords)
	{
		memcpy(p, object->constants, object->constant_dwords * sizeof(unsigned long));
		p += object->constant_dwords;
	}
	push_end(p);
}

static void SelectVertexShader_body(DWORD handle, DWORD address)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);
	unsigned long *p;

	PLATFORM_TRACE();
	if (object)
	{
		device.vertex_shader = object;
		device.dirty |= DIRTY_VERTEX_FORMAT | DIRTY_VERTEX_OFFSETS;
		p = push_begin(24);
		p = push2(p, NV097_SET_TRANSFORM_EXECUTION_MODE, 6, object->type != 0x2078);
		p = push_viewport(p);
		push_end(p);
	}
	emit1(NV097_SET_TRANSFORM_PROGRAM_START, address);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	TIMED_BEGIN;
	SelectVertexShader_body(handle, address);
	TIMED_END(TIMER_SHADERS);
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	PLATFORM_TRACE();
	D3DDevice_LoadVertexShader(handle, 0);
	D3DDevice_SelectVertexShader(handle, 0);
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	PLATFORM_TRACE();
	*size = object ? object->instruction_count : 0;
}

static void SetVertexShaderConstant_body(INT reg, CONST void *constant_data, DWORD constant_count)
{
	unsigned long dwords = constant_count * 4;
	const unsigned char *source = constant_data;
	unsigned long *p;

	PLATFORM_TRACE();
	if (!constant_count)
		return;
	p = push_begin(dwords + dwords / 32 + 4);
	p = push1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, (unsigned long)(reg + 96));
	while (dwords)
	{
		unsigned long batch = dwords > 32 ? 32 : dwords;

		p[0] = METHOD(batch, NV097_SET_TRANSFORM_CONSTANT);
		memcpy(p + 1, source, batch * sizeof(unsigned long));
		p += batch + 1;
		source += batch * sizeof(unsigned long);
		dwords -= batch;
	}
	push_end(p);
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	TIMED_BEGIN;
	SetVertexShaderConstant_body(reg, constant_data, constant_count);
	TIMED_END(TIMER_CONSTANTS);
}

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	PLATFORM_TRACE();
	device.constant_mode = mode;
}

/* ---------- vertex data and drawing */

void WINAPI D3DDevice_SetStreamSource(UINT stream, D3DVertexBuffer *buffer, UINT stride)
{
	PLATFORM_TRACE();
	if (stream >= 16)
		return;
	/* the game sets stream 0 before every draw: the same, nothing to redo
	(the offsets' base, SetIndices', is compared at the draw) */
	if (device.streams[stream].buffer == buffer && device.streams[stream].stride == stride &&
		device.streams[stream].data == (buffer ? buffer->Data : 0))
		return;
	if (device.streams[stream].stride != stride)
		device.dirty |= DIRTY_VERTEX_FORMAT;
	device.streams[stream].data = buffer ? buffer->Data : 0;
	device.streams[stream].stride = stride;
	device.streams[stream].buffer = buffer;
	device.dirty |= DIRTY_VERTEX_OFFSETS;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *buffer, UINT base_vertex_index)
{
	PLATFORM_TRACE();
	/* index buffers hold ordinary memory (d3d8_resources.c) */
	D3D__IndexData = buffer ? (WORD *)buffer->Data : NULL;
	device.index_base = base_vertex_index;
}

/* the vertex attributes of the current shader from the stream sources:
each slot's type and stride, and its start in memory */
static void apply_vertex_state(unsigned long index_base)
{
	const struct vertex_shader_object *shader = device.vertex_shader;
	unsigned long *p;
	int slot;

	apply_deferred_state();
	if (!shader)
		return;
	if (device.dirty & DIRTY_VERTEX_FORMAT)
	{
		unsigned long formats[VERTEX_SLOTS];

		device.dirty &= ~DIRTY_VERTEX_FORMAT;
		device.dirty |= DIRTY_VERTEX_OFFSETS;
		immediate_arrays_disabled = FALSE;
		for (slot = 0; slot < VERTEX_SLOTS; slot++)
		{
			const struct vertex_slot *element = &shader->slots[slot];

			formats[slot] = element->size_and_type + (device.streams[element->stream].stride << 8);
		}
		if (memcmp(formats, vertex_format_sent, sizeof(formats)))
		{
			memcpy(vertex_format_sent, formats, sizeof(formats));
			forget_vertex_offsets();
			p = push_begin(17);
			p[0] = METHOD(VERTEX_SLOTS, NV097_SET_VERTEX_DATA_ARRAY_FORMAT);
			memcpy(p + 1, formats, sizeof(formats));
			push_end(p + 17);
		}
	}
	if ((device.dirty & DIRTY_VERTEX_OFFSETS) || device.applied_index_base != index_base)
	{
		device.dirty &= ~DIRTY_VERTEX_OFFSETS;
		device.applied_index_base = index_base;
		p = push_begin(2 * VERTEX_SLOTS);
		for (slot = 0; slot < VERTEX_SLOTS; slot++)
		{
			const struct vertex_slot *element = &shader->slots[slot];
			const struct stream_source *stream = &device.streams[element->stream];

			if (element->size_and_type != SLOT_DISABLED && stream->data)
			{
				unsigned long offset = stream->data + element->offset + index_base * stream->stride;

				if (vertex_offset_sent[slot] != offset)
				{
					vertex_offset_sent[slot] = offset;
					p = push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + slot * 4, offset);
				}
			}
		}
		push_end(p);
	}
}



static void DrawVertices_body(D3DPRIMITIVETYPE type, UINT start_vertex, UINT vertex_count)
{
	draw_calls++;
	unsigned long batches = vertex_count ? ((vertex_count - 1) >> 8) + 1 : 0;
	unsigned long *p;

	PLATFORM_TRACE();
	if (!vertex_count)
		return;
	apply_vertex_state(0);
	mark_bound_resources();
	p = push_begin(batches + 6);
	p = push1(p, NV097_SET_BEGIN_END, type);
	p[0] = METHOD_SAME(batches, NV097_DRAW_ARRAYS);
	p++;
	while (vertex_count > 256)
	{
		*p++ = start_vertex | 0xff000000UL;
		start_vertex += 256;
		vertex_count -= 256;
	}
	*p++ = start_vertex | ((vertex_count - 1) << 24);
	p = push1(p, NV097_SET_BEGIN_END, 0);
	push_end(p);
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE type, UINT start_vertex, UINT vertex_count)
{
	TIMED_BEGIN;
	DrawVertices_body(type, start_vertex, vertex_count);
	TIMED_END(TIMER_DRAW);
}

static void DrawIndexedVertices_body(D3DPRIMITIVETYPE type, UINT vertex_count, CONST WORD *indices)
{
	draw_calls++;
	unsigned long *p;

	PLATFORM_TRACE();
	if (!vertex_count || !indices)
		return;
	apply_vertex_state(device.index_base);
	mark_bound_resources();
	p = push_begin(4);
	push_end(push1(p, NV097_SET_BEGIN_END, type));
	while (vertex_count >= 2)
	{
		unsigned long pairs = vertex_count / 2;
		unsigned long batch = pairs > 2047 ? 2047 : pairs;

		p = push_begin(batch + 1);
		p[0] = METHOD_SAME(batch, NV097_ARRAY_ELEMENT16);
		memcpy(p + 1, indices, batch * 2 * sizeof(WORD));
		push_end(p + batch + 1);
		indices += batch * 2;
		vertex_count -= batch * 2;
	}
	p = push_begin(4);
	if (vertex_count)
		p = push1(p, NV097_ARRAY_ELEMENT32, indices[0]);
	p = push1(p, NV097_SET_BEGIN_END, 0);
	push_end(p);
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE type, UINT vertex_count, CONST WORD *indices)
{
	TIMED_BEGIN;
	DrawIndexedVertices_body(type, vertex_count, indices);
	TIMED_END(TIMER_DRAW);
}

/* immediate mode: the vertex data methods, with the current program */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE type)
{
	IMMEDIATE_BEGIN();
	draw_calls++;
	unsigned long *p;
	int slot;

	PLATFORM_TRACE();
	apply_deferred_state();
	mark_bound_resources();
	/* immediate mode takes the attributes written with SetVertexData*
	only where no array is enabled: a buffer draw's arrays would otherwise
	still feed the slots (stale texture coordinates, colours). The next
	buffer draw restores its arrays (DIRTY_VERTEX_FORMAT). */
	if (immediate_arrays_disabled)
	{
		emit1(NV097_SET_BEGIN_END, type);
		return;
	}
	p = push_begin(19);
	p[0] = METHOD(VERTEX_SLOTS, NV097_SET_VERTEX_DATA_ARRAY_FORMAT);
	for (slot = 0; slot < VERTEX_SLOTS; slot++)
	{
		p[1 + slot] = SLOT_DISABLED;
		vertex_format_sent[slot] = SLOT_DISABLED;
	}
	forget_vertex_offsets();
	p = push1(p + 17, NV097_SET_BEGIN_END, type);
	push_end(p);
	device.dirty |= DIRTY_VERTEX_FORMAT;
	immediate_arrays_disabled = TRUE;
}

void WINAPI D3DDevice_End(void)
{
	PLATFORM_TRACE();
	emit1(NV097_SET_BEGIN_END, 0);
	if (immediate_from)
		timer_cycles[TIMER_IMMEDIATE] += cycle_counter() - immediate_from;
	immediate_from = 0;
}


void WINAPI D3DDevice_SetVertexData2f(INT reg, float a, float b)
{
	unsigned long *p = push_begin(3);

	p[0] = METHOD(2, NV097_SET_VERTEX_DATA2F_M + reg * 8);
	p[1] = float_bits(a);
	p[2] = float_bits(b);
	push_end(p + 3);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, float a, float b, float c, float d)
{
	unsigned long *p = push_begin(5);

	push_end(push4f(p, reg == D3DVSDE_VERTEX ? NV097_SET_VERTEX4F : NV097_SET_VERTEX_DATA4F_M + reg * 16, a, b, c, d));
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	emit1(NV097_SET_VERTEX_DATA2S + reg * 4, (unsigned short)a | ((unsigned long)(unsigned short)b << 16));
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	emit1(NV097_SET_VERTEX_DATA4UB + reg * 4, (unsigned long)a | ((unsigned long)b << 8) | ((unsigned long)c << 16) |
		((unsigned long)d << 24));
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	emit1(NV097_SET_VERTEX_DATA4UB + reg * 4, swap_rgb(color));
}

/* ---------- clearing and presenting */

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	D3DSurface *target = device.render_target;
	D3DSurface *depth = bound_depth();
	unsigned long saved_format = 0;
	unsigned long fill_depth = 0;
	D3DRECT whole;
	long left_clip = (long)device.viewport.X, top_clip = (long)device.viewport.Y;
	long right_clip = left_clip + (long)device.viewport.Width, bottom_clip = top_clip + (long)device.viewport.Height;

	PLATFORM_TRACE();
	if (present_pending)
	{
		resolve_pending_present();
		target = device.render_target;
		depth = bound_depth();
	}
	if (!format_is_linear(format_of(target)))
	{
		/* the clear works on a linear surface: switch, clear, switch back */
		saved_format = surface_format_word(target, depth);
		emit1(NV097_SET_SURFACE_FORMAT, (saved_format & ~0x200UL & ~0xffff0000UL) | 0x100);
	}
	if (flags & D3DCLEAR_TARGET)
	{
		switch (format_of(target))
		{
		case D3DFMT_R5G6B5: case D3DFMT_LIN_R5G6B5:
			color = ((color & 0x00f80000UL) >> 8) | ((color & 0x0000fc00UL) >> 5) | ((color & 0x000000f8UL) >> 3);
			break;
		case D3DFMT_X1R5G5B5: case D3DFMT_LIN_X1R5G5B5:
			color = ((color & 0x00f80000UL) >> 9) | ((color & 0x0000f800UL) >> 6) | ((color & 0x000000f8UL) >> 3);
			break;
		default:
			break;
		}
	}
	if (!depth)
		flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
	if (flags & D3DCLEAR_ZBUFFER)
	{
		unsigned long depth_format = format_of(depth);

		if (z < 0.0f)
			z = 0.0f;
		if (z > 1.0f)
			z = 1.0f;
		if (depth_format == D3DFMT_D16 || depth_format == D3DFMT_LIN_D16)
			fill_depth = (unsigned long)(z * 65535.0f);
		else if (format_is_float_depth(depth_format))
			fill_depth = float_bits(z) >> 8 << 8; /* approximate: the float's top bits */
		else
			fill_depth = (unsigned long)(z * 16777215.0f) << 8;
	}
	if (flags)
	{
		if (!count)
		{
			whole.x1 = left_clip;
			whole.y1 = top_clip;
			whole.x2 = right_clip;
			whole.y2 = bottom_clip;
			rectangles = &whole;
			count = 1;
		}
		for (; count; count--, rectangles++)
		{
			long left = rectangles->x1 > left_clip ? rectangles->x1 : left_clip;
			long top = rectangles->y1 > top_clip ? rectangles->y1 : top_clip;
			long right = rectangles->x2 < right_clip ? rectangles->x2 : right_clip;
			long bottom = rectangles->y2 < bottom_clip ? rectangles->y2 : bottom_clip;

			if (left < right && top < bottom)
			{
				/* on the target's pixels, and within them: the menus' offset
				on a widescreen display moves the game's right edge past the
				720 columns, and the console clears past a surface's end
				(into the next rows and beyond) where xemu clips */
				long width = (long)surface_width(target), height = (long)surface_height(target);
				unsigned long *p;

				left = physical_x(left);
				right = physical_x(right);
				top = physical_y(top);
				bottom = physical_y(bottom);
				if (right > width)
					right = width;
				if (bottom > height)
					bottom = height;
				if (left >= right || top >= bottom)
					continue;
				p = push_begin(8);
				p[0] = METHOD(2, NV097_SET_CLEAR_RECT_HORIZONTAL);
				p[1] = (unsigned long)left | ((unsigned long)(right - 1) << 16);
				p[2] = (unsigned long)top | ((unsigned long)(bottom - 1) << 16);
				p[3] = METHOD(3, NV097_SET_ZSTENCIL_CLEAR_VALUE);
				p[4] = fill_depth | (stencil & 0xff);
				p[5] = color;
				p[6] = flags;
				push_end(p + 7);
			}
		}
	}
	if (saved_format)
		emit1(NV097_SET_SURFACE_FORMAT, saved_format);
	/* a frame's first clear (the whole target): to the GPU at once, so it
	starts the frame while the game builds the rest */
	if ((flags & D3DCLEAR_TARGET) && target == &device.back_buffer)
		nx_gpu_flush();
}

static void visibility_frame_end(void);

/* Present's waits, for the statistics (a line every 120 frames): the GPU's
previous frame (fence), the yield, pbkit's full flip queue (table), and the
next frame's buffer leaving the screen (flip, in resolve_pending_present) */
struct wait_total
{
	unsigned long long total;
	unsigned long worst;
};
static struct wait_total wait_fence, wait_yield, wait_table, wait_flip;
static unsigned long wait_table_vblanks, wait_lag_fences, wait_lag_pending;
/* frames whose buffer had not left the screen yet */
static unsigned long buffer_waits;

static void note_wait(struct wait_total *wait, unsigned long long from)
{
	unsigned long elapsed = (unsigned long)(nx_microseconds() - from);

	wait->total += elapsed;
	if (elapsed > wait->worst)
		wait->worst = elapsed;
}

/* Present leaves the next frame's buffer to the frame's first draw: the
next frame is drawn into the buffer of the frame two before (triple
buffered; one before at 720p in the low 64 MB, double buffered), which must
have left the screen first, the display flipped past it. pbkit's full-table
check counts only frames the GPU has finished, so with the frame rate
unlocked (frames faster than the display's) a console's GPU drew into the
frame being shown or waiting to be: torn, flickering, half-drawn frames.
Waiting for that in Present held the CPU from the next frame's ticks too,
and double buffered every frame waited for its own flip: CPU, GPU and the
vertical blank in a row, 16.7 ms frames falling to 33.3 at a few more AI on
the screen. The wait now comes at the next frame's beginning (the game's
frame_begin, halo_xbox_frame_begin, and any draw, clear or back buffer
question before it): the ticks in between run while the GPU finishes and
the display flips. The flip wakes it (flip_event), not a 1 ms sleep. */
static void resolve_pending_present(void)
{
	static unsigned long expiries;
	unsigned long long from;
	int waited = 0;

	if (!present_pending)
		return;
	present_pending = 0;
	from = nx_microseconds();
	while (nx_gpu_back_buffer_in_use())
	{
		/* (never in a steady state: the flip follows within a vertical
		blank of the frame before; past 100 ms the counters are off, and
		the frame goes into a buffer the display still has) */
		if (nx_microseconds() - from > 100000ULL)
		{
			if (expiries++ < 10)
				platform_log("Present: the next frame's buffer is still on the screen after 100 ms (%d frames not shown)",
					nx_gpu_frames_pending());
			break;
		}
		waited = 1;
		if (flip_event)
			nxb_WaitForSingleObject(flip_event, 2);
		else
			nxb_Sleep(1);
	}
	if (waited)
		buffer_waits++;
	note_wait(&wait_flip, from);
	device.back_buffer.Data = nx_gpu_back_buffer();
	frame_buffer_seen(device.back_buffer.Data);
	/* the new back buffer as the target (and the device's window, pitch,
	offsets, clip, format and control: pbkit's flip used to re-target its
	own, and with only the DMA windows put back the next draw went to offset
	0 of the all-memory window, physical 0, the kernel) */
	apply_render_target();
}

/* the game's frame beginning (rasterizer.c, rasterizer_frame_begin), before
it reads last frame's lens flare answers: as when Present waited */
void halo_xbox_frame_begin(void)
{
	resolve_pending_present();
}

/* the time frames spent waiting for visibility answers, for the statistics */
static unsigned long long query_wait_total;

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle, void *destination_window,
	void *dirty_region)
{
	static unsigned long frames;

	PLATFORM_TRACE();
	(void)source_rectangle;
	(void)destination_rectangle;
	(void)destination_window;
	(void)dirty_region;
	unsigned long long waited_from, step_from;

	resolve_pending_present();
	waited_from = nx_microseconds();
	vertical_blank_start();
	/* how far behind the GPU is at the frame's end, for the statistics */
	if (fence_done)
	{
		wait_lag_fences += fence_time - *fence_done;
		wait_lag_pending += (unsigned long)nx_gpu_frames_pending();
	}
	/* the frame's fence: locks wait for a frame's draws at most */
	if (fence_done)
	{
		static unsigned long previous_frame_fence;

		push_fence();
		/* and the CPU goes at most one frame ahead of the GPU: the GPU has
		drawn the frame before this one (its fence; locks push others in
		between). The game was written for that (the retail runtime's
		pacing, and the vertical blank throttle, keep it so); with the frame
		rate unlocked nothing else held the CPU back, up to three frames
		ahead with pbkit's flip queue, and on a console (not the emulator,
		whose GPU keeps up) the game rewrote what the GPU had still to draw:
		torn and flickering geometry */
		step_from = nx_microseconds();
		if (previous_frame_fence)
			wait_for_fence(previous_frame_fence);
		previous_frame_fence = fence_time;
		note_wait(&wait_fence, step_from);
	}
	visibility_frame_end();
	/* a turn, once a frame, for the threads of the game's priority (the
	network stack's): with the frame rate unlocked and the game held by
	the processor, the renderer otherwise never waits */
	step_from = nx_microseconds();
	nxb_SwitchToThread();
	note_wait(&wait_yield, step_from);
	/* the flip queue holds three frames; a full one means the display has
	frames to show first */
	step_from = nx_microseconds();
	while (nx_gpu_finish() != 0)
	{
		wait_table_vblanks++;
		nx_gpu_wait_for_vblank();
	}
	note_wait(&wait_table, step_from);
	pending_flips++;
	/* the next frame's buffer and target: when it starts to draw
	(resolve_pending_present), so that its ticks run meanwhile */
	present_pending = 1;
	nx_gpu_flush();
	present_wait_microseconds += nx_microseconds() - waited_from;
	++frames;
	/* (in performance mode too: a line every 120 frames, and the only place
	the waits in Present show) */
	if (frames % 120 == 0)
	{
		/* the frame statistics: where a frame's time goes */
		unsigned long long now = nx_microseconds();
		unsigned long kicks, dwords, ring_wait;
		unsigned long frame_microseconds = (unsigned long)((now - stats_from) / 120);
		unsigned long present_microseconds = (unsigned long)(present_wait_microseconds / 120);

		nx_gpu_stats(&kicks, &dwords, &ring_wait);
		unsigned long long cycles_now = cycle_counter(), frame_cycles = cycles_now - frame_from_cycles;
		unsigned long percent[TIMER_COUNT], which;

		for (which = 0; which < TIMER_COUNT; which++)
		{
			percent[which] = frame_cycles ? (unsigned long)(timer_cycles[which] * 100 / frame_cycles) : 0;
			timer_cycles[which] = 0;
		}
		frame_from_cycles = cycles_now;
		platform_log("frame %lu: %lu.%lu ms/frame, %lu draws, %lu kicks, %lu KB pushed, %lu.%lu ms waiting for the ring, %lu.%lu ms in present; "
			"cpu in draws %lu%%, constants %lu%%, immediate %lu%%, textures %lu%%, shaders %lu%%; %lu stale query answers, "
			"%lu.%lu ms waiting for them; %lu frames waited for their buffer",
			frames, frame_microseconds / 1000, (frame_microseconds / 100) % 10, draw_calls / 120, kicks / 120, dwords * 4 / 1024 / 120,
			ring_wait / 120 / 1000, (ring_wait / 120 / 100) % 10, present_microseconds / 1000, (present_microseconds / 100) % 10,
			percent[TIMER_DRAW], percent[TIMER_CONSTANTS], percent[TIMER_IMMEDIATE], percent[TIMER_TEXTURES], percent[TIMER_SHADERS],
			stale_query_answers / 120, (unsigned long)(query_wait_total / 120 / 1000), (unsigned long)(query_wait_total / 120 / 100 % 10), buffer_waits);
		{
			unsigned long played, late;

			nx_audio_statistics(&played, &late);
			platform_log("audio: %lu buffers played, %lu late", played, late);
		}
		/* Present's waits, averaged over the 120 frames (the worst in
		brackets), and how far behind the GPU was at the frames' ends */
		platform_log("waits: gpu %lu.%lu (%lu.%lu), yield %lu.%lu, table %lu.%lu (%lu vblanks), flip %lu.%lu (%lu.%lu) ms; "
			"gpu behind %lu.%lu fences, %lu.%lu frames not shown",
			(unsigned long)(wait_fence.total / 120 / 1000), (unsigned long)(wait_fence.total / 120 / 100 % 10),
			wait_fence.worst / 1000, wait_fence.worst / 100 % 10,
			(unsigned long)(wait_yield.total / 120 / 1000), (unsigned long)(wait_yield.total / 120 / 100 % 10),
			(unsigned long)(wait_table.total / 120 / 1000), (unsigned long)(wait_table.total / 120 / 100 % 10), wait_table_vblanks,
			(unsigned long)(wait_flip.total / 120 / 1000), (unsigned long)(wait_flip.total / 120 / 100 % 10),
			wait_flip.worst / 1000, wait_flip.worst / 100 % 10,
			wait_lag_fences / 120, wait_lag_fences * 10 / 120 % 10, wait_lag_pending / 120, wait_lag_pending * 10 / 120 % 10);
		memset(&wait_fence, 0, sizeof(wait_fence));
		memset(&wait_yield, 0, sizeof(wait_yield));
		memset(&wait_table, 0, sizeof(wait_table));
		memset(&wait_flip, 0, sizeof(wait_flip));
		wait_table_vblanks = wait_lag_fences = wait_lag_pending = 0;
		{
			/* the room left as the game runs (720p on a 64 MB console is
			sized by it) */
			static unsigned long lowest = ~0UL;
			unsigned long total_bytes = 0, available_bytes = 0;

			nx_memory_status(&total_bytes, &available_bytes);
			if (available_bytes < lowest)
				lowest = available_bytes;
			platform_log("memory: %lu KB free (lowest %lu KB) of %lu MB", available_bytes >> 10, lowest >> 10,
				(total_bytes + 0x80000UL) >> 20);
		}
		stale_query_answers = 0;
		query_wait_total = 0;
		buffer_waits = 0;
		draw_calls = 0;
		present_wait_microseconds = 0;
		stats_from = now;
	}
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	PLATFORM_TRACE();
	return S_OK;
}

/* ---------- visibility (occlusion) tests

The GPU counts the pixels that pass the depth test between the begin and
the end, and writes a report (time stamp, count, status) where the end
says. */

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
	unsigned long *p = push_begin(3);

	PLATFORM_TRACE();
	push_end(push2(p, NV097_CLEAR_REPORT_VALUE, 1, 1));
}

/* The game numbers its tests within a frame (a lens flare's place in the
frame's list) and reads each answer in the frame that asked. With the
frame rate unlocked the CPU starts a frame while the GPU still draws the
one before, whose reports for the same numbers would land after this
frame's end marked them pending: the game took another frame's answer,
often another flare's, and flares flashed on and off over the walls. Each
frame parity has its own half of the report page (Present switches), and
the GPU is never more than a frame behind, so a half is free again when
its parity comes back. The game reads a lens flare's answer in the next
frame, though (rasterizer_lights_begin_for_new_frame, from frame_begin, after
Present switched halves): each test number remembers the slot its last test
went to (issued_slot), and the answer comes from there, whichever frame
reads it (before, the next frame read the other half: two frames old, and
another flare's whenever the list had shifted). */
static unsigned long query_half;
/* Slots go to the tests in the order they come within the frame's half (the
game numbers its tests by a flare's place in a list of up to 1024, and the
transparent pixel counter uses 0xfff: by number they shared slots), and each
number remembers its slot and the frame it was asked in, so a number not
asked last frame or this one has no answer. */
#define TEST_NUMBERS 4096
static unsigned short issued_slot[TEST_NUMBERS];
static unsigned long issued_frame[TEST_NUMBERS]; /* the frame + 1; 0 never */
static unsigned long query_frame = 1;
static unsigned long next_query_slot;
static int query_overflow_reported;
/* the time a frame may spend waiting for answers before "not ready" */
#define QUERY_WAIT_BUDGET_MICROSECONDS 2000ULL
static unsigned long long query_wait_spent;
#ifndef S_FALSE
#define S_FALSE ((HRESULT)1L)
#endif

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	struct visibility_report *report;
	unsigned long *p;
	unsigned long number = index & (TEST_NUMBERS - 1), slot;

	PLATFORM_TRACE();
	if (!device.reports)
		return E_OUTOFMEMORY;
	if (next_query_slot >= QUERY_SLOTS)
	{
		/* a frame with more tests than its half holds: this one goes
		unanswered (its flare keeps its last visibility) */
		if (!query_overflow_reported)
		{
			query_overflow_reported = 1;
			platform_log("visibility: more than %d tests in a frame; the rest go unanswered", (int)QUERY_SLOTS);
		}
		issued_frame[number] = 0;
		emit1(NV097_SET_ZPASS_PIXEL_COUNT_ENABLE, 0);
		return S_OK;
	}
	slot = next_query_slot++ + query_half * QUERY_SLOTS;
	report = &device.reports[slot];
	report->status = 0xffffffff;
	issued_slot[number] = (unsigned short)slot;
	issued_frame[number] = query_frame;
	p = push_begin(3);
	push_end(push2(p, NV097_SET_ZPASS_PIXEL_COUNT_ENABLE, 0, (slot * sizeof(*report)) | 0x01000000));
	/* (no kick for each: the lens flares' answers are read in the next
	frame, and a reader in this frame kicks first, GetVisibilityTestResult) */
	return S_OK;
}

/* S_FALSE (and a count of 0): no answer yet, or none for this number. The
game keeps the flare's last visibility then (rasterizer_xbox_widgets.c and
rasterizer_lights.c, HALO_XBOX) instead of taking another test's count. */
HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	const volatile struct visibility_report *report;
	unsigned long number = index & (TEST_NUMBERS - 1);
	unsigned long asked = issued_frame[number];

	PLATFORM_TRACE();
	if (result)
		*result = 0;
	if (time_stamp)
		*time_stamp = 0;
	if (!device.reports)
		return S_OK;
	/* asked this frame or the last (the flares' answers are read in the
	next frame's beginning) */
	if (!asked || query_frame - asked > 1)
		return S_FALSE;
	report = &device.reports[issued_slot[number]];
	if (report->status == 0xffffffff)
	{
		/* the GPU has not reached the test yet. On a console it runs close
		behind and answers in a moment, so the device waits, within a budget
		a frame; past it (the emulator, where it is the whole frame's GPU
		work) the answer is "not ready", and a flare fades a frame late */
		unsigned long long from = nx_microseconds(), now = from;

		nx_gpu_flush();
		while (report->status == 0xffffffff && query_wait_spent + (now - from) < QUERY_WAIT_BUDGET_MICROSECONDS)
		{
			__asm__ __volatile__("pause");
			now = nx_microseconds();
		}
		query_wait_spent += now - from;
		if (report->status == 0xffffffff)
		{
			stale_query_answers++;
			return S_FALSE;
		}
	}
	{
		/* in the game's pixels: the buffer's are scale_x by scale_y of them */
		float area = (device.scale_x > 0.0f ? device.scale_x : 1.0f) * (device.scale_y > 0.0f ? device.scale_y : 1.0f);
		UINT samples = area != 1.0f ? (UINT)((float)report->samples / area + 0.5f) : report->samples;

		if (result)
			*result = samples;
	}
	if (time_stamp)
		*time_stamp = ((ULONGLONG)report->time_stamp_high << 32) | report->time_stamp_low;
	return S_OK;
}

/* Present: the next frame's tests use the other half, from its start */
static void visibility_frame_end(void)
{
	query_half ^= 1;
	next_query_slot = 0;
	query_frame++;
	query_wait_total += query_wait_spent;
	query_wait_spent = 0;
}

/* ---------- the rest of the device */

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	PLATFORM_TRACE();
	if ((unsigned long)state < D3DTS_MAX)
		device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	PLATFORM_TRACE();
	if ((unsigned long)state < D3DTS_MAX)
		*matrix = device.transforms[state];
	else
		memset(matrix, 0, sizeof(*matrix));
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { PLATFORM_TRACE(); (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { PLATFORM_TRACE(); (void)enable; }

void halo_xbox_gpu_drain(void)
{
	nx_gpu_drain();
}

BOOL WINAPI D3DDevice_IsBusy(void)
{
	PLATFORM_TRACE();
	return nx_gpu_started() ? nx_gpu_busy() : FALSE;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
	PLATFORM_TRACE();
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	PLATFORM_TRACE();
	(void)type;
	/* the profiler's timing callbacks: reached at once */
	if (callback)
		callback(context);
}

/* ---------- creation */

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	static int direct3d_object;

	PLATFORM_TRACE();
	(void)sdk_version;
	return (Direct3D *)&direct3d_object;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	PLATFORM_TRACE();
	(void)segment_count;
	device.push_buffer_size = push_buffer_size;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	PLATFORM_TRACE();
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxVolumeExtent = 512;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 4096;
	caps->MaxAnisotropy = 4;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 192;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

/* the runtime's initial render and texture stage states
(libs/d3d8/initial_states.inc), set through the same paths a game uses */
static const DWORD initial_render_states[D3DRS_MAX - D3DRS_PS_MAX] =
{
	0x203, 0x207, 0, 0, 0, 1, 0, 1, 0, 0x1d01, 0x01010101, 0x1e00, 0x1e00, 0x207, 0, 0xffffffff, 0xffffffff, 0x8006, 0, 4,
	0, 0, 0, 0, 0, /* polygon offsets */
	0, 0, 0, 0x3f800000, 0x3f800000, 0, /* fog */
	0, 0, 0, 0, /* wraps */
	1, 0, 1, 1, 2, 1, 0, 0, 2, 1, 0, 0, 0, 0, /* lighting and materials */
	0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0x42800000, /* points */
	0, 0x3f800000, /* patches */
	0, 0, 0, 0x1b02, 0x1b02, 0, 0, 2, 0, 0x1e00, 0x900, 0x901, 0xffffffff, 0, 0, 0, 1, 0xffffffff, 0xdeadbeef, 0x200,
	0x3f800000, 1, 0, 1, 1, 0, 0, 0,
};

static const BYTE initial_texture_states[D3DTSS_MAX] =
{
	1, 1, 2, 1, 1, 1, 2, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static void set_initial_states(void)
{
	unsigned long index, stage;

	for (index = D3DRS_PS_MAX; index < D3DRS_MAX; index++)
	{
		if (index != D3DRS_MULTISAMPLETYPE)
			D3DDevice_SetRenderStateNotInline((D3DRENDERSTATETYPE)index, initial_render_states[index - D3DRS_PS_MAX]);
	}
	D3D__RenderState[D3DRS_MULTISAMPLETYPE] = D3DMULTISAMPLE_NONE;
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		for (index = 0; index < D3DTSS_MAX; index++)
			D3DDevice_SetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)index, initial_texture_states[index]);
		D3DDevice_SetTextureStageState(stage, D3DTSS_TEXCOORDINDEX, stage);
	}
}

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	unsigned long width, height, pitch;
	int index;

	PLATFORM_TRACE();
	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	if (!device.created)
	{
		unsigned long push_buffer_size = device.push_buffer_size;

		memset(&device, 0, sizeof(device));
		dma_contexts_bound = FALSE;
		device.push_buffer_size = push_buffer_size;
		/* every deferred group applied once, whatever the game sets: the
		setters skip a value the tables already hold */
		device.dirty = DIRTY_FOG | DIRTY_TEXTURE_STATE | DIRTY_SHADER_STAGES | DIRTY_VERTEX_FORMAT |
			DIRTY_VERTEX_OFFSETS | DIRTY_POINT_PARAMS;
		if (presentation_parameters)
			device.presentation = *presentation_parameters;
		width = device.presentation.BackBufferWidth ? device.presentation.BackBufferWidth : 640;
		height = device.presentation.BackBufferHeight ? device.presentation.BackBufferHeight : 480;
		/* (the game asked for its screen's width first, which chose it) */
		halo_xbox_choose_display_mode();
		if (!nx_gpu_start(push_buffer_size ? push_buffer_size * 2 : 1024 * 1024))
		{
			platform_log("Direct3D: no GPU");
			return E_FAIL;
		}
		pitch = nx_gpu_back_buffer_pitch();
		/* the screen the game draws, and the buffer it lands in: the
		display mode's (nx_video.c); a widescreen display's 854 columns
		are squeezed into its 720, and 720p scales 854x480 by 1.5 */
		device.logical_width = width;
		device.logical_height = height;
		if (nx_gpu_back_buffer_width())
			width = nx_gpu_back_buffer_width();
		if (nx_gpu_back_buffer_height())
			height = nx_gpu_back_buffer_height();
		device.scale_x = (float)width / (float)device.logical_width;
		device.scale_y = (float)height / (float)device.logical_height;
		device.ui_offset = 0;
		platform_log("screen: %lux%lu drawn in %lux%lu", device.logical_width, device.logical_height, width, height);
		/* textures and vertices through the all-memory window too: at 720p
		they are in the top 64 MB, beyond pbkit's windows (its flips rebind
		only colour and depth) */
		if (nx_high_memory_placed())
		{
			unsigned long *p = push_begin(8);

			p = push1(p, NV097_SET_CONTEXT_DMA_A, NX_GPU_MEMORY_CONTEXT_COLOR);
			p = push1(p, NV097_SET_CONTEXT_DMA_B, NX_GPU_MEMORY_CONTEXT_COLOR);
			p = push1(p, NV097_SET_CONTEXT_DMA_VERTEX_A, NX_GPU_MEMORY_CONTEXT_COLOR);
			p = push1(p, NV097_SET_CONTEXT_DMA_VERTEX_B, NX_GPU_MEMORY_CONTEXT_COLOR);
			push_end(p);
		}
		/* the back buffer: pbkit's, which changes at every flip */
		device.back_buffer.Common = D3DCOMMON_TYPE_SURFACE | 1;
		device.back_buffer.Data = nx_gpu_back_buffer();
		frame_buffer_seen(device.back_buffer.Data);
		device.back_buffer.Format = ((DWORD)D3DFMT_LIN_A8R8G8B8 << D3DFORMAT_FORMAT_SHIFT) |
			(2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_DMACHANNEL_A | D3DFORMAT_BORDERSOURCE_COLOR;
		device.back_buffer.Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
			((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
		/* the depth buffer: the device's own, so that it has a physical
		address (a game header naming its memory as a colour surface then
		works: the screen effects copy the frame there); without memory for
		it, pbkit's at offset 0 of pbkit's window, and those copies land
		nowhere useful */
		device.depth_buffer.Common = D3DCOMMON_TYPE_SURFACE | 1;
		device.depth_buffer.Data = nx_gpu_depth_buffer(pitch * height);
		if (!device.depth_buffer.Data)
			platform_log("no memory for a depth buffer of the device's own: screen effects will be wrong");
		device.depth_buffer.Format = ((DWORD)D3DFMT_LIN_D24S8 << D3DFORMAT_FORMAT_SHIFT) |
			(2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_DMACHANNEL_A | D3DFORMAT_BORDERSOURCE_COLOR;
		device.depth_buffer.Size = device.back_buffer.Size;
		/* the visibility reports: the page the report DMA context spans */
		device.reports = nx_gpu_reports();
		if (device.reports)
		{
			unsigned long *p = push_begin(6);

			p = push1(p, NV097_SET_CONTEXT_DMA_REPORT, NX_GPU_MEMORY_CONTEXT_REPORT);
			/* the fences' semaphore (pbkit binds its own, which it never
			writes) */
			p = push1(p, NV097_SET_CONTEXT_DMA_SEMAPHORE, NX_GPU_MEMORY_CONTEXT_REPORT);
			p = push1(p, NV097_SET_SEMAPHORE_OFFSET, FENCE_OFFSET);
			push_end(p);
			fence_done = (volatile unsigned long *)((char *)device.reports + FENCE_OFFSET);
			*fence_done = 0;
			fence_time = 0;
		}
		for (index = 0; index < D3DTS_MAX; index++)
		{
			device.transforms[index]._11 = 1.0f;
			device.transforms[index]._22 = 1.0f;
			device.transforms[index]._33 = 1.0f;
			device.transforms[index]._44 = 1.0f;
		}
		device.applied_index_base = 0xffffffff;
		forget_texture_registers();
		device.created = TRUE;
		D3DDevice_SetRenderTarget(&device.back_buffer, &device.depth_buffer);
		set_initial_states();
		D3DDevice_SetRenderStateNotInline(D3DRS_ZENABLE, TRUE);
		emit1(NV097_SET_LIGHTING_ENABLE, FALSE);
		emit1(NV097_SET_SPECULAR_ENABLE, TRUE);
		D3DDevice_Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
		platform_log("Direct3D: the NV2A device, %lux%lu, pitch %lu", width, height, pitch);
	}
	*returned_device = device_pointer();
	return S_OK;
}

ULONG WINAPI D3DDevice_Release(void)
{
	PLATFORM_TRACE();
	return 1;
}
