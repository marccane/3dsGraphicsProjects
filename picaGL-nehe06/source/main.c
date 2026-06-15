// NeHe Lesson 6 (texture mapping) ported to the 3DS via picaGL.
//
// Original: https://nehe.gamedev.net/tutorial/texture_mapping/12038/ — a spinning
// textured cube. Two deliberate changes were forced by what picaGL actually supports
// (see 8-opengl/3ds-opengl-landscape.md §4):
//
//   1. NeHe draws the cube with glBegin(GL_QUADS). picaGL has NO GL_QUADS — a quad
//      mode silently falls through to a single TRIANGLE_FAN, so a 24-vertex multi-quad
//      block renders garbage. We build the cube from GL_TRIANGLES (36 verts) instead.
//   2. NeHe loads NeHe.bmp from disk. We generate a 128x128 checkerboard texture at
//      runtime so the demo is self-contained (and 128 is power-of-two, which the PICA
//      requires). picaGL's glTexImage2D does the Morton tiling for us.
//
// Everything else is faithful fixed-function GL ES 1.1: matrix stack, gluPerspective,
// depth test, GL_TEXTURE_2D, glTexCoordPointer/glVertexPointer, glRotatef spin.
//
// Build: `make` (links -lpicaGL -lcitro3d -lctru). Run the resulting .3dsx via the
// Homebrew Launcher or `3dslink`.

#include <3ds.h>
#include <GL/picaGL.h>
#include <stdio.h>
#include <string.h>

// picaGL implements gluPerspective (source/glu.c) but does not declare it in <GL/gl.h>.
extern void gluPerspective(float fovy, float aspect, float near, float far);

#define TEX_SIZE 128

// ---- the 6 cube faces as quads (4 corners each), CCW; expanded to triangles below ----
// Cube spans -1..+1. Texcoords map the whole texture to every face.
static const GLfloat face_pos[6][4][3] = {
	{ {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1} }, // front  (+z)
	{ { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1} }, // back   (-z)
	{ {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1} }, // top    (+y)
	{ {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}, {-1,-1, 1} }, // bottom (-y)
	{ { 1,-1, 1}, { 1, 1, 1}, { 1, 1,-1}, { 1,-1,-1} }, // right  (+x)
	{ {-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1} }, // left   (-x)
};
static const GLfloat quad_uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };

// Expanded interleaved-free arrays: 6 faces * 2 tris * 3 verts = 36 vertices.
static GLfloat cube_verts[36 * 3];
static GLfloat cube_uvs[36 * 2];
// All-white per-vertex colours. picaGL maps v0=position, v1=colour, v2=texcoord with a fixed
// 0x3210 attribute permutation; the only config proven working on this HW (picaGL-test) supplied
// vertex+colour contiguously. Skipping colour leaves a gap at v1 that mis-routes the texcoord
// attribute -> scrambled texture. So we supply colour explicitly (white -> MODULATE = pure texel).
static GLfloat cube_colors[36 * 4];

// Runtime-generated texture (RGBA8, linear; picaGL tiles it).
static u8 texture_data[TEX_SIZE * TEX_SIZE * 4];

static void build_cube(void)
{
	// Each quad ABCD -> triangles (A,B,C) and (A,C,D).
	static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
	int v = 0;
	for (int f = 0; f < 6; f++) {
		for (int i = 0; i < 6; i++) {
			int c = idx[i];
			cube_verts[v * 3 + 0] = face_pos[f][c][0];
			cube_verts[v * 3 + 1] = face_pos[f][c][1];
			cube_verts[v * 3 + 2] = face_pos[f][c][2];
			cube_uvs[v * 2 + 0] = quad_uv[c][0];
			cube_uvs[v * 2 + 1] = quad_uv[c][1];
			cube_colors[v * 4 + 0] = 1.0f;
			cube_colors[v * 4 + 1] = 1.0f;
			cube_colors[v * 4 + 2] = 1.0f;
			cube_colors[v * 4 + 3] = 1.0f;
			v++;
		}
	}
}

static void put(int x, int y, u8 r, u8 g, u8 b, u8 a)
{
	u8 *p = &texture_data[(y * TEX_SIZE + x) * 4];
	p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

// A 16px checkerboard of two browns, a darker grid every 32px, and a black border.
// The asymmetry (border + grid) makes texture orientation/mapping obvious on hardware.
static void build_texture(void)
{
	for (int y = 0; y < TEX_SIZE; y++) {
		for (int x = 0; x < TEX_SIZE; x++) {
			int checker = ((x >> 4) ^ (y >> 4)) & 1;
			u8 r = checker ? 198 : 120;
			u8 g = checker ? 140 : 78;
			u8 b = checker ? 74  : 40;

			if ((x % 32) == 0 || (y % 32) == 0) { r = 60;  g = 40;  b = 20; }      // grid
			if (x < 3 || x >= TEX_SIZE - 3 || y < 3 || y >= TEX_SIZE - 3) {        // border
				r = 20; g = 12; b = 6;
			}
			put(x, y, r, g, b, 255);
		}
	}
}

int main(int argc, char **argv)
{
	gfxInitDefault();
	// pglSwapBuffers() presents BOTH screens (blits picaGL's unused bottom target over the console and
	// flips its double buffer), which glitches the HUD text. Single-buffer the bottom and present only
	// the top below (pglSwapBuffersEx). See 8-opengl/3ds-opengl-landscape.md §9.
	gfxSetDoubleBuffering(GFX_BOTTOM, false);
	consoleInit(GFX_BOTTOM, NULL);

	pglInit();
	pglSelectScreen(GFX_TOP, GFX_LEFT);

	build_cube();
	build_texture();

	// Upload the texture (picaGL handles the PICA Morton tiling internally).
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_SIZE, TEX_SIZE, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, texture_data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	// Projection: NeHe's 45deg perspective, aspect for the 400x240 top screen.
	glViewport(0, 0, 400, 240);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(45.0f, 400.0f / 240.0f, 0.1f, 100.0f);
	glMatrixMode(GL_MODELVIEW);

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDisable(GL_CULL_FACE);   // sidestep per-face winding; depth test handles occlusion
	glEnable(GL_TEXTURE_2D);

	// Fixed-function vertex arrays (GL ES 1.1 idiom; picaGL copies client arrays).
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, cube_verts);
	glColorPointer(4, GL_FLOAT, 0, cube_colors);
	glTexCoordPointer(2, GL_FLOAT, 0, cube_uvs);

	float xrot = 0.0f, yrot = 0.0f, zrot = 0.0f;
	int paused = 0, linear = 1;

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown();
		if (kDown & KEY_START) break;
		if (kDown & KEY_A) paused = !paused;
		if (kDown & KEY_B) {                       // toggle texture filtering
			linear = !linear;
			GLint f = linear ? GL_LINEAR : GL_NEAREST;
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
		}

		printf("\x1b[1;1HNeHe Lesson 6 - Texture Mapping (picaGL)\n");
		printf("\x1b[3;1Hxrot %6.1f  yrot %6.1f  zrot %6.1f\n", xrot, yrot, zrot);
		printf("\x1b[5;1HFilter: %s   %s   \n", linear ? "LINEAR " : "NEAREST",
		       paused ? "[PAUSED]" : "        ");
		printf("\x1b[20;1HA: pause   B: filter   START: exit");

		glClearColor(0.0f, 0.0f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		glLoadIdentity();
		glTranslatef(0.0f, 0.0f, -5.0f);
		glRotatef(xrot, 1.0f, 0.0f, 0.0f);
		glRotatef(yrot, 0.0f, 1.0f, 0.0f);
		glRotatef(zrot, 0.0f, 0.0f, 1.0f);

		glDrawArrays(GL_TRIANGLES, 0, 36);

		pglSwapBuffersEx(true, false);   // present TOP only; leave the bottom screen to the console
		gspWaitForVBlank();

		if (!paused) { xrot += 0.6f; yrot += 0.4f; zrot += 0.3f; }
	}

	glDeleteTextures(1, &tex);
	pglExit();
	gfxExit();
	return 0;
}
