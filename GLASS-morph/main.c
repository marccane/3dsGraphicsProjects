// GLASS (OpenGL ES 2) showcase: a triangle that MORPHS between two shapes,
// with the interpolation done in a programmable vertex shader and driven by a
// uniform animated on the CPU. picaGL (ES 1.1, fixed-function) cannot do this:
// it has no user vertex shaders and no custom uniforms.
//
// Press START to quit.
#include <3ds.h>
#include <math.h>
#include <GLES/gl2.h>          // pulls in <GLASS.h>
#include <KYGX/GX.h>           // kygxInit / kygxExit
#include "vshader_shbin.h"     // embedded PICA shader (picasso + bin2s)

// 7 floats per vertex: shape A (xy), shape B (xy), colour (rgb).
// Coords are in GLASS's rotated-screen NDC (x = long axis of the top screen).
typedef struct { float ax, ay; float bx, by; float r, g, b; } Vertex;

static const Vertex g_vertices[3] = {
	//  shape A (compact)   shape B (spread/flipped)   colour
	{  0.55f,  0.0f,        -0.85f,  0.0f,             1.0f, 0.0f, 0.0f }, // red
	{ -0.45f, -0.45f,        0.75f, -0.85f,            0.0f, 1.0f, 0.0f }, // green
	{ -0.45f,  0.45f,        0.75f,  0.85f,            0.0f, 0.0f, 1.0f }, // blue
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
	glClearColor(0.08f, 0.10f, 0.14f, 1.0f);

	// Load the PICA vertex shader, link, use.
	GLuint prog = glCreateProgram();
	GLuint shad = glCreateShader(GL_VERTEX_SHADER);
	glShaderBinary(1, &shad, GL_SHADER_BINARY_PICA, vshader_shbin, vshader_shbin_size);
	glAttachShader(prog, shad);
	glDeleteShader(shad);
	glLinkProgram(prog);
	glUseProgram(prog);

	// Grab the uniform location BEFORE deleting the program handle.
	GLint morphLoc = glGetUniformLocation(prog, "morph");
	glDeleteProgram(prog);

	// Interleaved VBO with both shapes + colour.
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(g_vertices), g_vertices, GL_STATIC_DRAW);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(2 * sizeof(float)));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(4 * sizeof(float)));
	glEnableVertexAttribArray(2);

	float phase = 0.0f;
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;

		// Animate the morph factor t in [0,1] with a smooth sine.
		phase += 0.025f;
		float t = 0.5f + 0.5f * sinf(phase);
		glUniform4f(morphLoc, t, 0.0f, 0.0f, 0.0f);

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
