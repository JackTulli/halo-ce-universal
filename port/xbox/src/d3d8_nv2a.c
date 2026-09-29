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
#define NV097_SET_CONTEXT_DMA_COLOR 0x0194
#define NV097_SET_CONTEXT_DMA_ZETA 0x0198
#define NV097_SET_CONTEXT_DMA_REPORT 0x01A8
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
#define VISIBILITY_REPORTS (NX_GPU_REPORT_BYTES / 16)

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
	(d3d8_nv2a_set_ui_offset). device.viewport stays in the game's columns */
	unsigned long logical_width;
	float scale_x;
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

/* the surface format register: the colour and depth formats, linear or
swizzled (with the swizzled sizes) */
static unsigned long surface_format_word(const D3DSurface *target, const D3DSurface *depth)
{
	unsigned long format = format_of(target);
	unsigned long color, zeta = 2, word;

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

		if (depth_format == D3DFMT_D16 || depth_format == D3DFMT_LIN_D16 || depth_format == D3DFMT_F16 ||
			depth_format == D3DFMT_LIN_F16)
			zeta = 1;
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
	}
	return 0;
}

static void vertical_blank_start(void)
{
	if (!vertical_blank_thread)
	{
		if (!vertical_blank_event)
			vertical_blank_event = nxb_CreateEventA(NULL, FALSE, FALSE, NULL);
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
static unsigned long target_logical_width(const D3DSurface *target);

static unsigned long *push_viewport(unsigned long *p)
{
	long left = physical_x((long)device.viewport.X);
	long right = physical_x((long)(device.viewport.X + device.viewport.Width));
	float x = (float)left + 0.53125f;
	float y = (float)device.viewport.Y + 0.53125f;
	float half_width = (float)(right - left) * 0.5f;
	float half_height = (float)device.viewport.Height * -0.5f;
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
	if (device.depth_stencil && format_is_float_depth(format_of(device.depth_stencil)))
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
static void apply_render_target(void)
{
	D3DSurface *target = device.render_target;
	D3DSurface *depth = device.depth_stencil;
	unsigned long width = surface_width(target), height = surface_height(target);
	unsigned long pitch = surface_pitch(target);
	unsigned long depth_pitch = depth ? surface_pitch(depth) : pitch;
	unsigned long *p;
	int repeat;

	if (target->Data < 0x61000)
	{
		/* a target in the kernel's or the executable's memory, below the
		game state at physical 0x61000 (SetRenderTarget maps the game's
		offset-0 header to the back buffer; anything else here is a
		surface without memory): the previous target stays. */
		static int reports;

		if (reports++ < 8)
			platform_log("render target %p at physical %08lx (%lux%lu pitch %lu, format %08lx size %08lx): refused", (void *)target,
				(unsigned long)target->Data, width, height, pitch, (unsigned long)target->Format, (unsigned long)target->Size);
		return;
	}
	p = push_begin(80);

	p = push1(p, NV097_SET_CONTEXT_DMA_COLOR, NX_GPU_MEMORY_CONTEXT_COLOR);
	p = push1(p, NV097_SET_CONTEXT_DMA_ZETA, NX_GPU_MEMORY_CONTEXT_COLOR);
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
		p = push1(p, NV097_SET_SURFACE_ZETA_OFFSET, depth ? depth->Data : 0);
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
	unsigned long width = target_logical_width(target), height = surface_height(target);
	unsigned long left = viewport->X, top = viewport->Y;
	unsigned long right = viewport->X + viewport->Width, bottom = viewport->Y + viewport->Height;
	unsigned long *p;

	PLATFORM_TRACE();
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

/* the columns of the current target the game may draw on */
static unsigned long target_logical_width(const D3DSurface *target)
{
	return screen_target(target) ? device.logical_width : surface_width(target);
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
	device.depth_stencil = depth_stencil;
	device.z_scale = depth_scale(depth_stencil);
	apply_render_target();
	viewport.X = 0;
	viewport.Y = 0;
	viewport.Width = target_logical_width(device.render_target);
	viewport.Height = surface_height(device.render_target);
	viewport.MinZ = 0.0f;
	viewport.MaxZ = 1.0f;
	D3DDevice_SetViewport(&viewport);
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	PLATFORM_TRACE();
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

	emit1(NV097_SET_DEPTH_TEST_ENABLE, value != D3DZB_FALSE && device.depth_stencil != NULL);
	D3D__RenderState[D3DRS_ZENABLE] = value;
	if (previous == D3DZB_USEW || value == D3DZB_USEW)
		emit1(NV097_SET_CONTROL0, control0_word());
}

void WINAPI D3DDevice_SetRenderState_StencilEnable(DWORD value)
{
	unsigned long *p = push_begin(4);

	p = push_occlude_zstencil(p);
	p = push1(p, NV097_SET_STENCIL_TEST_ENABLE, value && device.depth_stencil != NULL);
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
	emit1(0x1D74, value - D3DCMP_NEVER); /* NV097_SET_SHADOW_DEPTH_FUNC */
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
#define TIMED_BEGIN unsigned long long timed_from = cycle_counter()
#define TIMED_END(which) (timer_cycles[which] += cycle_counter() - timed_from)

static unsigned long long present_wait_microseconds, stats_from;

/* ---------- textures */

static void SetTexture_body(DWORD stage, D3DBaseTexture *texture)
{
	unsigned long *p;



	PLATFORM_TRACE();
	if (stage >= D3DTSS_MAXSTAGES)
		return;
	device.textures[stage] = texture;
	device.dirty |= DIRTY_TEXTURE_STATE | DIRTY_SHADER_STAGES;
	if (!texture)
	{
		emit1(NV097_SET_TEXTURE_CONTROL0 + stage * TEXTURE_STAGE_STRIDE, 0);
		return;
	}
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
		p = push_begin(8);
		p = push2(p, NV097_SET_TEXTURE_OFFSET + stage * TEXTURE_STAGE_STRIDE, data, format);
	}
	if (texture->Size)
	{
		unsigned long width = (texture->Size & D3DSIZE_WIDTH_MASK) + 1;
		unsigned long height = ((texture->Size & D3DSIZE_HEIGHT_MASK) >> D3DSIZE_HEIGHT_SHIFT) + 1;

		p = push1(p, NV097_SET_TEXTURE_CONTROL1 + stage * TEXTURE_STAGE_STRIDE, texture_pitch(texture) << 16);
		p = push1(p, NV097_SET_TEXTURE_IMAGE_RECT + stage * TEXTURE_STAGE_STRIDE, (width << 16) | height);
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
		unsigned long wrap = D3D__RenderState[D3DRS_WRAP0 + stage];
		unsigned long address = address_mode(state[D3DTSS_ADDRESSU]) | (address_mode(state[D3DTSS_ADDRESSV]) << 8) |
			(address_mode(state[D3DTSS_ADDRESSW]) << 16);
		unsigned long control0 = 0;
		unsigned long filter;
		unsigned long anisotropy = state[D3DTSS_MAXANISOTROPY];
		long bias = (long)(bits_float(state[D3DTSS_MIPMAPLODBIAS]) * 256.0f);
		unsigned long magnify;

		if (wrap & 1)
			address |= 1UL << 4;
		if (wrap & 2)
			address |= 1UL << 12;
		if (wrap & 4)
			address |= 1UL << 20;
		if (wrap & 8)
			address |= 1UL << 24;
		if (device.textures[stage])
		{
			/* the level range, in levels: the finest level the game allows
			in bits 18-29, no coarse limit in bits 6-17 (xemu reads them so) */
			control0 = 0x40000000UL | ((state[D3DTSS_MAXMIPLEVEL] & 0xfff) << 18) | (0xfffUL << 6) |
				(state[D3DTSS_COLORKEYOP] & 3) | (state[D3DTSS_ALPHAKILL] & 4);
			if (state[D3DTSS_MINFILTER] == D3DTEXF_ANISOTROPIC && anisotropy > 1)
				control0 |= (anisotropy >= 8 ? 3UL : anisotropy >= 4 ? 2UL : 1UL) << 4;
		}
		if (bias < -4096)
			bias = -4096;
		if (bias > 4095)
			bias = 4095;
		filter = ((unsigned long)bias & 0x1fff) |
			(minification_code(state[D3DTSS_MINFILTER], state[D3DTSS_MIPFILTER]) << 16);
		switch (state[D3DTSS_MAGFILTER])
		{
		case D3DTEXF_POINT: magnify = 1; break;
		case D3DTEXF_QUINCUNX: magnify = 4; filter |= 1UL << 13; break;
		case D3DTEXF_GAUSSIANCUBIC: magnify = 4; filter |= 2UL << 13; break;
		default: magnify = 2; break;
		}
		filter |= magnify << 24;
		filter |= state[D3DTSS_COLORSIGN] & 0xf0000000UL;
		p = push1(p, NV097_SET_TEXTURE_ADDRESS + stage * TEXTURE_STAGE_STRIDE, address);
		p = push1(p, NV097_SET_TEXTURE_CONTROL0 + stage * TEXTURE_STAGE_STRIDE, control0);
		p = push1(p, NV097_SET_TEXTURE_FILTER + stage * TEXTURE_STAGE_STRIDE, filter);
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
	emit1(NV097_SET_SHADER_STAGE_PROGRAM, result);
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
	unsigned long fixed_size;
	unsigned long *p = push_begin(16);

	device.dirty &= ~DIRTY_POINT_PARAMS;
	if (!scale_enable)
	{
		if (size < minimum)
			size = minimum;
		if (size > maximum)
			size = maximum;
		if (size > 64.0f)
			size = 64.0f;
	}
	else
	{
		float height = (float)device.viewport.Height;
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
	if (device.streams[stream].stride != stride)
		device.dirty |= DIRTY_VERTEX_FORMAT;
	device.streams[stream].data = buffer ? buffer->Data : 0;
	device.streams[stream].stride = stride;
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
		device.dirty &= ~DIRTY_VERTEX_FORMAT;
		device.dirty |= DIRTY_VERTEX_OFFSETS;
		immediate_arrays_disabled = FALSE;
		p = push_begin(17);
		p[0] = METHOD(VERTEX_SLOTS, NV097_SET_VERTEX_DATA_ARRAY_FORMAT);
		for (slot = 0; slot < VERTEX_SLOTS; slot++)
		{
			const struct vertex_slot *element = &shader->slots[slot];

			p[1 + slot] = element->size_and_type + (device.streams[element->stream].stride << 8);
		}
		push_end(p + 17);
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
				p = push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + slot * 4,
					stream->data + element->offset + index_base * stream->stride);
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
	immediate_from = cycle_counter();
	draw_calls++;
	unsigned long *p;
	int slot;

	PLATFORM_TRACE();
	apply_deferred_state();
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
		p[1 + slot] = SLOT_DISABLED;
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
	D3DSurface *depth = device.depth_stencil;
	unsigned long saved_format = 0;
	unsigned long fill_depth = 0;
	D3DRECT whole;
	long left_clip = (long)device.viewport.X, top_clip = (long)device.viewport.Y;
	long right_clip = left_clip + (long)device.viewport.Width, bottom_clip = top_clip + (long)device.viewport.Height;

	PLATFORM_TRACE();
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
				unsigned long *p = push_begin(8);

				left = physical_x(left);
				right = physical_x(right);
				if (right <= left)
					right = left + 1;
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
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle, void *destination_window,
	void *dirty_region)
{
	static unsigned long frames;

	PLATFORM_TRACE();
	(void)source_rectangle;
	(void)destination_rectangle;
	(void)destination_window;
	(void)dirty_region;
	unsigned long long waited_from = nx_microseconds();

	vertical_blank_start();
	/* the flip queue holds three frames; a full one means the display has
	frames to show first */
	while (nx_gpu_finish() != 0)
		nx_gpu_wait_for_vblank();
	present_wait_microseconds += nx_microseconds() - waited_from;
	pending_flips++;
	device.back_buffer.Data = nx_gpu_back_buffer();
	frame_buffer_seen(device.back_buffer.Data);
	/* pbkit's flip re-targets its own buffer and DMA window: back to ours */
	if (device.render_target == &device.back_buffer)
		apply_render_target();
	else
	{
		emit1(NV097_SET_CONTEXT_DMA_COLOR, NX_GPU_MEMORY_CONTEXT_COLOR);
		emit1(NV097_SET_CONTEXT_DMA_ZETA, NX_GPU_MEMORY_CONTEXT_COLOR);
	}
	++frames;
	if (!halo_performance_mode() && frames % 120 == 0)
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
			"cpu in draws %lu%%, constants %lu%%, immediate %lu%%, textures %lu%%, shaders %lu%%; %lu stale query answers",
			frames, frame_microseconds / 1000, (frame_microseconds / 100) % 10, draw_calls / 120, kicks / 120, dwords * 4 / 1024 / 120,
			ring_wait / 120 / 1000, (ring_wait / 120 / 100) % 10, present_microseconds / 1000, (present_microseconds / 100) % 10,
			percent[TIMER_DRAW], percent[TIMER_CONSTANTS], percent[TIMER_IMMEDIATE], percent[TIMER_TEXTURES], percent[TIMER_SHADERS],
			stale_query_answers / 120);
		{
			unsigned long played, late;

			nx_audio_statistics(&played, &late);
			platform_log("audio: %lu buffers played, %lu late", played, late);
		}
		stale_query_answers = 0;
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

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	struct visibility_report *report;
	unsigned long *p;

	PLATFORM_TRACE();
	if (!device.reports)
		return E_OUTOFMEMORY;
	report = &device.reports[index % VISIBILITY_REPORTS];
	report->status = 0xffffffff;
	p = push_begin(3);
	push_end(push2(p, NV097_SET_ZPASS_PIXEL_COUNT_ENABLE, 0,
		((index % VISIBILITY_REPORTS) * sizeof(*report)) | 0x01000000));
	return S_OK;
}

/* the last answer of each visibility test slot */
static UINT query_samples[VISIBILITY_REPORTS];
static ULONGLONG query_time_stamps[VISIBILITY_REPORTS];
static unsigned char query_answered[VISIBILITY_REPORTS];

HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	const volatile struct visibility_report *report;
	unsigned long slot;

	PLATFORM_TRACE();
	if (!device.reports)
	{
		if (result)
			*result = 0;
		if (time_stamp)
			*time_stamp = 0;
		return S_OK;
	}
	slot = index % VISIBILITY_REPORTS;
	report = &device.reports[slot];
	if (report->status == 0xffffffff)
	{
		/* the GPU has not reached the test yet. The game spins on the
		answer in the frame that asked (lens flare occlusion, several a
		frame): a moment on the console, where the GPU runs close behind,
		but here the whole frame's GPU work, with the CPU idle. The slot's
		previous answer serves instead; a flare fades a frame late. */
		nx_gpu_flush();
		if (!query_answered[slot])
			return D3DERR_TESTINCOMPLETE;
		stale_query_answers++;
		if (result)
			*result = query_samples[slot];
		if (time_stamp)
			*time_stamp = query_time_stamps[slot];
		return S_OK;
	}
	query_answered[slot] = 1;
	query_samples[slot] = device.scale_x > 0.0f && device.scale_x != 1.0f ?
		(unsigned long)((float)report->samples / device.scale_x + 0.5f) : report->samples;
	query_time_stamps[slot] = ((ULONGLONG)report->time_stamp_high << 32) | report->time_stamp_low;
	if (result)
		*result = query_samples[slot];
	if (time_stamp)
		*time_stamp = query_time_stamps[slot];
	return S_OK;
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
		device.push_buffer_size = push_buffer_size;
		/* every deferred group applied once, whatever the game sets: the
		setters skip a value the tables already hold */
		device.dirty = DIRTY_FOG | DIRTY_TEXTURE_STATE | DIRTY_SHADER_STAGES | DIRTY_VERTEX_FORMAT |
			DIRTY_VERTEX_OFFSETS | DIRTY_POINT_PARAMS;
		if (presentation_parameters)
			device.presentation = *presentation_parameters;
		width = device.presentation.BackBufferWidth ? device.presentation.BackBufferWidth : 640;
		height = device.presentation.BackBufferHeight ? device.presentation.BackBufferHeight : 480;
		if (!nx_gpu_start(push_buffer_size ? push_buffer_size * 2 : 1024 * 1024))
		{
			platform_log("Direct3D: no GPU");
			return E_FAIL;
		}
		pitch = nx_gpu_back_buffer_pitch();
		/* the screen the game draws, and the buffer it lands in: the
		display mode's (nx_video.c); a widescreen display's 854 columns
		are squeezed into its 720 */
		device.logical_width = width;
		if (nx_gpu_back_buffer_width())
			width = nx_gpu_back_buffer_width();
		if (nx_gpu_back_buffer_height())
			height = nx_gpu_back_buffer_height();
		device.scale_x = (float)width / (float)device.logical_width;
		device.ui_offset = 0;
		platform_log("screen: %lux%lu drawn in %lux%lu", device.logical_width, height, width, height);
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
			emit1(NV097_SET_CONTEXT_DMA_REPORT, NX_GPU_MEMORY_CONTEXT_REPORT);
		for (index = 0; index < D3DTS_MAX; index++)
		{
			device.transforms[index]._11 = 1.0f;
			device.transforms[index]._22 = 1.0f;
			device.transforms[index]._33 = 1.0f;
			device.transforms[index]._44 = 1.0f;
		}
		device.applied_index_base = 0xffffffff;
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
