// citro3d GEOMETRY-SHADER showcase: a furry ball.
//
// A subdivided sphere is the body; the geometry shader makes every triangle
// sprout a tapered "hair" along its face normal (1 input triangle -> 2 output
// triangles: the surface + the hair). The hairs bristle (length pulses) and can
// sway in the wind. Emitting extra geometry per primitive is geometry-shader-only.
//
// Controls (bottom screen):
//   D-pad up/down = fur length    B = toggle wind    START = quit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "program_shbin.h"

#define CLEAR_COLOR 0x0A0A14FF

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float position[3]; float color[4]; } vertex;

// No fixed cap: the VBO is (re)allocated to fit the current subdivision and grows
// until linearAlloc runs out (then it simply stops climbing). Limits are linear
// memory and — long before that — geometry-shader throughput.
#define MIN_SUBDIV 1

static int     vtx_count = 0;
static int     subdiv = 7;                       // current subdivision (= fur density)
static vertex* build_dst;                        // where push() writes during a rebuild

static DVLB_s* program_dvlb;
static shaderProgram_s program;
static int uLoc_projection, uLoc_params, uLoc_wind;
static C3D_Mtx projection;
static void* vbo_data = NULL;                    // current GPU VBO (linear heap)
static void* vbo_prev = NULL;                    // previous VBO, freed a rebuild later (GPU safety)

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

static void facePoint(int axis, float sign, float u, float v, float out[3])
{
	switch (axis) {
		case 0: out[0]=sign; out[1]=u;    out[2]=v;    break;
		case 1: out[0]=u;    out[1]=sign; out[2]=v;    break;
		default:out[0]=u;    out[1]=v;    out[2]=sign; break;
	}
}

static void push(const float p[3])
{
	float inv = 1.0f / sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
	float x = p[0]*inv, y = p[1]*inv, z = p[2]*inv;   // project onto unit sphere
	float hue = atan2f(z, x) / (2.0f * M_PI) + 0.5f;   // vivid rainbow by longitude
	float r, g, b;
	hsv(hue, 1.0f, 1.0f, &r, &g, &b);
	build_dst[vtx_count++] = (vertex){ { x, y, z }, { r, g, b, 1.0f } };
}

static void buildFurBall(void)
{
	vtx_count = 0;
	const float faces[6][2] = { {0,+1},{0,-1},{1,+1},{1,-1},{2,+1},{2,-1} };
	for (int fi = 0; fi < 6; ++fi) {
		int axis = (int)faces[fi][0];
		float sign = faces[fi][1];
		for (int j = 0; j < subdiv; ++j) {
			for (int i = 0; i < subdiv; ++i) {
				float u0 = -1.0f + 2.0f*i/subdiv,     u1 = -1.0f + 2.0f*(i+1)/subdiv;
				float v0 = -1.0f + 2.0f*j/subdiv,     v1 = -1.0f + 2.0f*(j+1)/subdiv;
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

// Rebuild the sphere at the current `subdiv` into a freshly-sized linear VBO and
// point the GPU at it. Returns false if linear memory is exhausted (caller reverts).
// The old buffer is freed one rebuild later, by which time the GPU is long done.
static bool rebuildGeometry(void)
{
	int needed = 6 * subdiv * subdiv * 6;
	size_t bytes = (size_t)needed * sizeof(vertex);

	vertex* nb = (vertex*)linearAlloc(bytes);
	if (!nb)
		return false;                 // out of linear memory — keep the current sphere

	build_dst = nb;
	buildFurBall();                   // fills nb, sets vtx_count
	GSPGPU_FlushDataCache(nb, bytes); // CPU writes -> visible to the GPU

	if (vbo_prev) linearFree(vbo_prev); // free the buffer from two rebuilds ago (GPU idle)
	vbo_prev = vbo_data;               // defer freeing the just-active buffer
	vbo_data = nb;

	// Repoint the vertex buffer at the new allocation.
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo_data, sizeof(vertex), 2, 0x10);
	return true;
}

static void sceneInit(void)
{
	program_dvlb = DVLB_ParseFile((u32*)program_shbin, program_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &program_dvlb->DVLE[0]);
	shaderProgramSetGsh(&program, &program_dvlb->DVLE[1], 6); // one triangle per invocation
	C3D_BindProgram(&program);

	uLoc_projection = shaderInstanceGetUniformLocation(program.geometryShader, "projection");
	uLoc_params     = shaderInstanceGetUniformLocation(program.geometryShader, "params");
	uLoc_wind       = shaderInstanceGetUniformLocation(program.geometryShader, "wind");

	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0 = position
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 4); // v1 = colour

	rebuildGeometry();   // allocates the VBO at the current subdivision and sets BufInfo

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL); // opaque fur, real depth sorting
	C3D_CullFace(GPU_CULL_NONE);
}

static void sceneRender(float ax, float ay, float furLen, float windX, float windZ)
{
	C3D_Mtx modelView;
	Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(45.0f), C3D_AspectRatioTop, 0.01f, 1000.0f, false);
	Mtx_Identity(&modelView);
	Mtx_Translate(&modelView, 0.0f, 0.0f, -3.6f, true);
	Mtx_RotateX(&modelView, ax, true);
	Mtx_RotateY(&modelView, ay, true);

	C3D_Mtx mvp;
	Mtx_Multiply(&mvp, &projection, &modelView);

	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, uLoc_projection, &mvp);
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, uLoc_params, furLen, 0.0f, 0.0f, 0.0f);
	C3D_FVUnifSet(GPU_GEOMETRY_SHADER, uLoc_wind, windX, 0.0f, windZ, 0.0f);

	C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, vtx_count);
}

static void sceneExit(void)
{
	if (vbo_data) linearFree(vbo_data);
	if (vbo_prev) linearFree(vbo_prev);
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

	printf("\x1b[2;3H=== Furry Ball (geoshader) ===");
	printf("\x1b[3;5Heach triangle sprouts a hair");
	printf("\x1b[6;2HControls:");
	printf("\x1b[7;4HD-pad U/D   fur length");
	printf("\x1b[8;4HD-pad L/R   fur amount (subdiv)");
	printf("\x1b[9;4HB           toggle wind");
	printf("\x1b[10;4HSTART       quit");

	float ax = 0.0f, ay = 0.0f, tp = 0.0f, tw = 0.0f;
	float furBase = 0.25f;     // D-pad adjustable
	bool  windOn = false;

	while (aptMainLoop())
	{
		hidScanInput();
		u32 kDown = hidKeysDown();
		u32 kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;
		if (kDown & KEY_B)      windOn = !windOn;
		if (kHeld & KEY_DUP)    furBase += 0.004f;
		if (kHeld & KEY_DDOWN)  furBase -= 0.004f;
		if (furBase < 0.0f)  furBase = 0.0f;
		if (furBase > 0.6f)  furBase = 0.6f;

		// D-pad left/right: fewer/more subdivisions -> fewer/more hairs (rebuild the sphere).
		// No upper cap: if the VBO no longer fits in linear memory, revert the step.
		if (kDown & KEY_DRIGHT) { subdiv++; if (!rebuildGeometry()) subdiv--; }
		if ((kDown & KEY_DLEFT) && subdiv > MIN_SUBDIV) { subdiv--; rebuildGeometry(); }

		ax += 0.008f;
		ay += 0.013f;
		tp += 0.040f;
		tw += 0.030f;

		float furLen = furBase + 0.04f * sinf(tp);          // gentle breathing
		float windX = windOn ? 0.13f * sinf(tw)        : 0.0f;
		float windZ = windOn ? 0.10f * sinf(tw * 0.8f) : 0.0f;

		printf("\x1b[13;2HFur length : %.2f  ", furLen);
		printf("\x1b[14;2HSubdiv     : %2d  (%d hairs)   ", subdiv, vtx_count / 3);
		printf("\x1b[15;2HWind       : %-3s", windOn ? "ON" : "OFF");

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(target);
			sceneRender(ax, ay, furLen, windX, windZ);
		C3D_FrameEnd(0);
	}

	sceneExit();
	C3D_Fini();
	gfxExit();
	return 0;
}
