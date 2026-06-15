// NeHe Lesson 8 (blending / transparency) ported to the 3DS via picaGL.
//
// Original: https://nehe.gamedev.net/tutorial/blending/16001/ — the Lesson 7 lit, textured,
// keyboard-controlled cube plus toggleable transparency. picaGL supports blending natively
// (glEnable(GL_BLEND), glBlendFunc, glDisable(GL_DEPTH_TEST) are all real), so this is a faithful
// port — only the same picaGL adaptations carried over from Lessons 6/7 apply:
//
//   * GL_TRIANGLES, not GL_QUADS (picaGL has no quads).
//   * Lighting is CPU-baked into the vertex COLOUR array each frame (picaGL has no GL lighting);
//     the colour buffer lives in linearAlloc so picaGL re-reads it live.
//   * Present TOP only (pglSwapBuffersEx(true,false)) + single-buffer the bottom, so the console HUD
//     on the bottom screen stays stable.
//
// Blending: NeHe uses additive glBlendFunc(GL_SRC_ALPHA, GL_ONE) with the depth test OFF and alpha
// 0.5, so the back faces glow through the front ones — the classic glass-cube look. We drive the
// per-fragment alpha through the vertex colour's alpha channel (tex-env MODULATE => texture.a * a).
//
// Controls: A = lighting, B = blending, X = filter (NEAREST/LINEAR); D-pad = spin speed;
//           L/R shoulders = move in/out; START = exit.

#include <3ds.h>
#include <GL/picaGL.h>
#include <stdio.h>
#include <math.h>

extern void gluPerspective(float fovy, float aspect, float near, float far);

#define TEX_SIZE 128
#define NVERTS   36
#define DEG2RAD  (3.14159265f / 180.0f)

static const GLfloat face_pos[6][4][3] = {
	{ {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1} }, // front  (+z)
	{ { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1} }, // back   (-z)
	{ {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1} }, // top    (+y)
	{ {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}, {-1,-1, 1} }, // bottom (-y)
	{ { 1,-1, 1}, { 1, 1, 1}, { 1, 1,-1}, { 1,-1,-1} }, // right  (+x)
	{ {-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1} }, // left   (-x)
};
static const GLfloat face_normal[6][3] = {
	{0,0,1}, {0,0,-1}, {0,1,0}, {0,-1,0}, {1,0,0}, {-1,0,0},
};
static const GLfloat quad_uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
static const int idx[6] = { 0, 1, 2, 0, 2, 3 };

static GLfloat cube_verts[NVERTS * 3];
static GLfloat cube_uvs[NVERTS * 2];
static int     vert_face[NVERTS];
static GLfloat *cube_colors = NULL;          // linearAlloc: RGB = lighting, A = transparency
static u8      texture_data[TEX_SIZE * TEX_SIZE * 4];

static void build_cube(void)
{
	int v = 0;
	for (int f = 0; f < 6; f++)
		for (int i = 0; i < 6; i++) {
			int c = idx[i];
			cube_verts[v * 3 + 0] = face_pos[f][c][0];
			cube_verts[v * 3 + 1] = face_pos[f][c][1];
			cube_verts[v * 3 + 2] = face_pos[f][c][2];
			cube_uvs[v * 2 + 0] = quad_uv[c][0];
			cube_uvs[v * 2 + 1] = quad_uv[c][1];
			vert_face[v] = f;
			v++;
		}
}

static void put(int x, int y, u8 r, u8 g, u8 b)
{
	u8 *p = &texture_data[(y * TEX_SIZE + x) * 4];
	p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
}

static void build_texture(void)
{
	for (int y = 0; y < TEX_SIZE; y++)
		for (int x = 0; x < TEX_SIZE; x++) {
			int checker = ((x >> 4) ^ (y >> 4)) & 1;
			u8 r = checker ? 198 : 120, g = checker ? 140 : 78, b = checker ? 74 : 40;
			if ((x % 32) == 0 || (y % 32) == 0) { r = 60; g = 40; b = 20; }
			if (x < 3 || x >= TEX_SIZE - 3 || y < 3 || y >= TEX_SIZE - 3) { r = 20; g = 12; b = 6; }
			put(x, y, r, g, b);
		}
}

// RGB = ambient+diffuse brightness (lit) or white (unlit); A = alpha (0.5 transparent / 1.0 opaque).
static void update_colors(float xrot, float yrot, int lit, float alpha)
{
	float face_b[6];
	if (lit) {
		float cx = cosf(xrot * DEG2RAD), sx = sinf(xrot * DEG2RAD);
		float cy = cosf(yrot * DEG2RAD), sy = sinf(yrot * DEG2RAD);
		for (int f = 0; f < 6; f++) {
			float nx = face_normal[f][0], ny = face_normal[f][1], nz = face_normal[f][2];
			float ay = ny, az = -nx * sy + nz * cy;     // normal -> eye space (Ry then Rx)
			float bz = ay * sx + az * cx;                // dot with light (0,0,1)
			float bb = 0.2f + 0.8f * (bz > 0.0f ? bz : 0.0f);
			face_b[f] = bb > 1.0f ? 1.0f : bb;
		}
	} else {
		for (int f = 0; f < 6; f++) face_b[f] = 1.0f;
	}
	for (int v = 0; v < NVERTS; v++) {
		float b = face_b[vert_face[v]];
		cube_colors[v * 4 + 0] = b;
		cube_colors[v * 4 + 1] = b;
		cube_colors[v * 4 + 2] = b;
		cube_colors[v * 4 + 3] = alpha;
	}
	GSPGPU_FlushDataCache(cube_colors, NVERTS * 4 * sizeof(GLfloat));
}

int main(int argc, char **argv)
{
	gfxInitDefault();
	// Present TOP only below; single-buffer the bottom so the console HUD is stable (see landscape §9).
	gfxSetDoubleBuffering(GFX_BOTTOM, false);
	consoleInit(GFX_BOTTOM, NULL);

	pglInit();
	pglSelectScreen(GFX_TOP, GFX_LEFT);

	cube_colors = (GLfloat *) linearAlloc(NVERTS * 4 * sizeof(GLfloat));
	build_cube();
	build_texture();

	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_SIZE, TEX_SIZE, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, texture_data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glViewport(0, 0, 400, 240);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(45.0f, 400.0f / 240.0f, 0.1f, 100.0f);
	glMatrixMode(GL_MODELVIEW);

	glDisable(GL_CULL_FACE);
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);  // texture * vertex colour (incl. alpha)
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);                            // NeHe's additive "glass" blend

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, cube_verts);
	glColorPointer(4, GL_FLOAT, 0, cube_colors);
	glTexCoordPointer(2, GL_FLOAT, 0, cube_uvs);

	float xrot = 0.0f, yrot = 0.0f, xspeed = 0.4f, yspeed = 0.3f, z = -5.0f;
	int lighting = 1, linear = 1, blending = 0;

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown();
		u32 kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;
		if (kDown & KEY_A) lighting = !lighting;
		if (kDown & KEY_B) blending = !blending;
		if (kDown & KEY_X) {
			linear = !linear;
			GLint f = linear ? GL_LINEAR : GL_NEAREST;
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
		}
		if (kHeld & KEY_DUP)    xspeed -= 0.05f;
		if (kHeld & KEY_DDOWN)  xspeed += 0.05f;
		if (kHeld & KEY_DLEFT)  yspeed -= 0.05f;
		if (kHeld & KEY_DRIGHT) yspeed += 0.05f;
		if (kHeld & KEY_L)      z -= 0.1f;
		if (kHeld & KEY_R)      z += 0.1f;

		// NeHe: when blending, enable additive blend and turn the depth test OFF (so back faces show
		// through); when opaque, depth test on and full alpha.
		if (blending) {
			glEnable(GL_BLEND);
			glDisable(GL_DEPTH_TEST);
		} else {
			glDisable(GL_BLEND);
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_LEQUAL);
		}

		printf("\x1b[2;14HNeHe Lesson 8");
		printf("\x1b[3;13HBlending");
		printf("\x1b[7;10HLighting : %-7s", lighting ? "ON" : "OFF");
		printf("\x1b[8;10HBlending : %-7s", blending ? "ON" : "OFF");
		printf("\x1b[9;10HFilter   : %-7s", linear ? "LINEAR" : "NEAREST");
		printf("\x1b[10;10HX-speed  : %5.2f", xspeed);
		printf("\x1b[11;10HY-speed  : %5.2f", yspeed);
		printf("\x1b[12;10HDepth    : %5.1f", z);
		printf("\x1b[25;3HA: light  B: blend  X: filter");
		printf("\x1b[26;3HD-pad: spin  L/R: move  START: exit");

		update_colors(xrot, yrot, lighting, blending ? 0.5f : 1.0f);

		glClearColor(0.0f, 0.0f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		glLoadIdentity();
		glTranslatef(0.0f, 0.0f, z);
		glRotatef(xrot, 1.0f, 0.0f, 0.0f);
		glRotatef(yrot, 0.0f, 1.0f, 0.0f);

		glDrawArrays(GL_TRIANGLES, 0, NVERTS);

		pglSwapBuffersEx(true, false);   // top only; console owns the bottom
		gspWaitForVBlank();

		xrot += xspeed;
		yrot += yspeed;
	}

	glDeleteTextures(1, &tex);
	linearFree(cube_colors);
	pglExit();
	gfxExit();
	return 0;
}
