// citro3d — SELF-SHADOWING DEBRIS FIELD (stereoscopic).
//
// A tight cluster of icosahedron shards, each tumbling independently around its
// own centroid. Because the shards stay clustered and constantly reorient, they
// keep crossing between the light and one another — so they cast clear, moving
// shadows ONTO EACH OTHER (self-shadowing), plus onto the floor. This is the
// configuration that actually shows off self-shadowing, which the radially-
// exploding version (citro3d-shadow-explode-stereo) couldn't (a convex solid
// flying apart barely self-occludes).
//
// How it's driven:
//   * The CPU does the per-shard tumble each frame (Rodrigues rotation around
//     each shard's centroid — the PICA shader has no sin/cos) and writes the
//     positions into a dynamic VBO (linearAlloc + GSPGPU_FlushDataCache).
//   * The SAME geometry-shader shadow pipeline as the explode demo is reused
//     verbatim, with bloom = 0 (no GS displacement): the caster GS renders the
//     shards' depth from the light, and the receiver GS lights them through the
//     fragment-lighting unit and samples the shadow map -> self-shadows. The
//     floor (stock receiver VS) receives shadows too.
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

typedef struct { float position[3]; float color[4];  } icovtx;
typedef struct { float position[3]; float normal[3]; } flrvtx;

#define N_FACES   20
#define VTX_COUNT (N_FACES * 3)
#define FLOOR_Y   (-2.6f)
#define FLOOR_E    4.0f

// Per-shard precomputed data (base triangle, centroid, tumble axis/speed, colour).
static float shard_v[N_FACES][3][3];   // base vertex positions (unit sphere)
static float shard_c[N_FACES][3];      // centroid
static float shard_ax[N_FACES][3];     // tumble axis (unit)
static float shard_w[N_FACES];         // angular speed
static float shard_col[N_FACES][4];    // colour

static const flrvtx floor_list[6] =
{
	{ {-FLOOR_E, FLOOR_Y, -FLOOR_E}, {0,1,0} },
	{ { FLOOR_E, FLOOR_Y, -FLOOR_E}, {0,1,0} },
	{ {-FLOOR_E, FLOOR_Y,  FLOOR_E}, {0,1,0} },
	{ { FLOOR_E, FLOOR_Y, -FLOOR_E}, {0,1,0} },
	{ { FLOOR_E, FLOOR_Y,  FLOOR_E}, {0,1,0} },
	{ {-FLOOR_E, FLOOR_Y,  FLOOR_E}, {0,1,0} },
};

static C3D_RenderTarget* shadow_rt;
static C3D_Tex           shadow_tex;
static int   filter;
static float bias;

static DVLB_s* caster_dvlb;   static shaderProgram_s caster_prog;
static int caster_uLoc_lightmvp, caster_uLoc_params;
static DVLB_s* ico_dvlb;      static shaderProgram_s ico_prog;
static int ico_uLoc_model, ico_uLoc_view, ico_uLoc_proj, ico_uLoc_lightvp, ico_uLoc_params;
static DVLB_s* floor_dvlb;    static shaderProgram_s floor_prog;
static int flr_uLoc_proj, flr_uLoc_view, flr_uLoc_model, flr_uLoc_light_vp;

static C3D_LightEnv lightEnv;
static C3D_Light    light;

static C3D_BufInfo ico_buf, floor_buf;
static void *ico_vbo, *floor_vbo;     // ico_vbo is dynamic (rewritten each frame)

static C3D_Mtx g_model, g_view, g_light_view, g_light_proj, g_light_vp;

static const C3D_Material mat_floor =
{
	{ 0.13f, 0.14f, 0.18f }, { 0.40f, 0.44f, 0.54f },
	{ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
};
static const C3D_Material mat_ico =
{
	{ 0.22f, 0.22f, 0.22f }, { 0.95f, 0.95f, 0.95f },
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

// Rotate v around unit axis a by angle (Rodrigues).
static void rotAxis(float out[3], const float a[3], float ang, const float v[3])
{
	float c = cosf(ang), s = sinf(ang);
	float d = a[0]*v[0] + a[1]*v[1] + a[2]*v[2];          // a·v
	float cx = a[1]*v[2] - a[2]*v[1];                      // a×v
	float cy = a[2]*v[0] - a[0]*v[2];
	float cz = a[0]*v[1] - a[1]*v[0];
	out[0] = v[0]*c + cx*s + a[0]*d*(1.0f - c);
	out[1] = v[1]*c + cy*s + a[1]*d*(1.0f - c);
	out[2] = v[2]*c + cz*s + a[2]*d*(1.0f - c);
}

static void buildDebris(void)
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
		hsv(frand(), 1.0f, 1.0f, &r, &g, &b);
		shard_col[f][0]=r; shard_col[f][1]=g; shard_col[f][2]=b; shard_col[f][3]=1.0f;

		shard_c[f][0]=shard_c[f][1]=shard_c[f][2]=0.0f;
		for (int k = 0; k < 3; ++k) {
			const float* p = V[F[f][k]];
			float inv = 1.0f / sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
			shard_v[f][k][0]=p[0]*inv; shard_v[f][k][1]=p[1]*inv; shard_v[f][k][2]=p[2]*inv;
			shard_c[f][0]+=shard_v[f][k][0]; shard_c[f][1]+=shard_v[f][k][1]; shard_c[f][2]+=shard_v[f][k][2];
		}
		shard_c[f][0]/=3.0f; shard_c[f][1]/=3.0f; shard_c[f][2]/=3.0f;

		// random tumble axis (unit) + signed speed
		float ax, ay, az, len;
		do { ax=2*frand()-1; ay=2*frand()-1; az=2*frand()-1; len=ax*ax+ay*ay+az*az; } while (len < 0.01f);
		len = 1.0f/sqrtf(len);
		shard_ax[f][0]=ax*len; shard_ax[f][1]=ay*len; shard_ax[f][2]=az*len;
		shard_w[f] = (0.5f + 1.2f*frand()) * (frand() < 0.5f ? -1.0f : 1.0f);
	}
}

// Rewrite the dynamic VBO: each shard tumbled around its centroid, centroids
// pushed out by `push` so the cluster is spaced enough to cast shadows yet tight
// enough to keep overlapping.
static void updateDebris(float t, float push)
{
	icovtx* dst = (icovtx*)ico_vbo;
	for (int f = 0; f < N_FACES; ++f) {
		float ang = shard_w[f] * t;
		for (int k = 0; k < 3; ++k) {
			float rel[3] = {
				shard_v[f][k][0] - shard_c[f][0],
				shard_v[f][k][1] - shard_c[f][1],
				shard_v[f][k][2] - shard_c[f][2],
			};
			float rot[3];
			rotAxis(rot, shard_ax[f], ang, rel);
			dst->position[0] = shard_c[f][0]*push + rot[0];
			dst->position[1] = shard_c[f][1]*push + rot[1];
			dst->position[2] = shard_c[f][2]*push + rot[2];
			dst->color[0]=shard_col[f][0]; dst->color[1]=shard_col[f][1];
			dst->color[2]=shard_col[f][2]; dst->color[3]=shard_col[f][3];
			++dst;
		}
	}
	GSPGPU_FlushDataCache(ico_vbo, VTX_COUNT * sizeof(icovtx));
}

static void useIcoAttr(void)
{
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 4);
}
static void useFloorAttr(void)
{
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 3);
}

static void sceneInit(void)
{
	filter = GPU_LINEAR;
	bias   = 0.006f;

	C3D_TexInitShadow(&shadow_tex, 512, 512);
	shadow_rt = C3D_RenderTargetCreateFromTex(&shadow_tex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH16);
	shadow_tex.border = 0xFFFFFFFF;
	C3D_TexSetWrap(&shadow_tex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);

	caster_dvlb = DVLB_ParseFile((u32*)caster_shbin, caster_shbin_size);
	shaderProgramInit(&caster_prog);
	shaderProgramSetVsh(&caster_prog, &caster_dvlb->DVLE[0]);
	shaderProgramSetGsh(&caster_prog, &caster_dvlb->DVLE[1], 6);
	caster_uLoc_lightmvp = shaderInstanceGetUniformLocation(caster_prog.geometryShader, "lightmvp");
	caster_uLoc_params   = shaderInstanceGetUniformLocation(caster_prog.geometryShader, "params");

	ico_dvlb = DVLB_ParseFile((u32*)ico_shbin, ico_shbin_size);
	shaderProgramInit(&ico_prog);
	shaderProgramSetVsh(&ico_prog, &ico_dvlb->DVLE[0]);
	shaderProgramSetGsh(&ico_prog, &ico_dvlb->DVLE[1], 6);
	ico_uLoc_model  = shaderInstanceGetUniformLocation(ico_prog.geometryShader, "model");
	ico_uLoc_view   = shaderInstanceGetUniformLocation(ico_prog.geometryShader, "view");
	ico_uLoc_proj   = shaderInstanceGetUniformLocation(ico_prog.geometryShader, "proj");
	ico_uLoc_lightvp= shaderInstanceGetUniformLocation(ico_prog.geometryShader, "light_viewproj");
	ico_uLoc_params = shaderInstanceGetUniformLocation(ico_prog.geometryShader, "params");

	floor_dvlb = DVLB_ParseFile((u32*)shadow_receiver_shbin, shadow_receiver_shbin_size);
	shaderProgramInit(&floor_prog);
	shaderProgramSetVsh(&floor_prog, &floor_dvlb->DVLE[0]);
	flr_uLoc_proj     = shaderInstanceGetUniformLocation(floor_prog.vertexShader, "proj");
	flr_uLoc_view     = shaderInstanceGetUniformLocation(floor_prog.vertexShader, "view");
	flr_uLoc_model    = shaderInstanceGetUniformLocation(floor_prog.vertexShader, "model");
	flr_uLoc_light_vp = shaderInstanceGetUniformLocation(floor_prog.vertexShader, "light_viewproj");

	buildDebris();
	ico_vbo = linearAlloc(VTX_COUNT * sizeof(icovtx));   // dynamic; filled each frame
	BufInfo_Init(&ico_buf);
	BufInfo_Add(&ico_buf, ico_vbo, sizeof(icovtx), 2, 0x10);

	floor_vbo = linearAlloc(sizeof(floor_list)); memcpy(floor_vbo, floor_list, sizeof(floor_list));
	BufInfo_Init(&floor_buf);
	BufInfo_Add(&floor_buf, floor_vbo, sizeof(flrvtx), 2, 0x10);

	C3D_LightEnvInit(&lightEnv);
	C3D_LightEnvMaterial(&lightEnv, &mat_floor);
	C3D_LightInit(&light, &lightEnv);
	C3D_LightColor(&light, 1.0f, 0.96f, 0.88f);
	C3D_LightShadowEnable(&light, true);
	C3D_LightEnvShadowMode(&lightEnv, GPU_SHADOW_PRIMARY);
	C3D_LightEnvShadowSel(&lightEnv, 0);

	C3D_CullFace(GPU_CULL_NONE);
}

static void updateFrame(float ax, float ay, float lightT)
{
	Mtx_Identity(&g_model);
	Mtx_RotateX(&g_model, ax, true);
	Mtx_RotateY(&g_model, ay, true);

	Mtx_LookAt(&g_view, FVec3_New(0, 3.0f, 6.6f), FVec3_New(0, -0.2f, 0), FVec3_New(0, 1, 0), false);

	// Keep the light in the upper-FRONT arc (camera side) instead of a full orbit,
	// so the shards we're looking at stay lit and can actually receive shadows.
	float az = 1.15f * sinf(lightT);                 // azimuth sways ~±66 deg around +z (front)
	C3D_FVec lpos = FVec3_New(4.3f * sinf(az), 4.4f, 4.3f * cosf(az));
	Mtx_LookAt(&g_light_view, lpos, FVec3_New(0, 0, 0), FVec3_New(0, 1, 0), false);
	Mtx_Ortho(&g_light_proj, -3.3f, 3.3f, -3.3f, 3.3f, 1.0f, 13.0f, false);
	Mtx_Multiply(&g_light_vp, &g_light_proj, &g_light_view);

	C3D_FVec ldir = FVec4_New(lpos.x, lpos.y, lpos.z, 0.0f);
	C3D_LightPosition(&light, &ldir);
}

// Pass 1: debris -> shadow map (bloom = 0, geometry already tumbled on the CPU).
static void drawShadowMap(void)
{
	C3D_BindProgram(&caster_prog);
	useIcoAttr();
	C3D_CullFace(GPU_CULL_NONE);
	C3D_ColorLogicOp(GPU_LOGICOP_COPY);

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvColor(env, 0xFFFFFFFF);
	C3D_TexEnvSrc(env, C3D_RGB,   GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(env, C3D_RGB,   GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);

	C3D_Mtx lightmvp;
	Mtx_Multiply(&lightmvp, &g_light_vp, &g_model);
	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, caster_uLoc_lightmvp, &lightmvp);
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, caster_uLoc_params, 0.0f, 0, 0, 0);   // bloom = 0

	C3D_SetBufInfo(&ico_buf);
	C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, VTX_COUNT);
}

static void sceneRender(float iod)
{
	C3D_Mtx proj;
	Mtx_PerspStereoTilt(&proj, C3D_AngleFromDegrees(45.0f), C3D_AspectRatioTop, 1.0f, 100.0f, iod, 6.6f, false);

	// --- Floor (receives shadow) ---
	C3D_BindProgram(&floor_prog);
	useFloorAttr();
	C3D_LightEnvMaterial(&lightEnv, &mat_floor);

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_FRAGMENT_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	C3D_Mtx model_id; Mtx_Identity(&model_id);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, flr_uLoc_proj,     &proj);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, flr_uLoc_view,     &g_view);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, flr_uLoc_model,    &model_id);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, flr_uLoc_light_vp, &g_light_vp);

	C3D_TexShadowParams(false, bias);
	C3D_TexBind(0, &shadow_tex);
	C3D_TexSetFilter(&shadow_tex, filter, filter);
	C3D_LightEnvBind(&lightEnv);

	C3D_SetBufInfo(&floor_buf);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);

	// --- Debris: lit + SELF-SHADOWING receiver ---
	C3D_BindProgram(&ico_prog);
	useIcoAttr();
	C3D_LightEnvMaterial(&lightEnv, &mat_ico);

	env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB,   GPU_FRAGMENT_PRIMARY_COLOR, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_RGB,   GPU_MODULATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);

	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, ico_uLoc_model,   &g_model);
	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, ico_uLoc_view,    &g_view);
	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, ico_uLoc_proj,    &proj);
	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, ico_uLoc_lightvp, &g_light_vp);
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, ico_uLoc_params, 0.0f, 0, 0, 0);     // bloom = 0

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

	printf("\x1b[2;2H=== Self-Shadowing Debris ===");
	printf("\x1b[3;5Htumbling shards shadow themselves");
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

		if (!paused) { ax += 0.004f; ay += 0.008f; tt += 1.0f / 60.0f; lightT += 0.006f; }

		// Gentle breathing of the cluster spacing (opens/closes -> varies occlusion).
		float push = 1.30f + 0.18f * sinf(tt * 0.6f);
		updateDebris(tt, push);

		updateFrame(ax, ay, lightT);

		frames++;
		u64 now = osGetTime();
		if (now - lastT >= 500) { fps = frames * 1000.0f / (float)(now - lastT); frames = 0; lastT = now; }

		printf("\x1b[14;2HBias   : %.3f   ", bias);
		printf("\x1b[15;2HFilter : %-7s ", filter == GPU_LINEAR ? "LINEAR" : "NEAREST");
		printf("\x1b[30;1HFPS: %.1f  ", fps);

		float slider = osGet3DSliderState();
		float iod = slider / 3.0f;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(shadow_rt, C3D_CLEAR_ALL, 0xFFFFFFFF, 0);
			C3D_FrameDrawOn(shadow_rt);
			drawShadowMap();

			C3D_RenderTargetClear(targetLeft, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(targetLeft);
			sceneRender(-iod);

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
