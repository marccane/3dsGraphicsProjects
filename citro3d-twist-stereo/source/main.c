// citro3d VERTEX-SHADER showcase: a TWISTING COLUMN (stereoscopic).
//
// A square column subdivided up its height; each vertex is rotated about the Y
// axis by an angle proportional to its height (theta = k*y + wt), so the four
// coloured side faces wind into a spiral that animates over time. Same building
// block as the flag (citro3d-flag-stereo): the angle's spatial part k*y is
// constant per vertex, so the CPU precomputes sin(k*y)/cos(k*y) and the shader
// forms theta via the angle-addition identity with a per-frame (cos wt, sin wt)
// uniform - no in-shader sin, no range reduction. The face normals are rotated
// by the same theta and lit by a world-fixed (headlight) light, so the spiral
// catches light as it turns.
//
// Controls (bottom screen):
//   D-pad L/R = twist animation speed   A = pause   START = quit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vshader_shbin.h"

#define CLEAR_COLOR 0x0C0E16FF

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// position + twist phase (sin k*y, cos k*y) + base normal (nx,nz) + colour
typedef struct { float pos[3]; float tw[2]; float nrm[2]; float col[3]; } vertex;

#define NY    64                 // height subdivisions
#define HW    0.34f              // half-width of the square cross-section
#define HH    1.25f              // half-height
#define KTW   5.0f               // twist: radians of rotation per unit height (~2 turns over the column)
#define FACE_VERTS  ((NY+1) * 2)
#define VERTS  (4 * FACE_VERTS)
#define INDS   (4 * NY * 6)

static vertex col_v[VERTS];
static u16    col_i[INDS];

static DVLB_s* vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection, uLoc_timewave, uLoc_light;
static C3D_Mtx projection;
static void *vbo, *ibo;

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

static void buildColumn(void)
{
	// 4 side faces: each a vertical strip between two adjacent corners, with a
	// constant outward normal in the xz plane.
	const float corner[4][2] = { { HW, HW}, {-HW, HW}, {-HW,-HW}, { HW,-HW} };
	const float fnorm [4][2] = { {0, 1}, {-1, 0}, {0,-1}, {1, 0} };  // +z, -x, -z, +x
	const float fhue  [4]    = { 0.00f, 0.30f, 0.55f, 0.83f };       // 4 bold face colours

	int v = 0;
	for (int f = 0; f < 4; ++f) {
		const float* c0 = corner[f];
		const float* c1 = corner[(f+1) & 3];
		float r, g, b; hsv(fhue[f], 0.9f, 1.0f, &r, &g, &b);
		for (int j = 0; j <= NY; ++j) {
			float y  = -HH + 2.0f*HH * j / NY;
			float ky = KTW * y;
			float s = sinf(ky), c = cosf(ky);
			// two corners of this face at height y
			col_v[v++] = (vertex){ { c0[0], y, c0[1] }, { s, c }, { fnorm[f][0], fnorm[f][1] }, { r, g, b } };
			col_v[v++] = (vertex){ { c1[0], y, c1[1] }, { s, c }, { fnorm[f][0], fnorm[f][1] }, { r, g, b } };
		}
	}
	int n = 0;
	for (int f = 0; f < 4; ++f) {
		int base = f * FACE_VERTS;
		for (int j = 0; j < NY; ++j) {
			u16 a = base + 2*j, bb = base + 2*j+1, cc = base + 2*(j+1), d = base + 2*(j+1)+1;
			col_i[n++]=a; col_i[n++]=bb; col_i[n++]=d;
			col_i[n++]=a; col_i[n++]=d;  col_i[n++]=cc;
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
	uLoc_timewave   = shaderInstanceGetUniformLocation(program.vertexShader, "timewave");
	uLoc_light      = shaderInstanceGetUniformLocation(program.vertexShader, "lightdir");

	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0 = position
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2); // v1 = twist phase (sin k*y, cos k*y)
	AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 2); // v2 = base normal (nx, nz)
	AttrInfo_AddLoader(attrInfo, 3, GPU_FLOAT, 3); // v3 = colour

	buildColumn();
	vbo = linearAlloc(sizeof(col_v)); memcpy(vbo, col_v, sizeof(col_v));
	ibo = linearAlloc(sizeof(col_i)); memcpy(ibo, col_i, sizeof(col_i));

	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo, sizeof(vertex), 4, 0x3210);

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_CullFace(GPU_CULL_NONE);          // open-ended tube: show both sides
}

static void sceneRender(float orbit, float cosT, float sinT, float iod)
{
	C3D_Mtx modelView;
	Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(48.0f), C3D_AspectRatioTop,
	                    0.01f, 1000.0f, iod, 3.6f, false);
	Mtx_Identity(&modelView);
	Mtx_Translate(&modelView, 0.0f, 0.0f, -3.6f, true);
	Mtx_RotateX(&modelView, -0.12f, true);    // slight tilt
	Mtx_RotateY(&modelView, orbit, true);     // slow orbit so we see all sides

	C3D_Mtx mvp;
	Mtx_Multiply(&mvp, &projection, &modelView);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &mvp);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_timewave, cosT, sinT, 0.0f, 0.0f);

	// World-fixed light, expressed in MODEL space (headlight trick): R^T * worldLight,
	// where R is the model's orbit+tilt rotation. Keeps the lit side world-consistent.
	C3D_Mtx rt;
	Mtx_Identity(&rt);
	Mtx_RotateY(&rt, -orbit, true);
	Mtx_RotateX(&rt,  0.12f, true);
	C3D_FVec lm = Mtx_MultiplyFVec4(&rt, FVec4_New(0.45f, 0.40f, 0.80f, 0.0f));
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_light, lm.x, lm.y, lm.z, 0.0f);

	C3D_DrawElements(GPU_TRIANGLES, INDS, C3D_UNSIGNED_SHORT, ibo);
}

static void sceneExit(void)
{
	linearFree(vbo);
	linearFree(ibo);
	shaderProgramFree(&program);
	DVLB_Free(vshader_dvlb);
}

int main()
{
	gfxInitDefault();
	gfxSet3D(true);
	consoleInit(GFX_BOTTOM, NULL);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

	C3D_RenderTarget* targetLeft  = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(targetLeft,  GFX_TOP, GFX_LEFT,  DISPLAY_TRANSFER_FLAGS);
	C3D_RenderTarget* targetRight = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(targetRight, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);

	sceneInit();

	printf("\x1b[2;2H=== Twisting Column (stereo) ===");
	printf("\x1b[3;6Hvertex-shader twist");
	printf("\x1b[6;2HControls:");
	printf("\x1b[7;4HD-pad L/R  twist speed");
	printf("\x1b[8;4HA          pause");
	printf("\x1b[9;4H3D slider  pop-out depth");
	printf("\x1b[10;4HSTART      quit");

	float phase = 0.0f, orbit = 0.0f;
	float speed = 0.020f;        // twist animation speed (D-pad L/R)
	bool paused = false;

	u64 lastT = osGetTime();
	int frames = 0; float fps = 0.0f;

	while (aptMainLoop())
	{
		hidScanInput();
		u32 kDown = hidKeysDown();
		u32 kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;
		if (kDown & KEY_A) paused = !paused;
		if (kHeld & KEY_DRIGHT) speed += 0.003f;
		if (kHeld & KEY_DLEFT)  speed -= 0.003f;
		if (speed < -0.20f) speed = -0.20f;
		if (speed >  0.20f) speed =  0.20f;

		if (!paused) {
			phase += speed;
			if (phase >  2.0f*M_PI) phase -= 2.0f*M_PI;   // keep cos/sin args bounded
			if (phase < -2.0f*M_PI) phase += 2.0f*M_PI;
		}
		orbit += 0.006f;          // gentle continuous orbit (always, so it's never static)

		frames++;
		u64 now = osGetTime();
		if (now - lastT >= 500) { fps = frames * 1000.0f / (float)(now - lastT); frames = 0; lastT = now; }

		printf("\x1b[13;2HTwist speed : %+.3f ", speed);
		printf("\x1b[14;2HState       : %-7s ", paused ? "PAUSED" : "RUNNING");
		printf("\x1b[30;1HFPS: %.1f  ", fps);

		float slider = osGet3DSliderState();
		float iod = slider / 3.0f;
		float cp = cosf(phase), sp = sinf(phase);

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(targetLeft, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(targetLeft);
			sceneRender(orbit, cp, sp, -iod);
			if (iod > 0.0f) {
				C3D_RenderTargetClear(targetRight, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
				C3D_FrameDrawOn(targetRight);
				sceneRender(orbit, cp, sp, iod);
			}
		C3D_FrameEnd(0);
	}

	sceneExit();
	C3D_Fini();
	gfxExit();
	return 0;
}
