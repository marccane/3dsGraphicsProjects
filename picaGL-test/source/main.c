// Minimal picaGL link/smoke test: clear the top screen and draw a coloured
// triangle with fixed-function vertex arrays (OpenGL ES 1.1). Exits on START.
#include <3ds.h>
#include <GL/picaGL.h>

// Top screen is 400x240 (picaGL renders the 3DS framebuffer sideways internally).
static const GLfloat triangle_verts[] = {
	 0.0f,  0.5f, 0.0f,
	-0.5f, -0.5f, 0.0f,
	 0.5f, -0.5f, 0.0f,
};

static const GLfloat triangle_colors[] = {
	1.0f, 0.0f, 0.0f, 1.0f,
	0.0f, 1.0f, 0.0f, 1.0f,
	0.0f, 0.0f, 1.0f, 1.0f,
};

int main(int argc, char **argv)
{
	gfxInitDefault();

	pglInit();
	pglSelectScreen(GFX_TOP, GFX_LEFT);

	glViewport(0, 0, 400, 240);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, triangle_verts);
	glColorPointer(4, GL_FLOAT, 0, triangle_colors);

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;

		glClearColor(0.2f, 0.2f, 0.4f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		glDrawArrays(GL_TRIANGLES, 0, 3);

		pglSwapBuffers();
		gspWaitForVBlank();
	}

	pglExit();
	gfxExit();
	return 0;
}
