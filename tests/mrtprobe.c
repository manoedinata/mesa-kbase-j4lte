/* MRT probe for reverse engineering the Mali blob's MRT path on Midgard v4,
 * and later for checking Mesa against it. One scenario per run:
 *
 *   mrtprobe <scenario>
 *
 *   draw      2 x RGBA8, no clear (old contents preserved), constant colors
 *   clear     2 x RGBA8, glClearBufferfv with a different color per RT, no draw
 *   clrdraw   2 x RGBA8, glClear then a half-screen draw
 *   rt4       4 x RGBA8, draw
 *   rt3       3 x RGBA8, draw
 *   blend     2 x RGBA8, additive blend on RT1 only (glBlendFunci is ES 3.2,
 *             so blend is enabled for all and RT0 writes alpha 0 -> unchanged)
 *   depth     2 x RGBA8 + DEPTH24_STENCIL8, depth test on, two overlapping draws
 *   mixdepth  same as depth with RGBA8 + R8 (forces the blob's multi-pass path)
 *   mixed     RGBA8 + RGBA16F (needs EXT_color_buffer_half_float)
 *   mixed2    RGBA8 + R8
 *   int       RGBA8 + RGBA8UI
 *   sizes     RT0 64x64, RT1 128x128 texture (FBO area = 64x64)
 *   level     RT1 is mip level 1 of a 128x128 texture
 *   msaa      2 x RGBA8 4x MSAA renderbuffers, resolved with glBlitFramebuffer
 *
 * Prints pixels read back from each RT. Exit 0 if they match expectations.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define W 64
#define H 64

static GLuint prog(const char *fs)
{
	const char *vs =
		"#version 300 es\n"
		"uniform float zv; uniform vec2 half_;\n"
		"void main() {\n"
		"  vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;\n"
		"  p.x = mix(p.x, half_.y, half_.x * step(0.0, p.x));\n"
		"  gl_Position = vec4(p, zv, 1.0);\n"
		"}\n";
	GLuint p = glCreateProgram();
	const char *src[2] = { vs, fs };
	GLenum ty[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(ty[i]);
		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		GLint ok;
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			char log[1024];
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			printf("compile: %s\n", log);
			exit(10);
		}
		glAttachShader(p, s);
	}
	glLinkProgram(p);
	GLint ok;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) { printf("link failed\n"); exit(11); }
	glUseProgram(p);
	glUniform2f(glGetUniformLocation(p, "half_"), 0.0f, 0.0f);
	glUniform1f(glGetUniformLocation(p, "zv"), 0.0f);
	return p;
}

static void quad(void) { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }

static GLuint tex(GLenum ifmt, int w, int h, int levels)
{
	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexStorage2D(GL_TEXTURE_2D, levels, ifmt, w, h);
	return t;
}

/* Fill an RGBA8 texture with a solid color through a temporary FBO clear. */
static void fill(GLuint t, int level, float r, float g, float b, float a)
{
	GLuint fb;
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, level);
	glClearColor(r, g, b, a);
	glClear(GL_COLOR_BUFFER_BIT);
	glDeleteFramebuffers(1, &fb);
}

static int fails;

static void expect(const char *what, int att, int x, int y, int r, int g, int b, int a)
{
	unsigned char px[4] = { 0 };
	glReadBuffer(GL_COLOR_ATTACHMENT0 + att);
	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	int ok = abs(px[0] - r) <= 2 && abs(px[1] - g) <= 2 && abs(px[2] - b) <= 2 && abs(px[3] - a) <= 2;
	printf("%-10s RT%d (%2d,%2d) = %3d %3d %3d %3d  want %3d %3d %3d %3d  %s\n", what, att, x, y,
	       px[0], px[1], px[2], px[3], r, g, b, a, ok ? "ok" : "BAD");
	fails += !ok;
}

static GLuint fbo_with(int n, const GLuint *t, const int *levels)
{
	GLuint fb;
	GLenum bufs[4];
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	for (int i = 0; i < n; i++) {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, t[i],
		                       levels ? levels[i] : 0);
		bufs[i] = GL_COLOR_ATTACHMENT0 + i;
	}
	glDrawBuffers(n, bufs);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		printf("FBO incomplete 0x%x\n", st);
		exit(12);
	}
	glViewport(0, 0, W, H);
	return fb;
}

#define FS_HDR "#version 300 es\nprecision mediump float;\n"

static void s_draw(int n)
{
	GLuint t[4];
	for (int i = 0; i < n; i++) {
		t[i] = tex(GL_RGBA8, W, H, 1);
		fill(t[i], 0, 0.0f, 0.5f, 0.0f, 1.0f); /* old contents: mid green */
	}
	fbo_with(n, t, NULL);
	prog(n == 2 ? FS_HDR "layout(location=0) out vec4 a; layout(location=1) out vec4 b;\n"
	              "void main() { a = vec4(1,0,0,1); b = vec4(0,0,1,1); }\n"
	     : n == 3 ? FS_HDR "layout(location=0) out vec4 a; layout(location=1) out vec4 b;"
	                "layout(location=2) out vec4 c;\n"
	                "void main() { a = vec4(1,0,0,1); b = vec4(0,0,1,1); c = vec4(1,1,0,1); }\n"
	              : FS_HDR "layout(location=0) out vec4 a; layout(location=1) out vec4 b;"
	                "layout(location=2) out vec4 c; layout(location=3) out vec4 d;\n"
	                "void main() { a = vec4(1,0,0,1); b = vec4(0,0,1,1); c = vec4(1,1,0,1); d = vec4(0,1,1,1); }\n");
	/* Left half only: right half must keep the old contents (preload). */
	GLint prg;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prg);
	glUniform2f(glGetUniformLocation(prg, "half_"), 1.0f, 0.0f);
	quad();
	static const int col[4][4] = { { 255, 0, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 0, 255 }, { 0, 255, 255, 255 } };
	for (int i = 0; i < n; i++) {
		expect("drawn", i, 8, 32, col[i][0], col[i][1], col[i][2], col[i][3]);
		expect("kept", i, 56, 32, 0, 128, 0, 255);
	}
}

static void s_clear(int with_draw)
{
	GLuint t[2] = { tex(GL_RGBA8, W, H, 1), tex(GL_RGBA8, W, H, 1) };
	fbo_with(2, t, NULL);
	const float c0[4] = { 1, 0, 1, 1 }, c1[4] = { 0, 1, 1, 1 };
	glClearBufferfv(GL_COLOR, 0, c0);
	glClearBufferfv(GL_COLOR, 1, c1);
	if (with_draw) {
		GLint prg = prog(FS_HDR "layout(location=0) out vec4 a; layout(location=1) out vec4 b;\n"
		                 "void main() { a = vec4(1,0,0,1); b = vec4(0,0,1,1); }\n");
		glUniform2f(glGetUniformLocation(prg, "half_"), 1.0f, 0.0f);
		quad();
		expect("drawn", 0, 8, 32, 255, 0, 0, 255);
		expect("drawn", 1, 8, 32, 0, 0, 255, 255);
	}
	expect("cleared", 0, 56, 32, 255, 0, 255, 255);
	expect("cleared", 1, 56, 32, 0, 255, 255, 255);
}

static void s_blend(void)
{
	GLuint t[2] = { tex(GL_RGBA8, W, H, 1), tex(GL_RGBA8, W, H, 1) };
	fill(t[0], 0, 0.0f, 0.5f, 0.0f, 1.0f);
	fill(t[1], 0, 0.0f, 0.5f, 0.0f, 1.0f);
	fbo_with(2, t, NULL);
	prog(FS_HDR "layout(location=0) out vec4 a; layout(location=1) out vec4 b;\n"
	     "void main() { a = vec4(1,0,0,0); b = vec4(0,0,1,1); }\n");
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE); /* RT0: alpha 0 adds nothing; RT1 adds blue */
	quad();
	glDisable(GL_BLEND);
	expect("blended", 0, 32, 32, 0, 128, 0, 255);
	expect("blended", 1, 32, 32, 0, 128, 255, 255);
}

static void s_depth(GLenum f1)
{
	GLuint t[2] = { tex(GL_RGBA8, W, H, 1), tex(f1, W, H, 1) };
	GLuint fb = fbo_with(2, t, NULL);
	GLuint rb;
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
	glClearColor(0, 0, 0, 1);
	glClearDepthf(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	GLint prg = prog(FS_HDR "uniform vec4 c0, c1;\n"
	                 "layout(location=0) out vec4 a; layout(location=1) out vec4 b;\n"
	                 "void main() { a = c0; b = c1; }\n");
	/* Near quad first (left half), then far full quad: far must lose on the left. */
	glUniform1f(glGetUniformLocation(prg, "zv"), -0.5f);
	glUniform2f(glGetUniformLocation(prg, "half_"), 1.0f, 0.0f);
	glUniform4f(glGetUniformLocation(prg, "c0"), 1, 0, 0, 1);
	glUniform4f(glGetUniformLocation(prg, "c1"), 0, 0, 1, 1);
	quad();
	glUniform1f(glGetUniformLocation(prg, "zv"), 0.5f);
	glUniform2f(glGetUniformLocation(prg, "half_"), 0.0f, 0.0f);
	glUniform4f(glGetUniformLocation(prg, "c0"), 0, 1, 0, 1);
	glUniform4f(glGetUniformLocation(prg, "c1"), 1, 1, 0, 1);
	quad();
	glDisable(GL_DEPTH_TEST);
	expect("near", 0, 8, 32, 255, 0, 0, 255);
	if (f1 == GL_R8) {
		expect("near", 1, 8, 32, 0, 0, 0, 255);
		expect("far", 0, 56, 32, 0, 255, 0, 255);
		expect("far", 1, 56, 32, 255, 0, 0, 255);
		return;
	}
	expect("near", 1, 8, 32, 0, 0, 255, 255);
	expect("far", 0, 56, 32, 0, 255, 0, 255);
	expect("far", 1, 56, 32, 255, 255, 0, 255);
}

static void s_mixed(GLenum f1, const char *out1, int r, int g, int b, int a)
{
	GLuint t[2] = { tex(GL_RGBA8, W, H, 1), tex(f1, W, H, 1) };
	fbo_with(2, t, NULL);
	char fs[512];
	snprintf(fs, sizeof(fs), FS_HDR "layout(location=0) out vec4 a; %s\n"
	         "void main() { a = vec4(1,0,0,1); b = %s; }\n", out1,
	         f1 == GL_RGBA8UI ? "uvec4(10u, 20u, 30u, 40u)" : "vec4(0.25, 0.5, 0.75, 1.0)");
	prog(fs);
	quad();
	expect("mixed", 0, 32, 32, 255, 0, 0, 255);
	if (f1 == GL_RGBA8UI) {
		GLuint px[4] = { 0 };
		glReadBuffer(GL_COLOR_ATTACHMENT1);
		glReadPixels(32, 32, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, px);
		int ok = px[0] == 10 && px[1] == 20 && px[2] == 30 && px[3] == 40;
		printf("mixed      RT1 (32,32) = %u %u %u %u  want 10 20 30 40  %s\n", px[0], px[1], px[2], px[3],
		       ok ? "ok" : "BAD");
		fails += !ok;
	} else if (f1 == GL_RGBA16F) {
		float px[4] = { 0 };
		glReadBuffer(GL_COLOR_ATTACHMENT1);
		glReadPixels(32, 32, 1, 1, GL_RGBA, GL_FLOAT, px);
		int ok = px[0] == 0.25f && px[1] == 0.5f && px[2] == 0.75f && px[3] == 1.0f;
		printf("mixed      RT1 (32,32) = %g %g %g %g  want 0.25 0.5 0.75 1  %s\n", px[0], px[1], px[2],
		       px[3], ok ? "ok" : "BAD");
		fails += !ok;
	} else {
		expect("mixed", 1, 32, 32, r, g, b, a);
	}
}

static void s_sizes(int use_level)
{
	GLuint t[2];
	int lv[2] = { 0, use_level ? 1 : 0 };
	t[0] = tex(GL_RGBA8, W, H, 1);
	t[1] = use_level ? tex(GL_RGBA8, 2 * W, 2 * H, 2) : tex(GL_RGBA8, 2 * W, 2 * H, 1);
	fbo_with(2, t, lv);
	prog(FS_HDR "layout(location=0) out vec4 a; layout(location=1) out vec4 b;\n"
	     "void main() { a = vec4(1,0,0,1); b = vec4(0,0,1,1); }\n");
	quad();
	expect("sized", 0, 32, 32, 255, 0, 0, 255);
	expect("sized", 1, 32, 32, 0, 0, 255, 255);
}

static void s_msaa(void)
{
	GLuint rb[2], fb, t[2], rfb;
	glGenRenderbuffers(2, rb);
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	for (int i = 0; i < 2; i++) {
		glBindRenderbuffer(GL_RENDERBUFFER, rb[i]);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, W, H);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_RENDERBUFFER, rb[i]);
	}
	const GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	glDrawBuffers(2, bufs);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	printf("msaa MRT FBO status 0x%x\n", st);
	if (st != GL_FRAMEBUFFER_COMPLETE) return;
	glViewport(0, 0, W, H);
	prog(FS_HDR "layout(location=0) out vec4 a; layout(location=1) out vec4 b;\n"
	     "void main() { a = vec4(1,0,0,1); b = vec4(0,0,1,1); }\n");
	quad();
	t[0] = tex(GL_RGBA8, W, H, 1);
	t[1] = tex(GL_RGBA8, W, H, 1);
	rfb = fbo_with(2, t, NULL);
	for (int i = 0; i < 2; i++) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
		glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rfb);
		GLenum one[2] = { GL_NONE, GL_NONE };
		one[i] = GL_COLOR_ATTACHMENT0 + i;
		glDrawBuffers(2, one);
		glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, rfb);
	expect("resolved", 0, 32, 32, 255, 0, 0, 255);
	expect("resolved", 1, 32, 32, 0, 0, 255, 255);
}

int main(int argc, char **argv)
{
	const char *sc = argc > 1 ? argv[1] : "draw";
	EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	eglInitialize(d, NULL, NULL);
	const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	EGLConfig c;
	EGLint n;
	eglChooseConfig(d, ca, &c, 1, &n);
	const EGLint sa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
	EGLSurface s = eglCreatePbufferSurface(d, c, sa);
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
	if (x == EGL_NO_CONTEXT || !eglMakeCurrent(d, s, s, x)) {
		printf("no ES 3 context 0x%x\n", eglGetError());
		return 100;
	}
	printf("%s | %s | scenario %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), sc);

	if (!strcmp(sc, "draw")) s_draw(2);
	else if (!strcmp(sc, "rt3")) s_draw(3);
	else if (!strcmp(sc, "rt4")) s_draw(4);
	else if (!strcmp(sc, "clear")) s_clear(0);
	else if (!strcmp(sc, "clrdraw")) s_clear(1);
	else if (!strcmp(sc, "blend")) s_blend();
	else if (!strcmp(sc, "depth")) s_depth(GL_RGBA8);
	else if (!strcmp(sc, "mixdepth")) s_depth(GL_R8);
	else if (!strcmp(sc, "mixed")) s_mixed(GL_RGBA16F, "layout(location=1) out vec4 b;", 64, 128, 191, 255);
	else if (!strcmp(sc, "mixed2")) s_mixed(GL_R8, "layout(location=1) out vec4 b;", 64, 0, 0, 255);
	else if (!strcmp(sc, "int")) s_mixed(GL_RGBA8UI, "layout(location=1) out uvec4 b;", 0, 0, 0, 0);
	else if (!strcmp(sc, "sizes")) s_sizes(0);
	else if (!strcmp(sc, "level")) s_sizes(1);
	else if (!strcmp(sc, "msaa")) s_msaa();
	else { printf("unknown scenario\n"); return 99; }

	GLenum e = glGetError();
	if (e) { printf("GL error 0x%x\n", e); fails++; }
	glFinish();
	printf("%s\n", fails ? "FAIL" : "PASS");
	return fails;
}
