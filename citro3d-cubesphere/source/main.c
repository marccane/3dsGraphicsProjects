// citro3d VERTEX-SHADER showcase: a cube that morphs into a sphere and back.
//
// A subdivided cube (each face an 8x8 grid) is fed to a vertex shader that
// blends every vertex between its cube position and its normalized (spherified)
// position by an animated uniform t. At t=0 it's a faceted cube; at t=1 a smooth
// sphere. The whole thing tumbles in 3D. Press START to quit.
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>
#include "vshader_shbin.h"

#define CLEAR_COLOR 0x0A0A14FF

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float position[3]; float color[4]; } vertex;

#define SUBDIV   8                              // cells per cube-face edge
#define HALF     0.85f                          // cube half-extent
#define MAX_VTX  (6 * SUBDIV * SUBDIV * 6)       // 6 faces * cells * 6 verts/cell

static vertex vertex_list[MAX_VTX];
static int    vtx_count = 0;

static DVLB_s* vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection, uLoc_params;
static C3D_Mtx projection;
static void* vbo_data;

// Place (u,v) in [-1,1] onto cube face `axis` at `sign`, scaled by HALF.
static void facePoint(int axis, float sign, float u, float v, float out[3])
{
	float a = u * HALF, b = v * HALF, s = sign * HALF;
	switch (axis) {
		case 0: out[0]=s; out[1]=a; out[2]=b; break; // +/-X
		case 1: out[0]=a; out[1]=s; out[2]=b; break; // +/-Y
		default:out[0]=a; out[1]=b; out[2]=s; break; // +/-Z
	}
}

static void push(const float p[3])
{
	float inv = 1.0f / sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
	// colour from direction (stays painted on the surface through the morph)
	vertex_list[vtx_count++] = (vertex){
		{ p[0], p[1], p[2] },
		{ p[0]*inv*0.5f+0.5f, p[1]*inv*0.5f+0.5f, p[2]*inv*0.5f+0.5f, 1.0f }
	};
}

static void buildCube(void)
{
	const float faces[6][2] = { {0,+1},{0,-1},{1,+1},{1,-1},{2,+1},{2,-1} };
	for (int fi = 0; fi < 6; ++fi) {
		int axis = (int)faces[fi][0];
		float sign = faces[fi][1];
		for (int j = 0; j < SUBDIV; ++j) {
			for (int i = 0; i < SUBDIV; ++i) {
				float u0 = -1.0f + 2.0f*i/SUBDIV,     u1 = -1.0f + 2.0f*(i+1)/SUBDIV;
				float v0 = -1.0f + 2.0f*j/SUBDIV,     v1 = -1.0f + 2.0f*(j+1)/SUBDIV;
				float p00[3], p10[3], p11[3], p01[3];
				facePoint(axis, sign, u0, v0, p00);
				facePoint(axis, sign, u1, v0, p10);
				facePoint(axis, sign, u1, v1, p11);
				facePoint(axis, sign, u0, v1, p01);
				push(p00); push(p10); push(p11);
				push(p00); push(p11); push(p01);
			}
		}
	}
}

static void sceneInit(void)
{
	vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
	C3D_BindProgram(&program);

	uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
	uLoc_params     = shaderInstanceGetUniformLocation(program.vertexShader, "params");

	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0 = position
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 4); // v1 = colour

	buildCube();
	vbo_data = linearAlloc(sizeof(vertex) * vtx_count);
	memcpy(vbo_data, vertex_list, sizeof(vertex) * vtx_count);

	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo_data, sizeof(vertex), 2, 0x10);

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL); // PICA convention: depth clears to 0, nearer = greater
	C3D_CullFace(GPU_CULL_NONE);
}

static void sceneRender(float ax, float ay, float morph)
{
	C3D_Mtx modelView;
	Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(45.0f), C3D_AspectRatioTop, 0.01f, 1000.0f, false);
	Mtx_Identity(&modelView);
	Mtx_Translate(&modelView, 0.0f, 0.0f, -4.0f, true);
	Mtx_RotateX(&modelView, ax, true);
	Mtx_RotateY(&modelView, ay, true);

	C3D_Mtx mvp;
	Mtx_Multiply(&mvp, &projection, &modelView);

	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &mvp);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_params, morph, 0.0f, 0.0f, 0.0f);

	C3D_DrawArrays(GPU_TRIANGLES, 0, vtx_count);
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

	float ax = 0.0f, ay = 0.0f, tt = 0.0f;
	while (aptMainLoop())
	{
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;

		ax += 0.009f;
		ay += 0.013f;
		tt += 0.022f;
		float morph = 0.5f + 0.5f * sinf(tt); // cube (0) <-> sphere (1)

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(target);
			sceneRender(ax, ay, morph);
		C3D_FrameEnd(0);
	}

	sceneExit();
	C3D_Fini();
	gfxExit();
	return 0;
}
