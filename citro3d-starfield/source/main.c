// citro3d GEOMETRY-SHADER showcase #4: GPU PARTICLE STARFIELD.
//
// A cloud of stars is uploaded as a POINT cloud (one vertex per star). The
// geometry shader turns every point into a camera-facing quad — emitted as a
// 4-triangle fan around a bright centre vertex (centre opaque, rim alpha 0) —
// so with additive blending each star reads as a soft round glow and clusters
// bloom. 1 point -> 4 triangles is pure GS amplification: the PICA200 has no
// point-sprite hardware and no fragment shader, so the geometry shader *is* the
// particle system. The whole field slowly rotates (auto-spin + Circle Pad
// steering); near stars are larger (perspective), so the spin reads as real 3D
// depth and the far side shows through (additive, no depth write) like a globe
// of stars.
//
// Controls (bottom screen):
//   Circle Pad = steer spin   D-pad U/D = star size   A = toggle auto-spin
//   START = quit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "program_shbin.h"

#define CLEAR_COLOR 0x03030CFF   // near-black deep-space blue (0xRRGGBBAA)

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// vertex: position.xyz + size (w), then colour.rgb + brightness (a)
typedef struct { float pos[4]; float col[4]; } vertex;

#define N_STARS    768
#define FIELD_R    6.0f     // radius of the star ball
#define FIELD_DIST 8.5f     // pushed this far in front of the camera

static vertex star_list[N_STARS];

static DVLB_s* program_dvlb;
static shaderProgram_s program;
static int uLoc_projection, uLoc_right, uLoc_up, uLoc_params;
static C3D_Mtx projection;
static void* vbo_data;

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

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

static void buildStarfield(void)
{
	srand((u32)svcGetSystemTick());
	for (int i = 0; i < N_STARS; ++i) {
		// uniform point inside a ball (rejection sampling)
		float x, y, z;
		do { x = 2*frand()-1; y = 2*frand()-1; z = 2*frand()-1; } while (x*x + y*y + z*z > 1.0f);
		x *= FIELD_R; y *= FIELD_R; z *= FIELD_R;

		// star colour: mostly cool blue-white, some warm; value kept high
		float r, g, b;
		if (frand() < 0.72f)
			hsv(0.55f + 0.10f*frand(), 0.10f + 0.35f*frand(), 0.85f + 0.15f*frand(), &r, &g, &b); // blue-white
		else
			hsv(0.03f + 0.10f*frand(), 0.35f + 0.35f*frand(), 0.90f + 0.10f*frand(), &r, &g, &b); // warm

		float bright = 0.5f + 0.5f*frand();
		float size   = 0.6f + 0.9f*frand();
		if (frand() < 0.05f) { size *= 1.8f; bright = 1.0f; } // a few prominent stars

		star_list[i] = (vertex){ { x, y, z, size }, { r, g, b, bright } };
	}
}

static void sceneInit(void)
{
	program_dvlb = DVLB_ParseFile((u32*)program_shbin, program_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &program_dvlb->DVLE[0]);
	shaderProgramSetGsh(&program, &program_dvlb->DVLE[1], 2); // stride 2 = one POINT per GS invocation
	C3D_BindProgram(&program);

	uLoc_projection = shaderInstanceGetUniformLocation(program.geometryShader, "projection");
	uLoc_right      = shaderInstanceGetUniformLocation(program.geometryShader, "billright");
	uLoc_up         = shaderInstanceGetUniformLocation(program.geometryShader, "billup");
	uLoc_params     = shaderInstanceGetUniformLocation(program.geometryShader, "params");

	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 4); // v0 = pos.xyz + size
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 4); // v1 = colour.rgb + brightness

	buildStarfield();
	vbo_data = linearAlloc(sizeof(star_list));
	memcpy(vbo_data, star_list, sizeof(star_list));

	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo_data, sizeof(vertex), 2, 0x10);

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	// Additive glow: out = dst + src.rgb * src.a -> bright over a dark sky, overlaps bloom.
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
	C3D_DepthTest(false, GPU_GREATER, GPU_WRITE_ALL); // particles: no depth test, everything blends
	C3D_CullFace(GPU_CULL_NONE);                      // billboards aren't consistently wound
}

static void sceneRender(float ax, float ay, float sizeBase, float pulse)
{
	C3D_Mtx modelView;
	Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(55.0f), C3D_AspectRatioTop, 0.05f, 1000.0f, false);
	Mtx_Identity(&modelView);
	Mtx_Translate(&modelView, 0.0f, 0.0f, -FIELD_DIST, true);
	Mtx_RotateX(&modelView, ax, true);
	Mtx_RotateY(&modelView, ay, true);

	C3D_Mtx mvp;
	Mtx_Multiply(&mvp, &projection, &modelView);
	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, uLoc_projection, &mvp);

	// Camera basis expressed in MODEL space = inverse of the model rotation
	// (R^T = RotY(-ay)*RotX(-ax)), pre-scaled by the global star size so the
	// billboards face the camera no matter how the field spins.
	C3D_Mtx rt;
	Mtx_Identity(&rt);
	Mtx_RotateY(&rt, -ay, true);
	Mtx_RotateX(&rt, -ax, true);
	C3D_FVec right = Mtx_MultiplyFVec4(&rt, FVec4_New(1.0f, 0.0f, 0.0f, 0.0f));
	C3D_FVec up    = Mtx_MultiplyFVec4(&rt, FVec4_New(0.0f, 1.0f, 0.0f, 0.0f));
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, uLoc_right, right.x*sizeBase, right.y*sizeBase, right.z*sizeBase, 0.0f);
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, uLoc_up,    up.x*sizeBase,    up.y*sizeBase,    up.z*sizeBase,    0.0f);
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, uLoc_params, pulse, 0.0f, 0.0f, 0.0f);

	C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, N_STARS);
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
	consoleInit(GFX_BOTTOM, NULL);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

	C3D_RenderTarget* target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	sceneInit();

	printf("\x1b[2;2H=== GPU Particle Starfield ===");
	printf("\x1b[3;6H%d stars, geo-shader glow", N_STARS);
	printf("\x1b[6;2HControls:");
	printf("\x1b[7;4HCircle Pad  steer spin");
	printf("\x1b[8;4HD-pad U/D   star size");
	printf("\x1b[9;4HA           toggle auto-spin");
	printf("\x1b[10;4HSTART       quit");

	float ax = 0.30f, ay = 0.0f, tt = 0.0f;
	float sizeBase = 0.07f;     // global billboard scale (D-pad U/D)
	bool  autospin = true;

	u64 lastT = osGetTime();
	int  frames = 0;
	float fps = 0.0f;

	while (aptMainLoop())
	{
		hidScanInput();
		u32 kDown = hidKeysDown();
		u32 kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;
		if (kDown & KEY_A) autospin = !autospin;
		if (kHeld & KEY_DUP)   sizeBase += 0.004f;
		if (kHeld & KEY_DDOWN) sizeBase -= 0.004f;
		if (sizeBase < 0.020f) sizeBase = 0.020f;
		if (sizeBase > 0.200f) sizeBase = 0.200f;

		circlePosition cp;
		hidCircleRead(&cp);                  // dx,dy in ~[-156,156]
		float steerX = cp.dy * -0.00006f;    // push up -> tilt up
		float steerY = cp.dx *  0.00006f;

		if (autospin) { ax += 0.0035f; ay += 0.0075f; }
		ax += steerX;
		ay += steerY;
		tt += 0.03f;
		float pulse = 0.85f + 0.15f * sinf(tt); // gentle global breathing

		frames++;
		u64 now = osGetTime();
		if (now - lastT >= 500) { fps = frames * 1000.0f / (float)(now - lastT); frames = 0; lastT = now; }

		printf("\x1b[13;2HSize : %.3f   ", sizeBase);
		printf("\x1b[14;2HSpin : %-4s   ", autospin ? "ON" : "OFF");
		printf("\x1b[30;1HFPS: %.1f  ", fps);

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(target);
			sceneRender(ax, ay, sizeBase, pulse);
		C3D_FrameEnd(0);
	}

	sceneExit();
	C3D_Fini();
	gfxExit();
	return 0;
}
