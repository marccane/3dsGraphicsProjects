// citro3d VERTEX-SHADER showcase: a WAVING FLAG (travelling-wave displacement).
//
// A subdivided grid rippled by a travelling sine wave in the vertex shader. The
// PICA has no sin/cos opcode, so instead of the polynomial-sin approximation
// (which needs fiddly range reduction) we use the fact that the wave's SPATIAL
// phase s = kx*x + ky*y is constant per vertex: the CPU precomputes sin(s)/cos(s)
// once at build time (stored as a vertex attribute), and the shader builds the
// travelling wave via the angle-addition identity with a per-frame (cos wt, sin
// wt) uniform — exact, cheap, no range reduction. (Same trick generalizes to
// twist / breathing.) The flag is pinned at the pole so amplitude grows toward
// the free edge, and cos(s+wt) is reused as a slope-based shading term.
//
// Controls (bottom screen):
//   D-pad U/D = wind strength (amplitude)   D-pad L/R = wave speed
//   A = pause   START = quit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vshader_shbin.h"

#define CLEAR_COLOR 0x10141CFF

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// position (x,y,0) + wave phase (sin s, cos s) + colour (rgb)
typedef struct { float pos[3]; float wave[2]; float col[3]; } vertex;

#define GX 48                       // grid columns
#define GY 28                       // grid rows
#define VERTS (GX * GY)
#define INDS  ((GX-1) * (GY-1) * 6)
#define KX 6.0f                      // spatial frequency across width (~2 waves over x in [-1,1])
#define KY 3.0f                      // + vertical component -> diagonal ripples

static vertex flag_v[VERTS];
static u16    flag_i[INDS];

static DVLB_s* vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection, uLoc_timewave, uLoc_wparams;
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

static void buildFlag(void)
{
	for (int j = 0; j < GY; ++j) {
		for (int i = 0; i < GX; ++i) {
			float x = -1.0f + 2.0f  * i / (GX - 1);     // [-1, 1]
			float y = -0.6f + 1.2f  * j / (GY - 1);     // [-0.6, 0.6]
			float s = KX * x + KY * y;                   // spatial phase (constant per vertex)
			float r, g, b;
			hsv(0.85f * i / (GX - 1), 0.85f, 1.0f, &r, &g, &b);  // rainbow gradient along the flag
			flag_v[j*GX + i] = (vertex){ { x, y, 0.0f }, { sinf(s), cosf(s) }, { r, g, b } };
		}
	}
	int n = 0;
	for (int j = 0; j < GY-1; ++j) {
		for (int i = 0; i < GX-1; ++i) {
			u16 a = j*GX + i, bb = j*GX + i+1, c = (j+1)*GX + i, d = (j+1)*GX + i+1;
			flag_i[n++]=a; flag_i[n++]=bb; flag_i[n++]=c;
			flag_i[n++]=bb; flag_i[n++]=d; flag_i[n++]=c;
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
	uLoc_wparams    = shaderInstanceGetUniformLocation(program.vertexShader, "wparams");

	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0 = position
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2); // v1 = wave phase (sin s, cos s)
	AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 3); // v2 = colour

	buildFlag();
	vbo = linearAlloc(sizeof(flag_v)); memcpy(vbo, flag_v, sizeof(flag_v));
	ibo = linearAlloc(sizeof(flag_i)); memcpy(ibo, flag_i, sizeof(flag_i));

	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo, sizeof(vertex), 3, 0x210);

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL); // reversed-depth convention
	C3D_CullFace(GPU_CULL_NONE);                     // a flag is visible from both sides
}

static void sceneRender(float sway, float amp, float cosT, float sinT, float iod)
{
	C3D_Mtx modelView;
	// Per-eye off-axis projection; zero-parallax plane at the flag distance (3.0)
	// so the ripples pop in front of / recede behind the screen.
	Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(50.0f), C3D_AspectRatioTop,
	                    0.01f, 1000.0f, iod, 3.0f, false);
	Mtx_Identity(&modelView);
	Mtx_Translate(&modelView, 0.0f, 0.0f, -3.0f, true);
	Mtx_RotateX(&modelView, -0.18f, true);           // slight downward tilt to read the ripples
	Mtx_RotateY(&modelView, sway, true);             // sways to face the "wind"

	C3D_Mtx mvp;
	Mtx_Multiply(&mvp, &projection, &modelView);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &mvp);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_timewave, cosT, sinT, 0.0f, 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_wparams,  amp, 0.0f, 0.0f, 0.0f);

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
	gfxSet3D(true);                           // enable the autostereoscopic top screen
	consoleInit(GFX_BOTTOM, NULL);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

	C3D_RenderTarget* targetLeft  = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(targetLeft,  GFX_TOP, GFX_LEFT,  DISPLAY_TRANSFER_FLAGS);
	C3D_RenderTarget* targetRight = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(targetRight, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);

	sceneInit();

	printf("\x1b[2;2H=== Waving Flag (stereo) ===");
	printf("\x1b[3;4Hvertex-shader travelling wave");
	printf("\x1b[6;2HControls:");
	printf("\x1b[7;4HD-pad U/D  wind strength");
	printf("\x1b[8;4HD-pad L/R  wave speed");
	printf("\x1b[9;4HA          pause");
	printf("\x1b[10;4H3D slider  pop-out depth");
	printf("\x1b[11;4HSTART      quit");

	float phase = 0.0f, swayT = 0.0f;
	float amp = 0.30f;      // wind strength (D-pad U/D)
	float speed = 0.060f;   // wave phase per frame (D-pad L/R)
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
		if (kHeld & KEY_DUP)    amp   += 0.005f;
		if (kHeld & KEY_DDOWN)  amp   -= 0.005f;
		if (kHeld & KEY_DRIGHT) speed += 0.004f;
		if (kHeld & KEY_DLEFT)  speed -= 0.004f;
		if (amp < 0.0f)   amp = 0.0f;
		if (amp > 0.8f)   amp = 0.8f;
		if (speed < 0.0f) speed = 0.0f;
		if (speed > 0.3f) speed = 0.3f;

		if (!paused) {
			phase += speed;
			if (phase > 2.0f*M_PI) phase -= 2.0f*M_PI;   // keep cos/sin args bounded
			swayT += 0.012f;
		}
		float sway = 0.5f * sinf(swayT);                 // flag sways +/- ~29 deg

		frames++;
		u64 now = osGetTime();
		if (now - lastT >= 500) { fps = frames * 1000.0f / (float)(now - lastT); frames = 0; lastT = now; }

		printf("\x1b[13;2HWind  : %.2f   ", amp);
		printf("\x1b[14;2HSpeed : %.3f  ", speed);
		printf("\x1b[15;2HState : %-7s ", paused ? "PAUSED" : "RUNNING");
		printf("\x1b[30;1HFPS: %.1f  ", fps);

		float slider = osGet3DSliderState();
		float iod = slider / 3.0f;
		float cp = cosf(phase), sp = sinf(phase);

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			// Left eye.
			C3D_RenderTargetClear(targetLeft, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(targetLeft);
			sceneRender(sway, amp, cp, sp, -iod);
			// Right eye — only when the slider is up.
			if (iod > 0.0f) {
				C3D_RenderTargetClear(targetRight, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
				C3D_FrameDrawOn(targetRight);
				sceneRender(sway, amp, cp, sp, iod);
			}
		C3D_FrameEnd(0);
	}

	sceneExit();
	C3D_Fini();
	gfxExit();
	return 0;
}
