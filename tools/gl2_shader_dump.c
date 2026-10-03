/*
GL2_SHADER_DUMP.C

A console program that runs the NV2A shader translators (port/linux/src/
nv2a_vsh.c and nv2a_psh.c) over the game's 67 vertex programs and a set of
pixel shader keys, and writes the GLSL of each to a folder. It uses no
OpenGL: the output is for reading, for comparing the translators before and
after a change, and for tools/gl2_shader_check.py, which checks that the
OpenGL 2.0 renderer's shaders use nothing newer than GLSL 1.10.

	gl2_shader_dump <folder> [gl4|gl2|gl2-window|gl4-without]

gl4 (the default) is the OpenGL 4.5 renderer's dialect, gl2 the OpenGL 2.0
renderer's, gl2-window the same drawing into the window's own back buffer,
and gl4-without the 4.5 dialect with everything debug.gl_disable can take
from the shaders taken (what the fallbacks write on a 4.5 context).
Each vertex program is written twice: vsNN_d.glsl for the streams of its
declaration, vsNN_i.glsl for immediate mode (all floats).

Built by hand: this file, nv2a_vsh.c, nv2a_psh.c and xgpu_text.c compiled
with the flags that "ninja -t commands build/windows/halo.exe" shows for
them (without -flto), and the four objects linked. GL2_SHADER_DUMP_BEFORE
builds it against the translators as they were before the OpenGL 2.0
dialect existed.
*/

#include "xgpu.h"
#include "port_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- what the translators ask the rest of the port for */

#ifndef GL2_SHADER_DUMP_BEFORE
struct xgpu_capabilities xgpu_capabilities;
#endif

const char *config_string(const char *name)
{
	(void)name;
	return "";
}

int config_boolean(const char *name)
{
	(void)name;
	return 0;
}

/* ---------- the game's vertex programs */

static const DWORD vertex_shader_code[] =
{
#include "../source/rasterizer/xbox/rasterizer_xbox_vertex_shaders_data.inc"
};

#define NUMBER_OF_VERTEX_SHADERS 67

/* source/rasterizer/xbox/rasterizer_xbox_vertex_shaders_initialize.c */
static const DWORD vertex_shader_declarations[] =
{
	0x20000000, 0x40320000, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40210004, 0xFFFFFFFF,
	0x20000000, 0x40340000, 0x40340009, 0x40150008, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40220004, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40220004, 0x40400009, 0x20000001, 0x4024000B, 0xFFFFFFFF,
	0x20000000, 0x40220000, 0x40220004, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40210004, 0x40240005, 0x40110006, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40220004, 0x20000001, 0x40160007, 0x40210008, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40220004, 0xFFFFFFFF,
};

/* each program's declaration, as a byte offset into the table above */
static const unsigned char vertex_shader_declaration_offsets[NUMBER_OF_VERTEX_SHADERS] =
{
	0x00, 0x10, 0x34, 0x64, 0x64, 0x78, 0xC4, 0xC4, 0xC4, 0x78, 0x78, 0x20,
	0x34, 0x78, 0x78, 0x78, 0x9C, 0x78, 0x78, 0xC4, 0xC4, 0xC4, 0xC4, 0xC4,
	0xC4, 0x9C, 0xC4, 0x78, 0xC4, 0xC4, 0x78, 0x78, 0x78, 0x20, 0xC4, 0xC4,
	0x78, 0xC4, 0x64, 0x78, 0xC4, 0xC4, 0xC4, 0xC4, 0x9C, 0x78, 0xC4, 0x78,
	0xC4, 0xC4, 0x78, 0xC4, 0xC4, 0xC4, 0xC4, 0xC4, 0x34, 0x78, 0x9C, 0x9C,
	0x78, 0xC4, 0x78, 0x78, 0x78, 0x34, 0x48,
};

/* the registers a declaration feeds as NORMPACKED3 and as D3DCOLOR (as
parse_declaration in d3d8_gl.c finds them) */
static void declaration_masks(const DWORD *declaration, unsigned long *packed_mask, unsigned long *color_mask)
{
	*packed_mask = *color_mask = 0;
	for (; *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;

		if ((token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT == D3DVSD_TOKEN_STREAMDATA &&
			!(token & D3DVSD_DATALOADTYPEMASK))
		{
			unsigned long type = (token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT;

			if (type == D3DVSDT_NORMPACKED3)
				*packed_mask |= 1UL << (token & D3DVSD_VERTEXREGMASK);
			if (type == D3DVSDT_D3DCOLOR)
				*color_mask |= 1UL << (token & D3DVSD_VERTEXREGMASK);
		}
	}
}

/* ---------- output */

static const char *output_folder;
static unsigned long files_written;

static void write_shader(const char *name, char *text)
{
	char path[1024];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s.glsl", output_folder, name);
	file = fopen(path, "wb");
	if (!file)
	{
		fprintf(stderr, "cannot write %s\n", path);
		exit(1);
	}
	fputs(text, file);
	fclose(file);
	free(text);
	files_written++;
}

static char *translate_vertex(const DWORD *instructions, unsigned long count, unsigned long packed_mask,
	unsigned long color_mask)
{
#ifdef GL2_SHADER_DUMP_BEFORE
	(void)color_mask;
	return nv2a_vertex_shader_to_glsl(instructions, count, packed_mask);
#else
	/* as the device asks (vertex_shader_get in d3d8_gl.c): with GL_BGRA
	attributes the shader has no colours to put in order */
	return nv2a_vertex_shader_to_glsl(instructions, count, packed_mask,
		xgpu_capabilities.bgra_attributes ? 0 : color_mask);
#endif
}

static void dump_vertex_shaders(void)
{
	unsigned long word = 0, total = sizeof(vertex_shader_code) / sizeof(vertex_shader_code[0]);
	int index;

	for (index = 0; index < NUMBER_OF_VERTEX_SHADERS && word < total; index++)
	{
		/* header: program type in the low word, instruction count in the high */
		unsigned long count = vertex_shader_code[word] >> 16;
		const DWORD *instructions = &vertex_shader_code[word + 1];
		unsigned long packed_mask, color_mask;
		char name[32];

		declaration_masks(&vertex_shader_declarations[vertex_shader_declaration_offsets[index] / sizeof(DWORD)],
			&packed_mask, &color_mask);
		snprintf(name, sizeof(name), "vs%02d_d", index);
		write_shader(name, translate_vertex(instructions, count, packed_mask, color_mask));
		snprintf(name, sizeof(name), "vs%02d_i", index);
		write_shader(name, translate_vertex(instructions, count, 0, 0));
		word += 1 + count * 4;
		/* a zero word follows each program */
		if (word < total && !vertex_shader_code[word])
			word++;
	}
	if (index != NUMBER_OF_VERTEX_SHADERS || word != total)
	{
		fprintf(stderr, "the vertex program table does not hold %d programs\n", NUMBER_OF_VERTEX_SHADERS);
		exit(1);
	}
}

/* ---------- pixel shader keys */

#define IN(a, b, c, d) (((DWORD)(a) << 24) | ((DWORD)(b) << 16) | ((DWORD)(c) << 8) | (DWORD)(d))
#define OUT(ab, cd, sum, flags) (((DWORD)(flags) << 12) | ((DWORD)(sum) << 8) | ((DWORD)(ab) << 4) | (DWORD)(cd))
#define MODES(t0, t1, t2, t3) (((DWORD)(t3) << 15) | ((DWORD)(t2) << 10) | ((DWORD)(t1) << 5) | (DWORD)(t0))

static struct nv2a_pixel_shader_key key;
static DWORD *s;

static void reset(DWORD modes, int t0, int t1, int t2, int t3)
{
	memset(&key, 0, sizeof(key));
	s = key.combiner_state;
	key.texture_modes = modes;
	key.sampler_type[0] = (unsigned char)t0;
	key.sampler_type[1] = (unsigned char)t1;
	key.sampler_type[2] = (unsigned char)t2;
	key.sampler_type[3] = (unsigned char)t3;
}

static void emit(const char *name)
{
	char file[64];

	snprintf(file, sizeof(file), "ps_%s", name);
	write_shader(file, nv2a_pixel_shader_to_glsl(&key));
}

/* _rasterizer_environment_specular_lights_begin (rasterizer_xbox_environment.c):
6 general combiners */
static void specular_light(void)
{
	reset(0x00018C41, _xgpu_sampler_2d, _xgpu_sampler_3d, _xgpu_sampler_cube, _xgpu_sampler_cube);
	s[D3DRS_PSCOMBINERCOUNT] = 0x00011006;
	s[D3DRS_PSALPHAINPUTS0 + 0] = 0x4B204B20; s[D3DRS_PSALPHAOUTPUTS0 + 0] = 0x00020400;
	s[D3DRS_PSRGBINPUTS0 + 0] = 0x484A0000; s[D3DRS_PSRGBOUTPUTS0 + 0] = 0x000020C0;
	s[D3DRS_PSALPHAINPUTS0 + 1] = 0x4A204A20; s[D3DRS_PSALPHAOUTPUTS0 + 1] = 0x00020500;
	s[D3DRS_PSRGBINPUTS0 + 1] = 0x48CC8A40; s[D3DRS_PSRGBOUTPUTS0 + 1] = 0x00010D00;
	s[D3DRS_PSALPHAINPUTS0 + 2] = 0x2C120C11; s[D3DRS_PSALPHAOUTPUTS0 + 2] = 0x00000C00;
	s[D3DRS_PSRGBINPUTS0 + 2] = 0xCD4B0809; s[D3DRS_PSRGBOUTPUTS0 + 2] = 0x000020D0;
	s[D3DRS_PSALPHAINPUTS0 + 3] = 0x0D0D1415; s[D3DRS_PSALPHAOUTPUTS0 + 3] = 0x000000D5;
	s[D3DRS_PSRGBINPUTS0 + 3] = 0x2C020C01; s[D3DRS_PSRGBOUTPUTS0 + 3] = 0x00000C00;
	s[D3DRS_PSALPHAINPUTS0 + 4] = 0x1D1D151C; s[D3DRS_PSALPHAOUTPUTS0 + 4] = 0x000000D5;
	s[D3DRS_PSRGBINPUTS0 + 4] = 0x0C091C09; s[D3DRS_PSRGBOUTPUTS0 + 4] = 0x000110CD;
	s[D3DRS_PSALPHAINPUTS0 + 5] = 0x1D1D0000; s[D3DRS_PSALPHAOUTPUTS0 + 5] = 0x000000D0;
	s[D3DRS_PSRGBINPUTS0 + 5] = 0x0C150D1D; s[D3DRS_PSRGBOUTPUTS0 + 5] = 0x000010CD;
	s[D3DRS_PSFINALCOMBINERINPUTSABCD] = 0x0C0F0000;
	s[D3DRS_PSFINALCOMBINERINPUTSEFG] = 0x1D200D00;
}

static void dump_pixel_shaders(void)
{
	/* ----- the four heaviest of the game's (the environment's) */

	specular_light();
	emit("specular_light_6");

	/* the same with the extra shiny flag: 8 general combiners */
	specular_light();
	s[D3DRS_PSRGBOUTPUTS0 + 4] = 0x000210CD;
	s[D3DRS_PSCOMBINERCOUNT] = 0x00011008;
	s[D3DRS_PSALPHAINPUTS0 + 6] = 0x1D1D0000; s[D3DRS_PSALPHAOUTPUTS0 + 6] = 0x000000D0;
	s[D3DRS_PSALPHAINPUTS0 + 7] = 0x1D1D0000; s[D3DRS_PSALPHAOUTPUTS0 + 7] = 0x000000D0;
	emit("specular_light_8");

	/* _rasterizer_environment_specular_lightmaps_begin: 6 general combiners */
	reset(0x00018C21, _xgpu_sampler_2d, _xgpu_sampler_2d, _xgpu_sampler_cube, _xgpu_sampler_cube);
	s[D3DRS_PSCOMBINERCOUNT] = 0x00011006;
	s[D3DRS_PSALPHAINPUTS0 + 0] = 0x4B204B20; s[D3DRS_PSALPHAOUTPUTS0 + 0] = 0x00020400;
	s[D3DRS_PSRGBINPUTS0 + 0] = 0x484A0911; s[D3DRS_PSRGBOUTPUTS0 + 0] = 0x000030C9;
	s[D3DRS_PSALPHAINPUTS0 + 1] = 0x4A204A20; s[D3DRS_PSALPHAOUTPUTS0 + 1] = 0x00020500;
	s[D3DRS_PSRGBINPUTS0 + 1] = 0x48CC8A40; s[D3DRS_PSRGBOUTPUTS0 + 1] = 0x00010D00;
	s[D3DRS_PSALPHAINPUTS0 + 2] = 0x2C120C11; s[D3DRS_PSALPHAOUTPUTS0 + 2] = 0x00000C00;
	s[D3DRS_PSRGBINPUTS0 + 2] = 0xCD4B0809; s[D3DRS_PSRGBOUTPUTS0 + 2] = 0x000020D0;
	s[D3DRS_PSALPHAINPUTS0 + 3] = 0x0D0D1415; s[D3DRS_PSALPHAOUTPUTS0 + 3] = 0x000000D5;
	s[D3DRS_PSRGBINPUTS0 + 3] = 0x2C020C01; s[D3DRS_PSRGBOUTPUTS0 + 3] = 0x00000C00;
	s[D3DRS_PSALPHAINPUTS0 + 4] = 0x1D1D151C; s[D3DRS_PSALPHAOUTPUTS0 + 4] = 0x000000D5;
	s[D3DRS_PSRGBINPUTS0 + 4] = 0x0C091C09; s[D3DRS_PSRGBOUTPUTS0 + 4] = 0x000110CD;
	s[D3DRS_PSALPHAINPUTS0 + 5] = 0x1D1D0000; s[D3DRS_PSALPHAOUTPUTS0 + 5] = 0x000000D0;
	s[D3DRS_PSRGBINPUTS0 + 5] = 0x0C150D1D; s[D3DRS_PSRGBOUTPUTS0 + 5] = 0x000010CD;
	s[D3DRS_PSFINALCOMBINERINPUTSABCD] = 0x0C0F0000;
	s[D3DRS_PSFINALCOMBINERINPUTSEFG] = 0x1D110D00;
	emit("specular_lightmap_6");

	/* _rasterizer_environment_reflection: a bumped cube map, 5 combiners,
	dot product stages with the diffuse and specular cubes */
	reset(MODES(1, 17, 11, 12), _xgpu_sampler_2d, _xgpu_sampler_2d, _xgpu_sampler_cube, _xgpu_sampler_cube);
	s[D3DRS_PSDOTMAPPING] = (1 << 8) | (1 << 4) | 0;
	s[D3DRS_PSCOMBINERCOUNT] = 5 | (0x110 << 8);
	s[D3DRS_PSRGBINPUTS0 + 0] = IN(10 | 64, 1 | 64, 11, 11);
	s[D3DRS_PSRGBINPUTS0 + 1] = IN(12, 12, 13, 13);
	s[D3DRS_PSRGBINPUTS0 + 2] = IN(12, 12, 13, 13);
	s[D3DRS_PSRGBOUTPUTS0 + 0] = OUT(12, 13, 0, 2);
	s[D3DRS_PSRGBOUTPUTS0 + 1] = OUT(12, 13, 0, 0);
	s[D3DRS_PSRGBOUTPUTS0 + 2] = OUT(0, 13, 0, 0);
	s[D3DRS_PSALPHAINPUTS0 + 3] = IN(12 | 32, 2 | 16, 12, 1 | 16);
	s[D3DRS_PSALPHAOUTPUTS0 + 3] = OUT(0, 0, 12, 0);
	s[D3DRS_PSRGBINPUTS0 + 3] = IN(12 | 32, 2, 12, 1);
	s[D3DRS_PSRGBOUTPUTS0 + 3] = OUT(0, 0, 12, 0);
	s[D3DRS_PSRGBINPUTS0 + 4] = IN(12 | 32, 13, 12, 11);
	s[D3DRS_PSRGBOUTPUTS0 + 4] = OUT(0, 0, 12, 0);
	s[D3DRS_PSFINALCOMBINERINPUTSABCD] = IN(12, 15, 0, 0);
	s[D3DRS_PSFINALCOMBINERINPUTSEFG] = IN(12 | 16, 8, 0, 0);
	emit("bumped_reflection_5");

	/* ----- simple ones */

	/* no combiners at all: the result is r0, t0's alpha */
	reset(MODES(1, 0, 0, 0), _xgpu_sampler_2d, 0, 0, 0);
	emit("texture_only");

	/* one stage, t0 * v0, with the alpha test and linear fog */
	reset(MODES(1, 0, 0, 0), _xgpu_sampler_2d, 0, 0, 0);
	s[D3DRS_PSCOMBINERCOUNT] = 1;
	s[D3DRS_PSRGBINPUTS0] = IN(8, 4, 0, 0); s[D3DRS_PSRGBOUTPUTS0] = OUT(12, 0, 0, 0);
	s[D3DRS_PSALPHAINPUTS0] = IN(8 | 16, 4 | 16, 0, 0); s[D3DRS_PSALPHAOUTPUTS0] = OUT(12, 0, 0, 0);
	s[D3DRS_PSFINALCOMBINERINPUTSABCD] = IN(3 | 16, 12, 3, 0);
	s[D3DRS_PSFINALCOMBINERINPUTSEFG] = IN(0, 0, 12 | 16, 0);
	key.alpha_test_function = D3DCMP_GREATEREQUAL;
	key.fog_enable = 1;
	key.fog_table_mode = D3DFOG_LINEAR;
	emit("modulate_fog_alpha_test");

	/* the sprites with depth (rasterizer_xbox_transparent_geometry.c): t0 and
	t1, then a dot product pair that replaces the pixel's depth */
	reset(0x00054421, _xgpu_sampler_2d, _xgpu_sampler_2d, 0, 0);
	s[D3DRS_PSINPUTTEXTURE] = 0x00110000;
	s[D3DRS_PSCOMBINERCOUNT] = 2;
	s[D3DRS_PSALPHAINPUTS0 + 0] = 0x18201415; s[D3DRS_PSALPHAOUTPUTS0 + 0] = 0x000000C4;
	s[D3DRS_PSRGBINPUTS0 + 0] = 0x08050000; s[D3DRS_PSRGBOUTPUTS0 + 0] = 0x000000C0;
	s[D3DRS_PSALPHAINPUTS0 + 1] = 0x1C190000; s[D3DRS_PSALPHAOUTPUTS0 + 1] = 0x000000C0;
	s[D3DRS_PSRGBINPUTS0 + 1] = 0x0C090000; s[D3DRS_PSRGBOUTPUTS0 + 1] = 0x000000C0;
	s[D3DRS_PSFINALCOMBINERINPUTSABCD] = 0x0000000C;
	s[D3DRS_PSFINALCOMBINERINPUTSEFG] = 0x00001C00;
	emit("depth_sprite");

	/* every sampler kind, bump environment mapping with luminance, a clip
	plane stage, signed channels, alpha kill, the least significant bit mux
	and per-stage constants */
	reset(MODES(1, 7, 3, 5), _xgpu_sampler_2d, _xgpu_sampler_2d, _xgpu_sampler_cube, 0);
	s[D3DRS_PSCOMPAREMODE] = 0x5000;
	s[D3DRS_PSCOMBINERCOUNT] = 2 | 0x11000;
	s[D3DRS_PSRGBINPUTS0 + 0] = IN(8, 1, 9, 2); s[D3DRS_PSRGBOUTPUTS0 + 0] = OUT(0, 0, 12, 4);
	s[D3DRS_PSALPHAINPUTS0 + 0] = IN(8 | 16, 1 | 16, 9 | 16, 2 | 16); s[D3DRS_PSALPHAOUTPUTS0 + 0] = OUT(0, 0, 12, 4);
	s[D3DRS_PSRGBINPUTS0 + 1] = IN(12, 10, 0, 0); s[D3DRS_PSRGBOUTPUTS0 + 1] = OUT(12, 0, 0, 0);
	key.color_sign[0] = 0x6;
	key.alpha_kill[1] = 1;
	key.fog_enable = 1;
	key.fog_table_mode = D3DFOG_EXP2;
	key.alpha_test_function = D3DCMP_NOTEQUAL;
	emit("bump_cube_clip_lsb_mux");

	/* a volume texture and dependent lookups */
	reset(MODES(2, 15, 16, 0), _xgpu_sampler_3d, _xgpu_sampler_2d, _xgpu_sampler_2d, 0);
	s[D3DRS_PSCOMBINERCOUNT] = 1 | 0x100;
	s[D3DRS_PSRGBINPUTS0] = IN(8, 9, 10, 4); s[D3DRS_PSRGBOUTPUTS0] = OUT(0, 0, 12, 4);
	s[D3DRS_PSALPHAINPUTS0] = IN(8 | 16, 9 | 16, 0, 0); s[D3DRS_PSALPHAOUTPUTS0] = OUT(12, 0, 0, 0);
	emit("volume_dependent_msb_mux");
}

int main(int argc, char **argv)
{
	const char *renderer = argc > 2 ? argv[2] : "gl4";

	if (argc < 2)
	{
		fprintf(stderr, "usage: gl2_shader_dump <folder> [gl4|gl2|gl2-window|gl4-without]\n");
		return 2;
	}
	output_folder = argv[1];
#ifdef GL2_SHADER_DUMP_BEFORE
	if (strcmp(renderer, "gl4"))
	{
		fprintf(stderr, "the translators of before have the gl4 dialect only\n");
		return 2;
	}
#else
	if (!strcmp(renderer, "gl4"))
	{
		xgpu_capabilities_set_renderer(&xgpu_capabilities, _xgpu_renderer_gl4);
	}
	else if (!strcmp(renderer, "gl4-without"))
	{
		xgpu_capabilities_set_renderer(&xgpu_capabilities, _xgpu_renderer_gl4);
		xgpu_capabilities.clip_control = FALSE;
		xgpu_capabilities.integer_attributes = FALSE;
		xgpu_capabilities.bgra_attributes = FALSE;
		xgpu_capabilities.sampler_objects = FALSE;
		xgpu_capabilities.depth_replace = TRUE;
		xgpu_capabilities.window_back_buffer = TRUE;
	}
	else if (!strcmp(renderer, "gl2") || !strcmp(renderer, "gl2-window"))
	{
		xgpu_capabilities_set_renderer(&xgpu_capabilities, _xgpu_renderer_gl2);
		xgpu_capabilities.window_back_buffer = !strcmp(renderer, "gl2-window");
	}
	else
	{
		fprintf(stderr, "unknown renderer %s\n", renderer);
		return 2;
	}
#endif
	dump_vertex_shaders();
	dump_pixel_shaders();
	printf("%lu shaders written to %s (%s)\n", files_written, output_folder, renderer);
	return 0;
}
