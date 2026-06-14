// citro3d GEOMETRY-SHADER showcase #2: a 3D exploding icosahedron.
//
// A 20-face icosahedron tumbles in 3D. The geometry shader computes each face's
// normal (cross product of two edges — needs the whole triangle) and flies the
// face out along that normal by an animated `bloom`, so the solid blows apart
// into 20 rigid shards and reassembles. Press START to quit.
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>
#include "program_shbin.h"

#define CLEAR_COLOR 0x0A0A14FF

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float position[3]; float color[4]; } vertex;

#define N_FACES   20
#define VTX_COUNT (N_FACES * 3)

static vertex vertex_list[VTX_COUNT];

static DVLB_s* program_dvlb;
static shaderProgram_s program;
static int uLoc_projection, uLoc_params;
static C3D_Mtx projection;
static void* vbo_data;

static void hsv(float h, float s, float v, float* r, float* g, float* b)
{
	float i = floorf(h * 6.0f), f = h * 6.0f - i;
	float p = v * (1.0f - s), q = v * (1.0f - f * s), t = v * (1.0f - (1.0f - f) * s);
	switch (((int)i) % 6) {
		case 0: *r=v; *g=t; *b=p; break;  case 1: *r=q; *g=v; *b=p; break;
		case 2: *r=p; *g=v; *b=t; break;  case 3: *r=p; *g=q; *b=v; break;
		case 4: *r=t; *g=p; *b=v; break;  default:*r=v; *g=p; *b=q; break;
	}
}

static void buildIcosahedron(void)
{
	const float t = 1.61803398875f; // golden ratio
	const float V[12][3] = {
		{-1, t, 0}, { 1, t, 0}, {-1,-t, 0}, { 1,-t, 0},
		{ 0,-1, t}, { 0, 1, t}, { 0,-1,-t}, { 0, 1,-t},
		{ t, 0,-1}, { t, 0, 1}, {-t, 0,-1}, {-t, 0, 1},
	};
	const int F[20][3] = {
		{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
		{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
		{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
		{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1},
	};
	for (int f = 0; f < N_FACES; ++f) {
		float r, g, b;
		hsv((float)f / N_FACES, 0.85f, 1.0f, &r, &g, &b);
		for (int k = 0; k < 3; ++k) {
			const float* p = V[F[f][k]];
			float inv = 1.0f / sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]); // -> unit sphere
			vertex_list[f*3+k] = (vertex){ { p[0]*inv, p[1]*inv, p[2]*inv }, { r, g, b, 1.0f } };
		}
	}
}

static void sceneInit(void)
{
	program_dvlb = DVLB_ParseFile((u32*)program_shbin, program_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &program_dvlb->DVLE[0]);
	shaderProgramSetGsh(&program, &program_dvlb->DVLE[1], 6); // 6 = one triangle per invocation
	C3D_BindProgram(&program);

	uLoc_projection = shaderInstanceGetUniformLocation(program.geometryShader, "projection");
	uLoc_params     = shaderInstanceGetUniformLocation(program.geometryShader, "params");

	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0 = position
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 4); // v1 = colour

	buildIcosahedron();
	vbo_data = linearAlloc(sizeof(vertex_list));
	memcpy(vbo_data, vertex_list, sizeof(vertex_list));

	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo_data, sizeof(vertex), 2, 0x10);

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL); // PICA convention: depth clears to 0, nearer = greater
	C3D_CullFace(GPU_CULL_NONE);                  // shards expose their back faces
}

static void sceneRender(float ax, float ay, float bloom)
{
	C3D_Mtx modelView;
	Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(45.0f), C3D_AspectRatioTop, 0.01f, 1000.0f, false);
	Mtx_Identity(&modelView);
	Mtx_Translate(&modelView, 0.0f, 0.0f, -3.2f, true);
	Mtx_RotateX(&modelView, ax, true);
	Mtx_RotateY(&modelView, ay, true);

	C3D_Mtx mvp;
	Mtx_Multiply(&mvp, &projection, &modelView);

	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, uLoc_projection, &mvp);
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, uLoc_params, bloom, 0.0f, 0.0f, 0.0f);

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

	float ax = 0.0f, ay = 0.0f, tt = 0.0f;
	while (aptMainLoop())
	{
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;

		ax += 0.011f;
		ay += 0.017f;            // different rates -> tumbling
		tt += 0.028f;
		float bloom = 0.5f + 0.5f * sinf(tt); // closed (0) <-> blown apart (1)

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(target);
			sceneRender(ax, ay, bloom);
		C3D_FrameEnd(0);
	}

	sceneExit();
	C3D_Fini();
	gfxExit();
	return 0;
}
