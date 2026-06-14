// citro3d GEOMETRY-SHADER showcase: a spinning, breathing rainbow pinwheel.
//
// The VBO holds a ring of plain triangles ("spikes") centered on the origin.
// The geometry shader (program.g.pica) processes one whole triangle at a time:
// it computes the triangle's centroid — needs all 3 vertices, so a vertex shader
// can't do it — and explodes each spike radially outward by an animated uniform.
// The CPU spins the whole pinwheel and pulses the explode amount. Press START to quit.
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>
#include "program_shbin.h"

#define CLEAR_COLOR 0x0A0A14FF // near-black background for contrast

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float position[3]; float color[4]; } vertex;

#define N_SPIKES   14
#define R_IN       24.0f
#define R_OUT      70.0f
#define SPREAD     0.26f          // half-angular width of a spike (radians)
#define VTX_COUNT  (N_SPIKES * 3)

static vertex vertex_list[VTX_COUNT];

static DVLB_s* program_dvlb;
static shaderProgram_s program;
static int uLoc_projection, uLoc_params;
static C3D_Mtx projection;
static void* vbo_data;

// Simple HSV->RGB (h,s,v in [0,1]).
static void hsv(float h, float s, float v, float* r, float* g, float* b)
{
	float i = floorf(h * 6.0f);
	float f = h * 6.0f - i;
	float p = v * (1.0f - s);
	float q = v * (1.0f - f * s);
	float t = v * (1.0f - (1.0f - f) * s);
	switch (((int)i) % 6) {
		case 0: *r = v; *g = t; *b = p; break;
		case 1: *r = q; *g = v; *b = p; break;
		case 2: *r = p; *g = v; *b = t; break;
		case 3: *r = p; *g = q; *b = v; break;
		case 4: *r = t; *g = p; *b = v; break;
		default:*r = v; *g = p; *b = q; break;
	}
}

static void buildPinwheel(void)
{
	for (int i = 0; i < N_SPIKES; ++i) {
		float a = (2.0f * M_PI * i) / N_SPIKES;
		float r, g, b;
		hsv((float)i / N_SPIKES, 0.9f, 1.0f, &r, &g, &b);

		vertex* v = &vertex_list[i * 3];
		// inner apex
		v[0] = (vertex){ { R_IN  * cosf(a),          R_IN  * sinf(a),          0.5f }, { r, g, b, 1.0f } };
		// outer-left
		v[1] = (vertex){ { R_OUT * cosf(a - SPREAD), R_OUT * sinf(a - SPREAD), 0.5f }, { r, g, b, 1.0f } };
		// outer-right
		v[2] = (vertex){ { R_OUT * cosf(a + SPREAD), R_OUT * sinf(a + SPREAD), 0.5f }, { r, g, b, 1.0f } };
	}
}

static void sceneInit(void)
{
	// Load vertex + geometry shaders. The geoshader stride is 6 => it processes
	// one triangle (3 verts x 2 attributes) per invocation.
	program_dvlb = DVLB_ParseFile((u32*)program_shbin, program_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &program_dvlb->DVLE[0]);
	shaderProgramSetGsh(&program, &program_dvlb->DVLE[1], 6);
	C3D_BindProgram(&program);

	uLoc_projection = shaderInstanceGetUniformLocation(program.geometryShader, "projection");
	uLoc_params     = shaderInstanceGetUniformLocation(program.geometryShader, "params");

	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0 = position
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 4); // v1 = colour

	buildPinwheel();
	vbo_data = linearAlloc(sizeof(vertex_list));
	memcpy(vbo_data, vertex_list, sizeof(vertex_list));

	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo_data, sizeof(vertex), 2, 0x10);

	// Pass interpolated vertex colour straight through the fragment stage.
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	// Flat 2D: no depth test, no culling (spikes may wind either way as they spin).
	C3D_DepthTest(false, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_CullFace(GPU_CULL_NONE);
}

static void sceneRender(float angle, float bloom)
{
	// MVP = OrthoTilt * Translate(screen centre) * RotateZ(angle).
	C3D_Mtx model;
	Mtx_OrthoTilt(&projection, 0.0f, 400.0f, 0.0f, 240.0f, 0.0f, 1.0f, true);
	Mtx_Identity(&model);
	Mtx_Translate(&model, 200.0f, 120.0f, 0.0f, true);
	Mtx_RotateZ(&model, angle, true);

	C3D_Mtx mvp;
	Mtx_Multiply(&mvp, &projection, &model);

	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, uLoc_projection, &mvp);
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, uLoc_params, bloom, 0.0f, 0.0f, 0.0f);

	// GPU_GEOMETRY_PRIM hands raw vertices to the geoshader for primitive emission.
	C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, VTX_COUNT);
}

static void sceneExit(void)
{
	linearFree(vbo_data);
	shaderProgramFree(&program);
	DVLB_Free(program_dvlb);
}

int main()
{
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

	C3D_RenderTarget* target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	sceneInit();

	float angle = 0.0f, t = 0.0f;
	while (aptMainLoop())
	{
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;

		angle += 0.010f;            // continuous spin
		t     += 0.030f;
		float bloom = 0.30f + 0.30f * sinf(t); // breathe: explode amount in [0, 0.6]

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(target);
			sceneRender(angle, bloom);
		C3D_FrameEnd(0);
	}

	sceneExit();
	C3D_Fini();
	gfxExit();
	return 0;
}
