// citro3d HARDWARE SHADOW-MAPPING showcase (stereoscopic).
//
// Real two-pass shadow mapping on the PICA200 — the genuinely-hardware kind,
// not a fake. A torus floats above a ground plane; an orbiting light sweeps a
// crisp shadow across the floor, and the torus self-shadows.
//
//   Pass 1 (caster):   render the torus from the LIGHT's point of view into a
//                       depth texture (C3D_TexInitShadow + a DEPTH16 render
//                       target). This is the shadow map.
//   Pass 2 (receiver): render the scene from the camera; the PICA fragment
//                       LIGHTING UNIT does the shadow comparison against that
//                       depth texture (C3D_LightEnvShadowMode / GPU_SHADOW_*),
//                       so shadowed fragments are darkened for free.
//
// The two vertex shaders (shadow_caster / shadow_receiver) are the proven
// devkitPro example shaders, used verbatim: the receiver emits the normal as a
// quaternion (normalquat) + the view vector for the lighting unit, plus the
// projective shadow-map texcoord computed from a light view-projection uniform.
// The GPU state mirrors the stock example exactly (depth left at the citro3d
// reversed-depth default, depth cleared to 0) so behaviour is known-good; only
// the geometry (our torus), the orbiting light, the two materials, and the HUD
// are ours.
//
// Controls (bottom screen):
//   A = pause   B = toggle shadow filter   D-pad U/D = depth bias
//   3D slider = pop-out depth   START = quit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "shadow_caster_shbin.h"
#include "shadow_receiver_shbin.h"

#define CLEAR_COLOR 0x141824FF   // dark slate (0xRRGGBBAA)

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float position[3]; float normal[3]; } vertex;

// ---- Torus (caster + receiver; self-shadows and casts onto the plane) ----
#define TORUS_RINGS 60          // segments around the main ring
#define TORUS_SIDES 30          // segments around the tube
#define TORUS_VTX   (TORUS_RINGS * TORUS_SIDES)
#define TORUS_IDX   (TORUS_RINGS * TORUS_SIDES * 6)
#define TORUS_R     1.05f       // major radius
#define TORUS_r     0.40f       // tube radius

static vertex torus_v[TORUS_VTX];
static u16    torus_i[TORUS_IDX];

// ---- Ground plane (receiver) ----
static const float plane_vertices[] =
{
	-4.0f, -1.5f, -4.0f,  0,1,0,
	 4.0f, -1.5f, -4.0f,  0,1,0,
	-4.0f, -1.5f,  4.0f,  0,1,0,
	 4.0f, -1.5f,  4.0f,  0,1,0,
};
#define plane_index_count 6
static const u16 plane_indices[] = { 2, 1, 0, 1, 2, 3 };

// ---- Shadow map ----
static C3D_RenderTarget* shadow_map_rt;
static C3D_Tex           shadow_map_tex;
static int   filter;
static float bias;

// ---- Shaders ----
static DVLB_s* caster_dvlb;
static shaderProgram_s caster_program;
static int caster_uLoc_model, caster_uLoc_viewproj;

static DVLB_s* receiver_dvlb;
static shaderProgram_s receiver_program;
static int recv_uLoc_model, recv_uLoc_view, recv_uLoc_proj, recv_uLoc_light_viewproj;

static C3D_LightEnv lightEnv;
static C3D_Light    light;
static C3D_Mtx      light_view, light_proj;

static C3D_BufInfo torus_buf, plane_buf;
static void *torus_vbo, *torus_ibo, *plane_vbo, *plane_ibo;

static float elapsed;   // model spin
static float lightT;    // light orbit angle

static const C3D_Material mat_torus =
{
	{ 0.18f, 0.12f, 0.06f }, // ambient
	{ 0.90f, 0.58f, 0.22f }, // diffuse (warm gold)
	{ 0.0f, 0.0f, 0.0f },    // specular0
	{ 0.0f, 0.0f, 0.0f },    // specular1
	{ 0.0f, 0.0f, 0.0f },    // emission
};
static const C3D_Material mat_floor =
{
	{ 0.10f, 0.12f, 0.16f }, // ambient
	{ 0.42f, 0.48f, 0.60f }, // diffuse (cool slate)
	{ 0.0f, 0.0f, 0.0f },
	{ 0.0f, 0.0f, 0.0f },
	{ 0.0f, 0.0f, 0.0f },
};

static void buildTorus(void)
{
	for (int i = 0; i < TORUS_RINGS; ++i) {
		float th = 2.0f * M_PI * i / TORUS_RINGS, ct = cosf(th), st = sinf(th);
		for (int j = 0; j < TORUS_SIDES; ++j) {
			float ph = 2.0f * M_PI * j / TORUS_SIDES, cp = cosf(ph), sp = sinf(ph);
			float nx = cp * ct, ny = sp, nz = cp * st;          // outward normal
			float x = (TORUS_R + TORUS_r * cp) * ct;
			float y =  TORUS_r * sp;
			float z = (TORUS_R + TORUS_r * cp) * st;
			torus_v[i * TORUS_SIDES + j] = (vertex){ { x, y, z }, { nx, ny, nz } };
		}
	}
	int k = 0;
	for (int i = 0; i < TORUS_RINGS; ++i) {
		int i2 = (i + 1) % TORUS_RINGS;
		for (int j = 0; j < TORUS_SIDES; ++j) {
			int j2 = (j + 1) % TORUS_SIDES;
			u16 a = i * TORUS_SIDES + j,  b = i2 * TORUS_SIDES + j;
			u16 c = i * TORUS_SIDES + j2, d = i2 * TORUS_SIDES + j2;
			torus_i[k++] = a; torus_i[k++] = b; torus_i[k++] = c;
			torus_i[k++] = b; torus_i[k++] = d; torus_i[k++] = c;
		}
	}
}

// Position the orbiting light; fills light_view / light_proj (used by both passes).
static C3D_FVec updateLight(void)
{
	C3D_FVec lpos = FVec3_New(4.6f * cosf(lightT), 4.8f, 4.6f * sinf(lightT));
	C3D_FVec ltar = FVec3_New(0, 0, 0);
	C3D_FVec lup  = FVec3_New(0, 1, 0);
	Mtx_LookAt(&light_view, lpos, ltar, lup, false);
	// Tight ortho so the 512^2 shadow map covers the scene at good resolution.
	Mtx_Ortho(&light_proj, -3.4f, 3.4f, -3.4f, 3.4f, 1.0f, 12.0f, false);
	return lpos;
}

static void sceneInit(void)
{
	filter = GPU_LINEAR;
	bias   = 0.005f;
	elapsed = 0.0f;
	lightT  = 0.0f;

	// Shadow-map depth texture + render target.
	C3D_TexInitShadow(&shadow_map_tex, 512, 512);
	shadow_map_rt = C3D_RenderTargetCreateFromTex(&shadow_map_tex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH16);
	shadow_map_tex.border = 0xFFFFFFFF;                 // off-map = fully lit
	C3D_TexSetWrap(&shadow_map_tex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);

	// Caster shader.
	caster_dvlb = DVLB_ParseFile((u32*)shadow_caster_shbin, shadow_caster_shbin_size);
	shaderProgramInit(&caster_program);
	shaderProgramSetVsh(&caster_program, &caster_dvlb->DVLE[0]);
	caster_uLoc_viewproj = shaderInstanceGetUniformLocation(caster_program.vertexShader, "viewproj");
	caster_uLoc_model    = shaderInstanceGetUniformLocation(caster_program.vertexShader, "model");

	// Receiver shader.
	receiver_dvlb = DVLB_ParseFile((u32*)shadow_receiver_shbin, shadow_receiver_shbin_size);
	shaderProgramInit(&receiver_program);
	shaderProgramSetVsh(&receiver_program, &receiver_dvlb->DVLE[0]);
	recv_uLoc_proj           = shaderInstanceGetUniformLocation(receiver_program.vertexShader, "proj");
	recv_uLoc_view           = shaderInstanceGetUniformLocation(receiver_program.vertexShader, "view");
	recv_uLoc_model          = shaderInstanceGetUniformLocation(receiver_program.vertexShader, "model");
	recv_uLoc_light_viewproj = shaderInstanceGetUniformLocation(receiver_program.vertexShader, "light_viewproj");

	// Attributes shared by both shaders: v0 = position, v1 = normal.
	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 3);

	// Torus VBO/IBO.
	buildTorus();
	torus_vbo = linearAlloc(sizeof(torus_v)); memcpy(torus_vbo, torus_v, sizeof(torus_v));
	torus_ibo = linearAlloc(sizeof(torus_i)); memcpy(torus_ibo, torus_i, sizeof(torus_i));
	BufInfo_Init(&torus_buf);
	BufInfo_Add(&torus_buf, torus_vbo, sizeof(vertex), 2, 0x10);

	// Plane VBO/IBO.
	plane_vbo = linearAlloc(sizeof(plane_vertices)); memcpy(plane_vbo, plane_vertices, sizeof(plane_vertices));
	plane_ibo = linearAlloc(sizeof(plane_indices));  memcpy(plane_ibo, plane_indices, sizeof(plane_indices));
	BufInfo_Init(&plane_buf);
	BufInfo_Add(&plane_buf, plane_vbo, sizeof(float[6]), 2, 0x10);

	// Lighting environment + shadow-enabled light (directional).
	C3D_LightEnvInit(&lightEnv);
	C3D_LightEnvMaterial(&lightEnv, &mat_torus);
	C3D_LightInit(&light, &lightEnv);
	C3D_LightColor(&light, 1.0f, 0.96f, 0.88f);          // warm sun
	C3D_LightShadowEnable(&light, true);
	C3D_LightEnvShadowMode(&lightEnv, GPU_SHADOW_PRIMARY);
	C3D_LightEnvShadowSel(&lightEnv, 0);                 // shadow map on tex unit 0
}

// Pass 1: render the torus from the light's POV into the shadow map.
static void drawShadowMap(void)
{
	C3D_BindProgram(&caster_program);
	C3D_CullFace(GPU_CULL_NONE);                         // closed torus; bias handles acne
	C3D_ColorLogicOp(GPU_LOGICOP_COPY);

	// Only alpha is compared against the fragment depth (matches the stock example).
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvColor(env, 0xFFFFFFFF);
	C3D_TexEnvSrc(env, C3D_RGB,   GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(env, C3D_RGB,   GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);

	C3D_Mtx viewproj;
	Mtx_Multiply(&viewproj, &light_proj, &light_view);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, caster_uLoc_viewproj, &viewproj);

	C3D_Mtx model;
	Mtx_Identity(&model);
	Mtx_RotateY(&model, elapsed, true);
	Mtx_RotateX(&model, elapsed * 0.6f, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, caster_uLoc_model, &model);

	C3D_SetBufInfo(&torus_buf);
	C3D_DrawElements(GPU_TRIANGLES, TORUS_IDX, C3D_UNSIGNED_SHORT, torus_ibo);
}

// Pass 2: render the scene from the camera, with shadows applied by the lighting unit.
static void sceneRender(float iod)
{
	C3D_BindProgram(&receiver_program);
	C3D_CullFace(GPU_CULL_NONE);

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_FRAGMENT_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	C3D_Mtx proj;
	Mtx_PerspStereoTilt(&proj, C3D_AngleFromDegrees(38.0f), C3D_AspectRatioTop, 1.0f, 100.0f, iod, 7.0f, false);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, recv_uLoc_proj, &proj);

	C3D_Mtx view;
	Mtx_LookAt(&view, FVec3_New(0, 3.4f, 6.6f), FVec3_New(0, -0.3f, 0), FVec3_New(0, 1, 0), false);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, recv_uLoc_view, &view);

	C3D_Mtx light_viewproj;
	Mtx_Multiply(&light_viewproj, &light_proj, &light_view);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, recv_uLoc_light_viewproj, &light_viewproj);

	C3D_TexShadowParams(false, bias);                    // false = ortho light
	C3D_TexBind(0, &shadow_map_tex);
	C3D_TexSetFilter(&shadow_map_tex, filter, filter);
	C3D_LightEnvBind(&lightEnv);

	// Torus (warm), spinning.
	C3D_LightEnvMaterial(&lightEnv, &mat_torus);
	C3D_Mtx model;
	Mtx_Identity(&model);
	Mtx_RotateY(&model, elapsed, true);
	Mtx_RotateX(&model, elapsed * 0.6f, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, recv_uLoc_model, &model);
	C3D_SetBufInfo(&torus_buf);
	C3D_DrawElements(GPU_TRIANGLES, TORUS_IDX, C3D_UNSIGNED_SHORT, torus_ibo);

	// Floor (cool), static.
	C3D_LightEnvMaterial(&lightEnv, &mat_floor);
	Mtx_Identity(&model);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, recv_uLoc_model, &model);
	C3D_SetBufInfo(&plane_buf);
	C3D_DrawElements(GPU_TRIANGLES, plane_index_count, C3D_UNSIGNED_SHORT, plane_ibo);
}

static void sceneExit(void)
{
	C3D_RenderTargetDelete(shadow_map_rt);
	C3D_TexDelete(&shadow_map_tex);
	linearFree(torus_vbo); linearFree(torus_ibo);
	linearFree(plane_vbo); linearFree(plane_ibo);
	shaderProgramFree(&caster_program);   DVLB_Free(caster_dvlb);
	shaderProgramFree(&receiver_program); DVLB_Free(receiver_dvlb);
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

	printf("\x1b[2;2H=== Hardware Shadow Mapping ===");
	printf("\x1b[3;5Htorus + orbiting light, stereo");
	printf("\x1b[6;2HControls:");
	printf("\x1b[7;4HA          pause");
	printf("\x1b[8;4HB          shadow filter");
	printf("\x1b[9;4HD-pad U/D  depth bias");
	printf("\x1b[10;4H3D slider  pop-out depth");
	printf("\x1b[11;4HSTART      quit");

	bool paused = false;
	u64 lastT = osGetTime();
	int frames = 0;
	float fps = 0.0f;

	while (aptMainLoop())
	{
		hidScanInput();
		u32 kDown = hidKeysDown();
		u32 kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;
		if (kDown & KEY_A) paused = !paused;
		if (kDown & KEY_B) filter = (filter == GPU_NEAREST) ? GPU_LINEAR : GPU_NEAREST;
		if (kHeld & KEY_DUP)   bias += 0.001f;
		if (kHeld & KEY_DDOWN) bias -= 0.001f;
		if (bias < 0.0f)   bias = 0.0f;
		if (bias > 0.05f)  bias = 0.05f;

		if (!paused) { elapsed += 1.0f / 60.0f; lightT += 0.012f; }
		updateLight();
		// Keep the lighting-unit light direction in sync with the orbiting shadow light.
		C3D_FVec ldir = FVec4_New(4.6f * cosf(lightT), 4.8f, 4.6f * sinf(lightT), 0.0f);
		C3D_LightPosition(&light, &ldir);

		frames++;
		u64 now = osGetTime();
		if (now - lastT >= 500) { fps = frames * 1000.0f / (float)(now - lastT); frames = 0; lastT = now; }

		printf("\x1b[14;2HBias   : %.3f   ", bias);
		printf("\x1b[15;2HFilter : %-7s ", filter == GPU_LINEAR ? "LINEAR" : "NEAREST");
		printf("\x1b[16;2HState  : %-7s ", paused ? "PAUSED" : "RUNNING");
		printf("\x1b[30;1HFPS: %.1f  ", fps);

		float slider = osGet3DSliderState();
		float iod = slider / 3.0f;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			// Pass 1: shadow map (shared by both eyes).
			C3D_RenderTargetClear(shadow_map_rt, C3D_CLEAR_ALL, 0xFFFFFFFF, 0);
			C3D_FrameDrawOn(shadow_map_rt);
			drawShadowMap();

			// Pass 2: left eye.
			C3D_RenderTargetClear(targetLeft, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(targetLeft);
			sceneRender(-iod);

			// Pass 2: right eye (only when the slider is up).
			if (iod > 0.0f) {
				C3D_RenderTargetClear(targetRight, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
				C3D_FrameDrawOn(targetRight);
				sceneRender(iod);
			}
		C3D_FrameEnd(0);
	}

	sceneExit();
	C3D_Fini();
	gfxExit();
	return 0;
}
