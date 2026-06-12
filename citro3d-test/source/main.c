// Direct citro3d test: draws a per-vertex RGB triangle on the top screen using
// a PICA200 vertex shader (citro3d is NOT OpenGL — it requires a real shader).
// Modeled on devkitPro's simple_tri example, extended to per-vertex colours.
#include <3ds.h>
#include <citro3d.h>
#include <string.h>
#include "vshader_shbin.h"

#define CLEAR_COLOR 0x204060FF // dark blue background

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float x, y, z; float r, g, b; } vertex;

// Top screen is 400x240. One corner each: red, green, blue.
static const vertex vertex_list[] =
{
	{ 200.0f, 200.0f, 0.5f,  1.0f, 0.0f, 0.0f }, // top    - red
	{ 100.0f,  40.0f, 0.5f,  0.0f, 1.0f, 0.0f }, // left   - green
	{ 300.0f,  40.0f, 0.5f,  0.0f, 0.0f, 1.0f }, // right  - blue
};

#define vertex_list_count (sizeof(vertex_list)/sizeof(vertex_list[0]))

static DVLB_s* vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection;
static C3D_Mtx projection;
static void* vbo_data;

static void sceneInit(void)
{
	// Load the vertex shader, create a shader program and bind it
	vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
	C3D_BindProgram(&program);

	// Get the location of the projection uniform
	uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");

	// Configure attributes for use with the vertex shader
	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0 = position
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 3); // v1 = colour

	// Orthographic projection for the rotated top screen
	Mtx_OrthoTilt(&projection, 0.0, 400.0, 0.0, 240.0, 0.0, 1.0, true);

	// Create and fill the VBO
	vbo_data = linearAlloc(sizeof(vertex_list));
	memcpy(vbo_data, vertex_list, sizeof(vertex_list));

	// Configure buffers: stride = sizeof(vertex), 2 attributes (0 then 1)
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo_data, sizeof(vertex), 2, 0x10);

	// Fragment pipeline: just pass through the interpolated vertex colour
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

static void sceneRender(void)
{
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
	C3D_DrawArrays(GPU_TRIANGLES, 0, vertex_list_count);
}

static void sceneExit(void)
{
	linearFree(vbo_data);
	shaderProgramFree(&program);
	DVLB_Free(vshader_dvlb);
}

int main()
{
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

	C3D_RenderTarget* target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	sceneInit();

	while (aptMainLoop())
	{
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(target);
			sceneRender();
		C3D_FrameEnd(0);
	}

	sceneExit();
	C3D_Fini();
	gfxExit();
	return 0;
}
