// citro3d VERTEX-SHADER showcase: SPHERICAL BREATHING (stereoscopic).
//
// A UV sphere that pulses in and out, with latitude bands rippling pole-to-pole.
// Third effect in the precompute-the-spatial-phase family (flag = displacement,
// twist = rotation, this = displacement ALONG the normal). For a unit sphere the
// normal is the position, so the vertex shader just scales each vertex radially
// by 1 + disp, where disp = ampUniform*sin(wt) + ampRipple*sin(s+wt). The ripple
// phase s = k*latitude is constant per vertex, so the CPU precomputes sin(s)/cos(s)
// and the shader forms sin(s+wt) by angle addition with a per-frame (cos wt,
// sin wt) uniform — no in-shader sin. s uses latitude only, so the coincident
// pole vertices share it and don't tear.
//
// Controls (bottom screen):
//   D-pad U/D = breath amplitude   D-pad L/R = speed   A = pause   START = quit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vshader_shbin.h"

#define CLEAR_COLOR 0x0A0C16FF

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// unit-sphere position (== normal) + breath phase (sin s, cos s) + colour
typedef struct { float pos[3]; float tw[2]; float col[3]; } vertex;

#define NLAT  48                    // latitude bands
#define NLON  64                    // longitude segments
#define VERTS ((NLAT+1) * (NLON+1))
#define INDS  (NLAT * NLON * 6)
#define KLAT  4.0f                   // ripple frequency over latitude (~2 waves pole-to-pole)

static vertex sph_v[VERTS];
static u16    sph_i[INDS];

static DVLB_s* vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection, uLoc_timewave, uLoc_amps, uLoc_light;
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

static void buildSphere(void)
{
	for (int i = 0; i <= NLAT; ++i) {
		float lat = -M_PI*0.5f + M_PI * i / NLAT;   // -pi/2 .. pi/2
		float cy = sinf(lat), cr = cosf(lat);
		float s = KLAT * lat;                        // spatial phase (latitude only)
		float ss = sinf(s), cs = cosf(s);
		float r, g, b;
		hsv(0.05f + 0.80f * (lat + M_PI*0.5f) / M_PI, 0.85f, 1.0f, &r, &g, &b); // rainbow pole->pole
		for (int j = 0; j <= NLON; ++j) {
			float lon = 2.0f*M_PI * j / NLON;
			float x = cr * cosf(lon), z = cr * sinf(lon);
			sph_v[i*(NLON+1)+j] = (vertex){ { x, cy, z }, { ss, cs }, { r, g, b } };
		}
	}
	int n = 0;
	for (int i = 0; i < NLAT; ++i) {
		for (int j = 0; j < NLON; ++j) {
			u16 a = i*(NLON+1)+j, bb = i*(NLON+1)+j+1, c = (i+1)*(NLON+1)+j, d = (i+1)*(NLON+1)+j+1;
			sph_i[n++]=a; sph_i[n++]=bb; sph_i[n++]=d;
			sph_i[n++]=a; sph_i[n++]=d;  sph_i[n++]=c;
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
	uLoc_amps       = shaderInstanceGetUniformLocation(program.vertexShader, "amps");
	uLoc_light      = shaderInstanceGetUniformLocation(program.vertexShader, "lightdir");

	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0 = position (= normal)
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2); // v1 = breath phase (sin s, cos s)
	AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 3); // v2 = colour

	buildSphere();
	vbo = linearAlloc(sizeof(sph_v)); memcpy(vbo, sph_v, sizeof(sph_v));
	ibo = linearAlloc(sizeof(sph_i)); memcpy(ibo, sph_i, sizeof(sph_i));

	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo, sizeof(vertex), 3, 0x210);

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_CullFace(GPU_CULL_BACK_CCW);
}

static void sceneRender(float spin, float ampMul, float cosT, float sinT, float iod)
{
	C3D_Mtx modelView;
	Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(45.0f), C3D_AspectRatioTop,
	                    0.01f, 1000.0f, iod, 3.4f, false);
	Mtx_Identity(&modelView);
	Mtx_Translate(&modelView, 0.0f, 0.0f, -3.4f, true);
	Mtx_RotateX(&modelView, -0.30f, true);
	Mtx_RotateY(&modelView, spin, true);

	C3D_Mtx mvp;
	Mtx_Multiply(&mvp, &projection, &modelView);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &mvp);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_timewave, cosT, sinT, 0.0f, 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_amps, 0.06f*ampMul, 0.16f*ampMul, 0.0f, 0.0f);

	// World-fixed light expressed in model space (headlight): R^T * worldLight.
	C3D_Mtx rt;
	Mtx_Identity(&rt);
	Mtx_RotateY(&rt, -spin, true);
	Mtx_RotateX(&rt,  0.30f, true);
	C3D_FVec lm = Mtx_MultiplyFVec4(&rt, FVec4_New(0.40f, 0.55f, 0.73f, 0.0f));
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

	printf("\x1b[2;2H=== Spherical Breathing (stereo) ===");
	printf("\x1b[3;6Hvertex-shader normal displacement");
	printf("\x1b[6;2HControls:");
	printf("\x1b[7;4HD-pad U/D  breath amplitude");
	printf("\x1b[8;4HD-pad L/R  speed");
	printf("\x1b[9;4HA          pause");
	printf("\x1b[10;4H3D slider  pop-out depth");
	printf("\x1b[11;4HSTART      quit");

	float phase = 0.0f, spin = 0.0f;
	float ampMul = 1.0f;       // breath amplitude multiplier (D-pad U/D)
	float speed = 0.040f;      // breath phase per frame (D-pad L/R)
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
		if (kHeld & KEY_DUP)    ampMul += 0.02f;
		if (kHeld & KEY_DDOWN)  ampMul -= 0.02f;
		if (kHeld & KEY_DRIGHT) speed  += 0.004f;
		if (kHeld & KEY_DLEFT)  speed  -= 0.004f;
		if (ampMul < 0.0f) ampMul = 0.0f;
		if (ampMul > 2.5f) ampMul = 2.5f;
		if (speed < 0.0f)  speed = 0.0f;
		if (speed > 0.2f)  speed = 0.2f;

		if (!paused) {
			phase += speed;
			if (phase > 2.0f*M_PI) phase -= 2.0f*M_PI;
		}
		spin += 0.006f;          // gentle orbit so the breathing reads in 3D

		frames++;
		u64 now = osGetTime();
		if (now - lastT >= 500) { fps = frames * 1000.0f / (float)(now - lastT); frames = 0; lastT = now; }

		printf("\x1b[14;2HAmplitude : %.2f  ", ampMul);
		printf("\x1b[15;2HSpeed     : %.3f  ", speed);
		printf("\x1b[16;2HState     : %-7s ", paused ? "PAUSED" : "RUNNING");
		printf("\x1b[30;1HFPS: %.1f  ", fps);

		float slider = osGet3DSliderState();
		float iod = slider / 3.0f;
		float cp = cosf(phase), sp = sinf(phase);

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(targetLeft, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(targetLeft);
			sceneRender(spin, ampMul, cp, sp, -iod);
			if (iod > 0.0f) {
				C3D_RenderTargetClear(targetRight, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
				C3D_FrameDrawOn(targetRight);
				sceneRender(spin, ampMul, cp, sp, iod);
			}
		C3D_FrameEnd(0);
	}

	sceneExit();
	C3D_Fini();
	gfxExit();
	return 0;
}
