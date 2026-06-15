// NeHe Lesson 7 (texture filters, lighting & keyboard control) ported to the 3DS via picaGL.
//
// Original: https://nehe.gamedev.net/tutorial/texture_filters,_lighting_&_keyboard_control/15002/
// A textured cube you can spin with the keyboard, with three texture filters and a toggleable light.
//
// What the PICA200 + picaGL force us to change (see 8-opengl/3ds-opengl-landscape.md §4/§9):
//
//   * LIGHTING IS A NO-OP in picaGL — glLight*/glMaterial*/glNormal*/glEnable(GL_LIGHTING) are all
//     stubs. So we do what picaGL apps actually do: compute the lighting ON THE CPU and bake it into
//     the per-vertex COLOUR array. Each frame we rotate the 6 face normals by the cube's current
//     orientation, take a diffuse term against a fixed eye-space light, and write brightness into the
//     colours; with the default GL_MODULATE tex-env that gives `texture * brightness` = a lit cube.
//     Toggling "lighting" off just writes white -> plain textured cube (the Lesson 6 look).
//
//   * NO MIPMAPS — glTexImage2D ignores level!=0 and the mipmap min-filters degrade to plain LINEAR/
//     NEAREST. NeHe's third (mipmapped) filter therefore collapses to two: GL_NEAREST and GL_LINEAR.
//
//   * Colours change every frame, so the colour array lives in the LINEAR HEAP: picaGL then reads it
//     directly each draw (pgl_address_in_linear -> cached_len 0xFFFFFFFF) instead of snapshotting it.
//     We GSPGPU_FlushDataCache after each update so the GPU sees fresh data. Position/texcoord are
//     static (set once). All three attributes are supplied contiguously (v0/v1/v2) — skipping the
//     colour array mis-routes texcoords on picaGL (the Lesson 6 bug).
//
// Controls: D-pad U/D = X-spin speed, L/R = Y-spin speed; L/R shoulders = move cube in/out;
//           A = toggle lighting, B = toggle filter (NEAREST/LINEAR), START = exit.

#include <3ds.h>
#include <GL/picaGL.h>
#include <stdio.h>
#include <math.h>

// picaGL implements gluPerspective (source/glu.c) but does not declare it in <GL/gl.h>.
extern void gluPerspective(float fovy, float aspect, float near, float far);

#define TEX_SIZE 128
#define NVERTS   36   // 6 faces * 2 triangles * 3 verts (no GL_QUADS on picaGL)
#define DEG2RAD  (3.14159265f / 180.0f)

// Cube faces as quads (-1..+1), expanded to triangles below. Plus a per-face normal.
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
static int     vert_face[NVERTS];           // face index per vertex, for normal lookup
static GLfloat *cube_colors = NULL;         // linearAlloc'd: updated & re-read every frame
static u8      texture_data[TEX_SIZE * TEX_SIZE * 4];

static void build_cube(void)
{
	int v = 0;
	for (int f = 0; f < 6; f++) {
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

// CPU "lighting": rotate each face normal into eye space by the cube's current X/Y rotation
// (matching glRotatef(xrot,1,0,0) then glRotatef(yrot,0,1,0)), diffuse against a +Z eye-space
// light, and bake ambient+diffuse brightness into the vertex colours. lit==0 -> plain white.
static void update_lighting(float xrot, float yrot, int lit)
{
	if (!lit) {
		for (int v = 0; v < NVERTS; v++) {
			cube_colors[v * 4 + 0] = 1.0f;
			cube_colors[v * 4 + 1] = 1.0f;
			cube_colors[v * 4 + 2] = 1.0f;
			cube_colors[v * 4 + 3] = 1.0f;
		}
	} else {
		float cx = cosf(xrot * DEG2RAD), sx = sinf(xrot * DEG2RAD);
		float cy = cosf(yrot * DEG2RAD), sy = sinf(yrot * DEG2RAD);
		float face_b[6];
		for (int f = 0; f < 6; f++) {
			float nx = face_normal[f][0], ny = face_normal[f][1], nz = face_normal[f][2];
			// Ry first, then Rx (vertex transform is Rx*Ry, so normal = Rx*(Ry*n)).
			// Light is (0,0,1), so we only need the eye-space Z of the rotated normal.
			float ay = ny, az = -nx * sy + nz * cy;
			float bz = ay * sx + az * cx;                 // dot(N_eye, light(0,0,1))
			float b = 0.2f + 0.8f * (bz > 0.0f ? bz : 0.0f);  // ambient 0.2 + diffuse 0.8 (dramatic)
			face_b[f] = b > 1.0f ? 1.0f : b;
		}
		for (int v = 0; v < NVERTS; v++) {
			float b = face_b[vert_face[v]];
			cube_colors[v * 4 + 0] = b;
			cube_colors[v * 4 + 1] = b;
			cube_colors[v * 4 + 2] = b;
			cube_colors[v * 4 + 3] = 1.0f;
		}
	}
	GSPGPU_FlushDataCache(cube_colors, NVERTS * 4 * sizeof(GLfloat));
}

int main(int argc, char **argv)
{
	gfxInitDefault();
	// picaGL's pglSwapBuffers() presents BOTH screens — it blits its (unused) bottom render target
	// onto the bottom framebuffer and flips that screen's double buffer every frame, which fights the
	// console and makes the HUD text glitch (partial/fading). Single-buffer the bottom so the console
	// writes to one stable, always-shown buffer, and below present only the TOP (pglSwapBuffersEx).
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

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDisable(GL_CULL_FACE);
	glEnable(GL_TEXTURE_2D);
	// Be explicit: fragment = texture * primary(vertex) colour. Our CPU-baked lighting lives in the
	// vertex colours, so MODULATE is what makes it visible. (picaGL defaults to this, but make sure.)
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	// All three attributes contiguous (v0=pos, v1=colour, v2=texcoord). Colour buffer is linear,
	// so picaGL reads our per-frame updates directly.
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, cube_verts);
	glColorPointer(4, GL_FLOAT, 0, cube_colors);
	glTexCoordPointer(2, GL_FLOAT, 0, cube_uvs);

	float xrot = 0.0f, yrot = 0.0f, xspeed = 0.4f, yspeed = 0.3f, z = -5.0f;
	int lighting = 1, linear = 1;

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown();
		u32 kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;
		if (kDown & KEY_A) lighting = !lighting;
		if (kDown & KEY_B) {
			linear = !linear;
			GLint f = linear ? GL_LINEAR : GL_NEAREST;
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
		}
		if (kHeld & KEY_DUP)    xspeed -= 0.05f;
		if (kHeld & KEY_DDOWN)  xspeed += 0.05f;
		if (kHeld & KEY_DLEFT)  yspeed -= 0.05f;
		if (kHeld & KEY_DRIGHT) yspeed += 0.05f;
		if (kHeld & KEY_L)      z -= 0.1f;   // move cube away
		if (kHeld & KEY_R)      z += 0.1f;   // move cube closer

		// Bottom screen console is 40 cols x 30 rows. Centre the title, left-align the fields
		// at a common column with colon-aligned labels.
		printf("\x1b[2;14HNeHe Lesson 7");                 // 13 chars -> centred ~col 14
		printf("\x1b[3;7HFilters / Lighting / Keyboard");  // 28 chars -> centred ~col 7

		printf("\x1b[7;10HLighting : %-7s", lighting ? "ON" : "OFF");
		printf("\x1b[8;10HFilter   : %-7s", linear ? "LINEAR" : "NEAREST");
		printf("\x1b[9;10HX-speed  : %5.2f", xspeed);
		printf("\x1b[10;10HY-speed  : %5.2f", yspeed);
		printf("\x1b[11;10HDepth    : %5.1f", z);

		printf("\x1b[14;4H(lighting is CPU-baked into vertex");
		printf("\x1b[15;5Hcolours - picaGL has no GL light)");

		printf("\x1b[25;3HD-pad: spin speed    L/R: move");
		printf("\x1b[26;3HA: light   B: filter   START: exit");

		update_lighting(xrot, yrot, lighting);

		glClearColor(0.0f, 0.0f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		glLoadIdentity();
		glTranslatef(0.0f, 0.0f, z);
		glRotatef(xrot, 1.0f, 0.0f, 0.0f);
		glRotatef(yrot, 0.0f, 1.0f, 0.0f);

		glDrawArrays(GL_TRIANGLES, 0, NVERTS);

		pglSwapBuffersEx(true, false);   // present TOP only; leave the bottom screen to the console
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
