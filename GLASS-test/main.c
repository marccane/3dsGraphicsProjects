// GLASS (OpenGL ES 2) test: draws a per-vertex RGB triangle on the top screen
// using a precompiled PICA200 vertex shader loaded via glShaderBinary.
// Verifies GLASSv2 + rip + kygx + (forked) libctru link AND render.
#include <3ds.h>
#include <GLES/gl2.h>            // pulls in <GLASS.h>
#include <KYGX/GX.h>             // kygxInit / kygxExit
#include "vshader_shbin.h"       // embedded PICA shader (picasso + bin2s)

// Both GLASS screens are rotated 90deg: bottom-left is (-1,1), top-right (1,-1),
// so x is the long screen axis. One RGB colour per corner.
typedef struct { float x, y; float r, g, b; } Vertex;

static const Vertex g_vertices[3] = {
	{  0.66f,  0.0f,  1.0f, 0.0f, 0.0f }, // red
	{ -0.66f, -0.5f,  0.0f, 1.0f, 0.0f }, // green
	{ -0.66f,  0.5f,  0.0f, 0.0f, 1.0f }, // blue
};

int main(int argc, char **argv)
{
	gfxInitDefault();
	kygxInit();

	GLASSCtx ctx = glassCreateDefaultContext(GLASS_VERSION_ES_2);
	glassBindContext(ctx);

	u16 w = 0, h = 0;
	glassGetScreenFramebuffer(ctx, &w, &h, NULL);

	GLuint fb, rb;
	glGenFramebuffers(1, &fb);
	glGenRenderbuffers(1, &rb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);

	glViewport(0, 0, w, h);
	glClearColor(0.125f, 0.25f, 0.375f, 1.0f); // dark blue background

	// Load the precompiled PICA vertex shader, link, use.
	GLuint prog = glCreateProgram();
	GLuint shad = glCreateShader(GL_VERTEX_SHADER);
	glShaderBinary(1, &shad, GL_SHADER_BINARY_PICA, vshader_shbin, vshader_shbin_size);
	glAttachShader(prog, shad);
	glDeleteShader(shad);
	glLinkProgram(prog);
	glUseProgram(prog);
	glDeleteProgram(prog);

	// Interleaved VBO: position (v0) + colour (v1).
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(g_vertices), g_vertices, GL_STATIC_DRAW);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(2 * sizeof(float)));
	glEnableVertexAttribArray(1);

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;

		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glassSwapBuffers();
		gspWaitForVBlank();
	}

	glDeleteBuffers(1, &vbo);
	glDeleteRenderbuffers(1, &rb);
	glDeleteFramebuffers(1, &fb);
	glassDestroyContext(ctx);

	kygxExit();
	gfxExit();
	return 0;
}
