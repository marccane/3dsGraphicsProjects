// citro3d GEOMETRY-SHADER showcase #2b: STEREOSCOPIC 3D exploding icosahedron.
//
// Same geometry-shader explode as `citro3d-icosahedron` (per-face normal via
// cross product -> fly each face out along it), but rendered for the 3DS's
// autostereoscopic top screen: the scene is drawn TWICE per frame, once into
// the GFX_LEFT framebuffer and once into GFX_RIGHT, each with its own
// per-eye projection (Mtx_PerspStereoTilt). With the physical 3D slider up,
// the exploding shards literally pop out of the screen.
//
// How the stereo works:
//   * gfxSet3D(true) enables the parallax-barrier 3D mode on the top screen.
//   * Two render targets share the top screen: one bound to GFX_LEFT, one to
//     GFX_RIGHT. Each frame we clear+draw both.
//   * osGet3DSliderState() (0..1) is the user's depth dial. We scale it into an
//     interocular offset `iod`; the left eye uses -iod, the right uses +iod.
//   * Mtx_PerspStereoTilt(... iod, focLen ...) builds an off-axis (sheared)
//     frustum per eye. `focLen` is the zero-parallax (screen) plane: geometry
//     nearer than it pops OUT toward the viewer, farther recedes INTO the
//     screen. We sit the icosahedron's centre near the screen plane so the
//     near shards fly out of the display as it blows apart.
//   * When the slider is at 0 we skip the right eye entirely (it's redundant).
//
// Controls (shown on the bottom screen):
//   B = toggle glass / solid   Y = toggle dynamic (opacity synced to the blast)
//   D-pad L/R = tune glass opacity   3D slider = depth   START = quit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

// Distance from camera to the icosahedron centre. Also used as the stereo
// zero-parallax (screen) plane, so the solid straddles the display surface and
// the near-side shards pop forward when it explodes.
#define OBJ_DIST  3.2f

static vertex vertex_list[VTX_COUNT];

static DVLB_s* program_dvlb;
static shaderProgram_s program;
static int uLoc_projection, uLoc_params;
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
	srand((u32)svcGetSystemTick());
	for (int f = 0; f < N_FACES; ++f) {
		float r, g, b;
		hsv(frand(), 1.0f, 1.0f, &r, &g, &b);  // fully-saturated random hue -> vivid, varied
		float seed = frand();                   // per-face random "solidity", used by DYNAMIC mode
		for (int k = 0; k < 3; ++k) {
			const float* p = V[F[f][k]];
			float inv = 1.0f / sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]); // -> unit sphere
			vertex_list[f*3+k] = (vertex){ { p[0]*inv, p[1]*inv, p[2]*inv }, { r, g, b, seed } };
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

	// Standard alpha blending so the per-face alpha (our "glass" term) reads as translucency.
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL); // PICA convention: depth clears to 0, nearer = greater
	C3D_CullFace(GPU_CULL_NONE);                  // shards expose their back faces
}

// Render the scene once for one eye. `iod` is the signed interocular offset
// (negative = left eye, positive = right eye, 0 = mono). The modelView is the
// same for both eyes; only the projection's off-axis shear differs.
static void sceneRender(float ax, float ay, float bloom, float aFloor, float aGain,
                        float randMix, float iod)
{
	C3D_Mtx modelView;
	Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(45.0f), C3D_AspectRatioTop,
	                    0.01f, 1000.0f, iod, OBJ_DIST, false);
	Mtx_Identity(&modelView);
	Mtx_Translate(&modelView, 0.0f, 0.0f, -OBJ_DIST, true);
	Mtx_RotateX(&modelView, ax, true);
	Mtx_RotateY(&modelView, ay, true);

	C3D_Mtx mvp;
	Mtx_Multiply(&mvp, &projection, &modelView);

	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, uLoc_projection, &mvp);
	// params: x = bloom, y = alpha floor, z = alpha gain, w = random-mode flag (0/1)
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, uLoc_params, bloom, aFloor, aGain, randMix);

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
	gfxSet3D(true);                           // enable the autostereoscopic top screen
	consoleInit(GFX_BOTTOM, NULL);            // text console on the bottom screen
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

	// One render target per eye, both feeding the top screen.
	C3D_RenderTarget* targetLeft  = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(targetLeft,  GFX_TOP, GFX_LEFT,  DISPLAY_TRANSFER_FLAGS);
	C3D_RenderTarget* targetRight = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(targetRight, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);

	sceneInit();

	// Static controls text (bottom screen). Dynamic status is printed in the loop.
	printf("\x1b[2;2H=== Stereo Icosahedron ===");
	printf("\x1b[3;5Hstereoscopic 3D geo-shader");
	printf("\x1b[6;2HControls:");
	printf("\x1b[7;4HB          toggle GLASS / SOLID");
	printf("\x1b[8;4HY          toggle DYNAMIC opacity");
	printf("\x1b[9;4HD-pad < >  tune glass opacity");
	printf("\x1b[10;4H3D slider  pop-out depth");
	printf("\x1b[11;4HSTART      quit");

	float ax = 0.0f, ay = 0.0f, tt = 0.0f;
	bool  solid = false;       // B toggles glass <-> solid
	bool  dynamic = false;     // Y toggles auto-opacity synced to the explosion
	float glassFloor = 0.45f;  // minimum opacity in glass mode (D-pad L/R tunes it live)

	while (aptMainLoop())
	{
		hidScanInput();
		u32 kDown = hidKeysDown();
		u32 kHeld = hidKeysHeld();
		if (kDown & KEY_START)
			break;
		if (kDown & KEY_B)
			solid = !solid;                       // toggle glass / solid
		if (kDown & KEY_Y)
			dynamic = !dynamic;                   // toggle dynamic (synced) opacity
		if (kHeld & KEY_DLEFT)  glassFloor -= 0.01f; // more transparent
		if (kHeld & KEY_DRIGHT) glassFloor += 0.01f; // more opaque
		if (glassFloor < 0.0f)  glassFloor = 0.0f;
		if (glassFloor > 0.95f) glassFloor = 0.95f;

		ax += 0.011f;
		ay += 0.017f;            // different rates -> tumbling
		tt += 0.028f;
		float bloom = 0.5f + 0.5f * sinf(tt); // closed (0) <-> blown apart (1)

		// alpha = clamp(floor + gain * max(N·L,0), 0, 1)
		float aFloor, aGain;
		const char* modeStr;
		if (dynamic) {
			// Sync opacity to the blast: assembled = near-solid, blown apart = ghostly.
			aFloor = 0.90f - 0.90f * bloom;       // bloom 0 -> 0.90
			aGain  = 1.0f - aFloor;
			modeStr = "DYNAMIC (synced)";
		} else if (solid) {
			aFloor = 1.0f; aGain = 0.0f;
			modeStr = "SOLID";
		} else {
			aFloor = glassFloor; aGain = 1.0f - glassFloor;
			modeStr = "GLASS";
		}

		// 3D depth slider (0..1) -> interocular offset. /3 keeps it comfortable.
		float slider = osGet3DSliderState();
		float iod = slider / 3.0f;

		// Live status on the bottom screen (trailing spaces clear leftovers).
		printf("\x1b[14;2HMode    : %-18s", modeStr);
		printf("\x1b[15;2HOpacity : %.2f   ", aFloor);
		printf("\x1b[16;2H3D depth: %.2f   ", slider);

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			// Left eye.
			C3D_RenderTargetClear(targetLeft, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(targetLeft);
			sceneRender(ax, ay, bloom, aFloor, aGain, dynamic ? 1.0f : 0.0f, -iod);
			// Right eye — only when the slider is up (otherwise it's identical to the left).
			if (iod > 0.0f) {
				C3D_RenderTargetClear(targetRight, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
				C3D_FrameDrawOn(targetRight);
				sceneRender(ax, ay, bloom, aFloor, aGain, dynamic ? 1.0f : 0.0f, iod);
			}
		C3D_FrameEnd(0);
	}

	sceneExit();
	C3D_Fini();
	gfxExit();
	return 0;
}
