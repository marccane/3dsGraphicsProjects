// citro3d — EXPLODING ICOSAHEDRON casting REAL hardware shadows (stereoscopic).
//
// Combines our geometry-shader explode with the PICA's hardware shadow mapping:
// a tumbling icosahedron blows apart into 20 lit shards, and those shards cast a
// real, animated shadow onto a floor below as an orbiting light sweeps around.
//
//   Pass 1 (caster):   the exploding icosahedron is rendered FROM THE LIGHT into
//                       a depth texture by a geometry shader that does the same
//                       explosion (caster.g.pica) — so the shadow matches the
//                       flying shards exactly.
//   Pass 2 (camera):   the FLOOR is drawn with the stock shadow-receiver vertex
//                       shader and the lighting unit applies the shadow; then the
//                       icosahedron is drawn with its own geometry-shader flat
//                       shading (ico.g.pica).
//
// Design note: only the FLOOR uses the fragment-lighting/shadow path (the proven
// stock receiver VS). The icosahedron's geometry shaders only ever output
// position + colour — never the lighting-unit semantics (normalquat/view) — so
// the risky "GS drives fragment lighting" path is avoided entirely. The icosahedron
// doesn't self-shadow, but it casts a real shadow and keeps its vivid flat look.
//
// Controls (bottom screen):
//   A = pause   B = shadow filter   D-pad U/D = depth bias
//   3D slider = pop-out depth   START = quit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "caster_shbin.h"
#include "ico_shbin.h"
#include "shadow_receiver_shbin.h"

#define CLEAR_COLOR 0x121622FF

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float position[3]; float color[4];  } icovtx;  // icosahedron: pos + colour
typedef struct { float position[3]; float normal[3]; } flrvtx;  // floor: pos + normal

#define N_FACES   20
#define VTX_COUNT (N_FACES * 3)
#define FLOOR_Y   (-2.6f)
#define FLOOR_E    4.0f

static icovtx ico_list[VTX_COUNT];
static const flrvtx floor_list[6] =
{
	{ {-FLOOR_E, FLOOR_Y, -FLOOR_E}, {0,1,0} },
	{ { FLOOR_E, FLOOR_Y, -FLOOR_E}, {0,1,0} },
	{ {-FLOOR_E, FLOOR_Y,  FLOOR_E}, {0,1,0} },
	{ { FLOOR_E, FLOOR_Y, -FLOOR_E}, {0,1,0} },
	{ { FLOOR_E, FLOOR_Y,  FLOOR_E}, {0,1,0} },
	{ {-FLOOR_E, FLOOR_Y,  FLOOR_E}, {0,1,0} },
};

// Shadow map
static C3D_RenderTarget* shadow_rt;
static C3D_Tex           shadow_tex;
static int   filter;
static float bias;

// Shaders
static DVLB_s* caster_dvlb;   static shaderProgram_s caster_prog;
static int caster_uLoc_lightmvp, caster_uLoc_params;
static DVLB_s* ico_dvlb;      static shaderProgram_s ico_prog;
static int ico_uLoc_proj, ico_uLoc_lightdir, ico_uLoc_params;
static DVLB_s* floor_dvlb;    static shaderProgram_s floor_prog;
static int flr_uLoc_proj, flr_uLoc_view, flr_uLoc_model, flr_uLoc_light_vp;

static C3D_LightEnv lightEnv;
static C3D_Light    light;

static C3D_BufInfo ico_buf, floor_buf;
static void *ico_vbo, *floor_vbo;

// Per-frame state shared by the two passes.
static C3D_Mtx g_model, g_view, g_light_view, g_light_proj, g_light_vp;
static C3D_FVec g_lightdir_model;
static float g_bloom;

static const C3D_Material mat_floor =
{
	{ 0.13f, 0.14f, 0.18f }, // ambient
	{ 0.40f, 0.44f, 0.54f }, // diffuse (cool slate)
	{ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
};

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
	const float t = 1.61803398875f;
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
		hsv(frand(), 1.0f, 1.0f, &r, &g, &b);   // vivid random hue per face
		for (int k = 0; k < 3; ++k) {
			const float* p = V[F[f][k]];
			float inv = 1.0f / sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]); // -> unit sphere
			ico_list[f*3+k] = (icovtx){ { p[0]*inv, p[1]*inv, p[2]*inv }, { r, g, b, 1.0f } };
		}
	}
}

static void useIcoAttr(void)
{
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3); // v0 = position
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 4); // v1 = colour
}

static void useFloorAttr(void)
{
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3); // v0 = position
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 3); // v1 = normal
}

static void sceneInit(void)
{
	filter = GPU_LINEAR;
	bias   = 0.006f;

	// Shadow map.
	C3D_TexInitShadow(&shadow_tex, 512, 512);
	shadow_rt = C3D_RenderTargetCreateFromTex(&shadow_tex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH16);
	shadow_tex.border = 0xFFFFFFFF;
	C3D_TexSetWrap(&shadow_tex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);

	// Caster program (vsh + gsh).
	caster_dvlb = DVLB_ParseFile((u32*)caster_shbin, caster_shbin_size);
	shaderProgramInit(&caster_prog);
	shaderProgramSetVsh(&caster_prog, &caster_dvlb->DVLE[0]);
	shaderProgramSetGsh(&caster_prog, &caster_dvlb->DVLE[1], 6);
	caster_uLoc_lightmvp = shaderInstanceGetUniformLocation(caster_prog.geometryShader, "lightmvp");
	caster_uLoc_params   = shaderInstanceGetUniformLocation(caster_prog.geometryShader, "params");

	// Icosahedron program (vsh + gsh).
	ico_dvlb = DVLB_ParseFile((u32*)ico_shbin, ico_shbin_size);
	shaderProgramInit(&ico_prog);
	shaderProgramSetVsh(&ico_prog, &ico_dvlb->DVLE[0]);
	shaderProgramSetGsh(&ico_prog, &ico_dvlb->DVLE[1], 6);
	ico_uLoc_proj     = shaderInstanceGetUniformLocation(ico_prog.geometryShader, "projection");
	ico_uLoc_lightdir = shaderInstanceGetUniformLocation(ico_prog.geometryShader, "lightdir");
	ico_uLoc_params   = shaderInstanceGetUniformLocation(ico_prog.geometryShader, "params");

	// Floor program (stock receiver vsh only).
	floor_dvlb = DVLB_ParseFile((u32*)shadow_receiver_shbin, shadow_receiver_shbin_size);
	shaderProgramInit(&floor_prog);
	shaderProgramSetVsh(&floor_prog, &floor_dvlb->DVLE[0]);
	flr_uLoc_proj     = shaderInstanceGetUniformLocation(floor_prog.vertexShader, "proj");
	flr_uLoc_view     = shaderInstanceGetUniformLocation(floor_prog.vertexShader, "view");
	flr_uLoc_model    = shaderInstanceGetUniformLocation(floor_prog.vertexShader, "model");
	flr_uLoc_light_vp = shaderInstanceGetUniformLocation(floor_prog.vertexShader, "light_viewproj");

	// VBOs.
	buildIcosahedron();
	ico_vbo = linearAlloc(sizeof(ico_list)); memcpy(ico_vbo, ico_list, sizeof(ico_list));
	BufInfo_Init(&ico_buf);
	BufInfo_Add(&ico_buf, ico_vbo, sizeof(icovtx), 2, 0x10);

	floor_vbo = linearAlloc(sizeof(floor_list)); memcpy(floor_vbo, floor_list, sizeof(floor_list));
	BufInfo_Init(&floor_buf);
	BufInfo_Add(&floor_buf, floor_vbo, sizeof(flrvtx), 2, 0x10);

	// Lighting/shadow setup (for the floor).
	C3D_LightEnvInit(&lightEnv);
	C3D_LightEnvMaterial(&lightEnv, &mat_floor);
	C3D_LightInit(&light, &lightEnv);
	C3D_LightColor(&light, 1.0f, 0.96f, 0.88f);
	C3D_LightShadowEnable(&light, true);
	C3D_LightEnvShadowMode(&lightEnv, GPU_SHADOW_PRIMARY);
	C3D_LightEnvShadowSel(&lightEnv, 0);

	C3D_CullFace(GPU_CULL_NONE);   // shards + floor: both sides
}

// Advance animation + compute the per-frame matrices shared by both passes.
static void updateFrame(float ax, float ay, float bloom, float lightT)
{
	g_bloom = bloom;

	Mtx_Identity(&g_model);
	Mtx_RotateX(&g_model, ax, true);
	Mtx_RotateY(&g_model, ay, true);          // model = Rx*Ry (icosahedron spins at origin)

	Mtx_LookAt(&g_view, FVec3_New(0, 3.2f, 6.5f), FVec3_New(0, -0.3f, 0), FVec3_New(0, 1, 0), false);

	// Orbiting light.
	C3D_FVec lpos = FVec3_New(4.2f * cosf(lightT), 5.0f, 4.2f * sinf(lightT));
	Mtx_LookAt(&g_light_view, lpos, FVec3_New(0, 0, 0), FVec3_New(0, 1, 0), false);
	Mtx_Ortho(&g_light_proj, -3.5f, 3.5f, -3.5f, 3.5f, 1.0f, 13.0f, false);
	Mtx_Multiply(&g_light_vp, &g_light_proj, &g_light_view);

	// Lighting-unit light direction (world space, directional) = toward the light.
	C3D_FVec ldir = FVec4_New(lpos.x, lpos.y, lpos.z, 0.0f);
	C3D_LightPosition(&light, &ldir);

	// Icosahedron shading light in MODEL space = R^T * worldLightDir (the fur "headlight" trick),
	// so the flat shading tracks the same orbiting light that casts the shadow.
	C3D_Mtx rt;
	Mtx_Identity(&rt);
	Mtx_RotateY(&rt, -ay, true);
	Mtx_RotateX(&rt, -ax, true);              // rt = Ry(-ay)*Rx(-ax) = inverse model rotation
	g_lightdir_model = Mtx_MultiplyFVec4(&rt, FVec4_New(lpos.x, lpos.y, lpos.z, 0.0f));
}

// Pass 1: exploding icosahedron -> shadow map (from the light's POV).
static void drawShadowMap(void)
{
	C3D_BindProgram(&caster_prog);
	useIcoAttr();
	C3D_CullFace(GPU_CULL_NONE);
	C3D_ColorLogicOp(GPU_LOGICOP_COPY);   // disable alpha blending: the shadow map's depth rides in alpha

	// Match the stock caster: constant-white RGB, primary-colour (depth) alpha.
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvColor(env, 0xFFFFFFFF);
	C3D_TexEnvSrc(env, C3D_RGB,   GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(env, C3D_RGB,   GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);

	C3D_Mtx lightmvp;
	Mtx_Multiply(&lightmvp, &g_light_vp, &g_model);   // light_viewproj * model
	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, caster_uLoc_lightmvp, &lightmvp);
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, caster_uLoc_params, g_bloom, 0, 0, 0);

	C3D_SetBufInfo(&ico_buf);
	C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, VTX_COUNT);
}

// Pass 2: floor (receives shadow) + icosahedron (flat-shaded), for one eye.
static void sceneRender(float iod)
{
	C3D_Mtx proj;
	Mtx_PerspStereoTilt(&proj, C3D_AngleFromDegrees(45.0f), C3D_AspectRatioTop, 1.0f, 100.0f, iod, 6.5f, false);

	// --- Floor: stock receiver VS + lighting-unit shadow ---
	C3D_BindProgram(&floor_prog);
	useFloorAttr();

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_FRAGMENT_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	C3D_Mtx model_id; Mtx_Identity(&model_id);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, flr_uLoc_proj,     &proj);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, flr_uLoc_view,     &g_view);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, flr_uLoc_model,    &model_id);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, flr_uLoc_light_vp, &g_light_vp);

	C3D_TexShadowParams(false, bias);          // false = ortho light
	C3D_TexBind(0, &shadow_tex);
	C3D_TexSetFilter(&shadow_tex, filter, filter);
	C3D_LightEnvBind(&lightEnv);

	C3D_SetBufInfo(&floor_buf);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);

	// --- Icosahedron: its own flat shading (no shadow reception) ---
	C3D_BindProgram(&ico_prog);
	useIcoAttr();
	C3D_LightEnvBind(NULL);                     // icosahedron isn't lit by the unit

	env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	C3D_Mtx mvp;
	Mtx_Multiply(&mvp, &proj, &g_view);
	Mtx_Multiply(&mvp, &mvp, &g_model);         // proj * view * model
	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, ico_uLoc_proj, &mvp);
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, ico_uLoc_lightdir, g_lightdir_model.x, g_lightdir_model.y, g_lightdir_model.z, 0.0f);
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, ico_uLoc_params, g_bloom, 0, 0, 0);

	C3D_SetBufInfo(&ico_buf);
	C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, VTX_COUNT);
}

static void sceneExit(void)
{
	C3D_RenderTargetDelete(shadow_rt);
	C3D_TexDelete(&shadow_tex);
	linearFree(ico_vbo);
	linearFree(floor_vbo);
	shaderProgramFree(&caster_prog); DVLB_Free(caster_dvlb);
	shaderProgramFree(&ico_prog);    DVLB_Free(ico_dvlb);
	shaderProgramFree(&floor_prog);  DVLB_Free(floor_dvlb);
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

	printf("\x1b[2;2H=== Exploding Icosahedron ===");
	printf("\x1b[3;7Hcasting real shadows");
	printf("\x1b[6;2HControls:");
	printf("\x1b[7;4HA          pause");
	printf("\x1b[8;4HB          shadow filter");
	printf("\x1b[9;4HD-pad U/D  depth bias");
	printf("\x1b[10;4H3D slider  pop-out depth");
	printf("\x1b[11;4HSTART      quit");

	float ax = 0.0f, ay = 0.0f, tt = 0.0f, lightT = 0.0f;
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
		if (kDown & KEY_B) filter = (filter == GPU_NEAREST) ? GPU_LINEAR : GPU_NEAREST;
		if (kHeld & KEY_DUP)   bias += 0.001f;
		if (kHeld & KEY_DDOWN) bias -= 0.001f;
		if (bias < 0.0f)  bias = 0.0f;
		if (bias > 0.05f) bias = 0.05f;

		if (!paused) { ax += 0.011f; ay += 0.017f; tt += 0.028f; lightT += 0.006f; }
		float bloom = 0.5f + 0.5f * sinf(tt);   // closed (0) <-> blown apart (1)

		updateFrame(ax, ay, bloom, lightT);

		frames++;
		u64 now = osGetTime();
		if (now - lastT >= 500) { fps = frames * 1000.0f / (float)(now - lastT); frames = 0; lastT = now; }

		printf("\x1b[14;2HBloom  : %.2f   ", bloom);
		printf("\x1b[15;2HBias   : %.3f   ", bias);
		printf("\x1b[16;2HFilter : %-7s ", filter == GPU_LINEAR ? "LINEAR" : "NEAREST");
		printf("\x1b[30;1HFPS: %.1f  ", fps);

		float slider = osGet3DSliderState();
		float iod = slider / 3.0f;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			// Pass 1: shadow map (shared by both eyes).
			C3D_RenderTargetClear(shadow_rt, C3D_CLEAR_ALL, 0xFFFFFFFF, 0);
			C3D_FrameDrawOn(shadow_rt);
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
