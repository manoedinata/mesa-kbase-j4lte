/* GLES 3.0 probe for Panfrost on Mali-T720 (Midgard v4).
 *
 * Mesa exposes only ES 2.0 on v4: the hardware path has one render target
 * (SFBD), and Mesa needs MaxColorAttachments >= 4 for ES 3.0. This test is
 * meant to run with the version forced, to see which ES 3.0 features work
 * anyway:
 *
 *   setprop debug.mesa.gles.version.override 3.0
 *
 * (see tests/gles3-override.sh). It creates an ES 3 context on a pbuffer,
 * prints the limits, then runs one subtest per ES 3.0 feature and prints
 * PASS / FAIL / SKIP. Exit code = number of failures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define W 64
#define H 64

static int fails;

static void result(const char *name, int ok, const char *why)
{
	GLenum e;
	while ((e = glGetError()) != GL_NO_ERROR) {
		if (ok) why = "GL error";
		ok = 0;
		printf("  [%s] glGetError 0x%x\n", name, e);
	}
	printf("%-28s %s%s%s\n", name, ok ? "PASS" : "FAIL",
	       why && !ok ? "  " : "", why && !ok ? why : "");
	fails += !ok;
}

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		printf("  shader compile: %s\n", log);
	}
	return s;
}

/* Link vs+fs. xfb_var != NULL sets one interleaved transform feedback output. */
static GLuint program(const char *vs, const char *fs, const char *xfb_var)
{
	GLuint p = glCreateProgram();
	glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
	if (xfb_var)
		glTransformFeedbackVaryings(p, 1, &xfb_var, GL_INTERLEAVED_ATTRIBS);
	glLinkProgram(p);
	GLint ok;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		printf("  program link: %s\n", log);
		glDeleteProgram(p);
		return 0;
	}
	return p;
}

/* Fullscreen-ish triangle pair from gl_VertexID, no attributes needed. */
#define VS_QUAD \
	"#version 300 es\n" \
	"void main() {\n" \
	"  vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;\n" \
	"  gl_Position = vec4(p, 0.0, 1.0);\n" \
	"}\n"

static void draw_quad(void) { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }

static int px_eq(const unsigned char *p, int r, int g, int b)
{
	return abs(p[0] - r) <= 2 && abs(p[1] - g) <= 2 && abs(p[2] - b) <= 2;
}

static GLuint fbo_rgba8(GLuint *tex)
{
	GLuint fb;
	glGenTextures(1, tex);
	glBindTexture(GL_TEXTURE_2D, *tex);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
	return fb;
}

/* 1. ESSL 3.00 in/out, draw to the default framebuffer. */
static void t_essl300(void)
{
	GLuint p = program(VS_QUAD,
		"#version 300 es\nprecision mediump float;\nout vec4 c;\n"
		"void main() { c = vec4(1.0, 0.0, 1.0, 1.0); }\n", NULL);
	if (!p) { result("essl300_draw", 0, "link failed"); return; }
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, W, H);
	glUseProgram(p);
	draw_quad();
	unsigned char px[4];
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	result("essl300_draw", px_eq(px, 255, 0, 255), "wrong color");
	glDeleteProgram(p);
}

/* 2. Integer render target (RGBA8UI) + integer readback. */
static void t_int_rt(void)
{
	GLuint tex, fb;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8UI, W, H);
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		result("int_render_target", 0, "FBO incomplete");
		goto out;
	}
	GLuint p = program(VS_QUAD,
		"#version 300 es\nprecision mediump float;\nout uvec4 c;\n"
		"void main() { c = uvec4(7u, 200u, 33u, 255u); }\n", NULL);
	if (!p) { result("int_render_target", 0, "link failed"); goto out; }
	glViewport(0, 0, W, H);
	glUseProgram(p);
	draw_quad();
	GLuint px[4] = { 0 };
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, px);
	result("int_render_target", px[0] == 7 && px[1] == 200 && px[2] == 33, "wrong value");
	if (px[0] != 7) printf("  got %u %u %u %u\n", px[0], px[1], px[2], px[3]);
	glDeleteProgram(p);
out:
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(1, &tex);
}

/* 3. Float texture (R32F) sampling + texelFetch. */
static void t_float_tex(void)
{
	GLuint tex, rt, fb = fbo_rgba8(&rt);
	const float v[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	glGenTextures(1, &tex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 4, 1, 0, GL_RED, GL_FLOAT, v);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	GLuint p = program(VS_QUAD,
		"#version 300 es\nprecision highp float;\nuniform highp sampler2D t;\nout vec4 c;\n"
		"void main() { c = vec4(texelFetch(t, ivec2(1, 0), 0).r, texelFetch(t, ivec2(2, 0), 0).r, 0.0, 1.0); }\n",
		NULL);
	if (!p) { result("float_texture_r32f", 0, "link failed"); goto out; }
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "t"), 0);
	glViewport(0, 0, W, H);
	draw_quad();
	unsigned char px[4];
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	result("float_texture_r32f", px_eq(px, 128, 191, 0), "wrong color");
	if (!px_eq(px, 128, 191, 0)) printf("  got %d %d %d\n", px[0], px[1], px[2]);
	glDeleteProgram(p);
out:
	glDeleteTextures(1, &tex);
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(1, &rt);
}

/* 4. Instanced draw with gl_InstanceID: 4 instances, one per quadrant. */
static void t_instancing(void)
{
	GLuint rt, fb = fbo_rgba8(&rt);
	GLuint p = program(
		"#version 300 es\nflat out int inst;\n"
		"void main() {\n"
		"  vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
		"  vec2 o = vec2(float(gl_InstanceID & 1), float(gl_InstanceID >> 1));\n"
		"  gl_Position = vec4(p + o - 1.0, 0.0, 1.0);\n"
		"  inst = gl_InstanceID;\n"
		"}\n",
		"#version 300 es\nprecision mediump float;\nflat in int inst;\nout vec4 c;\n"
		"void main() { c = vec4(float(inst) / 3.0, 0.0, 0.0, 1.0); }\n", NULL);
	if (!p) { result("instanced_draw", 0, "link failed"); goto out; }
	glUseProgram(p);
	glViewport(0, 0, W, H);
	glClearColor(0, 0, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 4);
	unsigned char a[4], b[4];
	glReadPixels(W / 4, H / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a);         /* instance 0 */
	glReadPixels(3 * W / 4, 3 * H / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b); /* instance 3 */
	result("instanced_draw", px_eq(a, 0, 0, 0) && px_eq(b, 255, 0, 0), "wrong color");
	glDeleteProgram(p);
out:
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(1, &rt);
}

/* 5. Transform feedback capture + glMapBufferRange readback. */
static void t_xfb(void)
{
	GLuint p = program(
		"#version 300 es\nout float v;\n"
		"void main() { v = float(gl_VertexID) * 2.5; gl_Position = vec4(0.0); }\n",
		"#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n",
		"v");
	if (!p) { result("transform_feedback", 0, "link failed"); return; }
	GLuint buf;
	glGenBuffers(1, &buf);
	glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buf);
	glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 4 * sizeof(float), NULL, GL_STATIC_READ);
	glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buf);
	glUseProgram(p);
	glEnable(GL_RASTERIZER_DISCARD);
	glBeginTransformFeedback(GL_POINTS);
	glDrawArrays(GL_POINTS, 0, 4);
	glEndTransformFeedback();
	glDisable(GL_RASTERIZER_DISCARD);
	const float *m = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 4 * sizeof(float), GL_MAP_READ_BIT);
	int ok = m && m[0] == 0.0f && m[1] == 2.5f && m[2] == 5.0f && m[3] == 7.5f;
	if (m && !ok) printf("  got %g %g %g %g\n", m[0], m[1], m[2], m[3]);
	if (m) glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
	result("transform_feedback", ok, m ? "wrong values" : "map failed");
	glDeleteBuffers(1, &buf);
	glDeleteProgram(p);
}

/* 6. Uniform buffer object. */
static void t_ubo(void)
{
	GLuint rt, fb = fbo_rgba8(&rt);
	GLuint p = program(VS_QUAD,
		"#version 300 es\nprecision mediump float;\n"
		"layout(std140) uniform U { vec4 col; };\nout vec4 c;\n"
		"void main() { c = col; }\n", NULL);
	if (!p) { result("uniform_buffer", 0, "link failed"); goto out; }
	const float col[4] = { 0.0f, 1.0f, 1.0f, 1.0f };
	GLuint ubo;
	glGenBuffers(1, &ubo);
	glBindBuffer(GL_UNIFORM_BUFFER, ubo);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(col), col, GL_STATIC_DRAW);
	glUniformBlockBinding(p, glGetUniformBlockIndex(p, "U"), 0);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
	glUseProgram(p);
	glViewport(0, 0, W, H);
	draw_quad();
	unsigned char px[4];
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	result("uniform_buffer", px_eq(px, 0, 255, 255), "wrong color");
	glDeleteBuffers(1, &ubo);
	glDeleteProgram(p);
out:
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(1, &rt);
}

/* 7. 2D array texture, sample layer 2. */
static void t_tex_array(void)
{
	GLuint tex, rt, fb = fbo_rgba8(&rt);
	unsigned char data[3][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 } };
	glGenTextures(1, &tex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	GLuint p = program(VS_QUAD,
		"#version 300 es\nprecision mediump float;\nuniform mediump sampler2DArray t;\nout vec4 c;\n"
		"void main() { c = texture(t, vec3(0.5, 0.5, 2.0)); }\n", NULL);
	if (!p) { result("texture_2d_array", 0, "link failed"); goto out; }
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "t"), 0);
	glViewport(0, 0, W, H);
	draw_quad();
	unsigned char px[4];
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	result("texture_2d_array", px_eq(px, 0, 0, 255), "wrong color");
	glDeleteProgram(p);
out:
	glDeleteTextures(1, &tex);
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(1, &rt);
}

/* 8. MSAA 4x renderbuffer + glBlitFramebuffer resolve. */
static void t_msaa(void)
{
	GLint max_samples = 0;
	glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
	if (max_samples < 4) {
		printf("%-28s SKIP  GL_MAX_SAMPLES=%d\n", "msaa4_resolve", max_samples);
		return;
	}
	GLuint rb, ms, rt, fb = fbo_rgba8(&rt);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, W, H);
	glGenFramebuffers(1, &ms);
	glBindFramebuffer(GL_FRAMEBUFFER, ms);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		result("msaa4_resolve", 0, "FBO incomplete");
		goto out;
	}
	glViewport(0, 0, W, H);
	glClearColor(1, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, ms);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
	glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	unsigned char px[4];
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	int ok = px_eq(px, 255, 255, 0);
	if (!ok) printf("  clear+resolve got %d %d %d %d\n", px[0], px[1], px[2], px[3]);

	/* Same, but with a draw instead of a clear, to tell the two apart. */
	GLuint p = program(VS_QUAD,
		"#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(0.0, 1.0, 1.0, 1.0); }\n", NULL);
	glBindFramebuffer(GL_FRAMEBUFFER, ms);
	glUseProgram(p);
	draw_quad();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, ms);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
	glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	if (!px_eq(px, 0, 255, 255)) { ok = 0; printf("  draw+resolve got %d %d %d %d\n", px[0], px[1], px[2], px[3]); }
	glDeleteProgram(p);
	result("msaa4_resolve", ok, "wrong color");
out:
	glDeleteFramebuffers(1, &ms);
	glDeleteRenderbuffers(1, &rb);
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(1, &rt);
}

/* 9. Occlusion query (ANY_SAMPLES_PASSED). */
static void t_occlusion(void)
{
	GLuint rt, fb = fbo_rgba8(&rt);
	GLuint p = program(VS_QUAD,
		"#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n", NULL);
	if (!p) { result("occlusion_query", 0, "link failed"); goto out; }
	GLuint q;
	glGenQueries(1, &q);
	glUseProgram(p);
	glViewport(0, 0, W, H);
	glBeginQuery(GL_ANY_SAMPLES_PASSED, q);
	draw_quad();
	glEndQuery(GL_ANY_SAMPLES_PASSED);
	GLuint any = 0;
	glGetQueryObjectuiv(q, GL_QUERY_RESULT, &any);
	result("occlusion_query", any == GL_TRUE, "no samples passed");
	glDeleteQueries(1, &q);
	glDeleteProgram(p);
out:
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(1, &rt);
}

/* 10. Primitive restart (fixed index, always on in ES 3.0). */
static void t_prim_restart(void)
{
	GLuint rt, fb = fbo_rgba8(&rt);
	GLuint p = program(
		"#version 300 es\nin vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n",
		"#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(0.0, 1.0, 0.0, 1.0); }\n",
		NULL);
	if (!p) { result("primitive_restart", 0, "link failed"); goto out; }
	/* Two separate strips: left quad and right quad; middle column stays clear. */
	const float v[] = {
		-1.0f, -1.0f, -0.5f, -1.0f, -1.0f, 1.0f, -0.5f, 1.0f,
		 0.5f, -1.0f,  1.0f, -1.0f,  0.5f, 1.0f,  1.0f, 1.0f,
	};
	const GLushort idx[] = { 0, 1, 2, 3, 0xffff, 4, 5, 6, 7 };
	GLuint vb, ib;
	glGenBuffers(1, &vb);
	glBindBuffer(GL_ARRAY_BUFFER, vb);
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glGenBuffers(1, &ib);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
	glBindAttribLocation(p, 0, "pos");
	glLinkProgram(p);
	glUseProgram(p);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);
	glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
	glViewport(0, 0, W, H);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawElements(GL_TRIANGLE_STRIP, 9, GL_UNSIGNED_SHORT, 0);
	glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
	glDisableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	unsigned char l[4], m[4], r[4];
	glReadPixels(W / 16, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, l);
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, m);
	glReadPixels(W - W / 16, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, r);
	result("primitive_restart", px_eq(l, 0, 255, 0) && px_eq(m, 0, 0, 0) && px_eq(r, 0, 255, 0),
	       "restart not honored");
	glDeleteBuffers(1, &vb);
	glDeleteBuffers(1, &ib);
	glDeleteProgram(p);
out:
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(1, &rt);
}

/* 11. MRT: two color attachments. Expected to fail on v4 (MAX_DRAW_BUFFERS=1).
 * Reported as INFO, not counted as failure: it is the known gap. */
static void t_mrt(void)
{
	GLint max_db = 0;
	glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_db);
	if (max_db < 2) {
		printf("%-28s INFO  GL_MAX_DRAW_BUFFERS=%d, MRT unavailable (known v4 gap)\n", "mrt_2_targets", max_db);
		return;
	}
	GLuint t[2], fb;
	glGenTextures(2, t);
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	for (int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, t[i]);
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, t[i], 0);
	}
	const GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	glDrawBuffers(2, bufs);
	GLuint p = program(VS_QUAD,
		"#version 300 es\nprecision mediump float;\n"
		"layout(location = 0) out vec4 a;\nlayout(location = 1) out vec4 b;\n"
		"void main() { a = vec4(1.0, 0.0, 0.0, 1.0); b = vec4(0.0, 0.0, 1.0, 1.0); }\n", NULL);
	if (!p) { result("mrt_2_targets", 0, "link failed"); goto out; }
	glUseProgram(p);
	glViewport(0, 0, W, H);
	draw_quad();
	unsigned char a[4], b[4];
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a);
	glReadBuffer(GL_COLOR_ATTACHMENT1);
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
	result("mrt_2_targets", px_eq(a, 255, 0, 0) && px_eq(b, 0, 0, 255), "wrong color");
	glDeleteProgram(p);
out:
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(2, t);
}

int main(void)
{
	EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint maj, min;
	if (!eglInitialize(d, &maj, &min)) {
		printf("eglInitialize failed 0x%x\n", eglGetError());
		return 100;
	}
	printf("EGL %d.%d %s\n", maj, min, eglQueryString(d, EGL_VENDOR));

	const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	EGLConfig c;
	EGLint n;
	if (!eglChooseConfig(d, ca, &c, 1, &n) || !n) {
		printf("no config 0x%x\n", eglGetError());
		return 101;
	}
	const EGLint sa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
	EGLSurface s = eglCreatePbufferSurface(d, c, sa);
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
	if (x == EGL_NO_CONTEXT) {
		printf("ES 3 context failed 0x%x (version override not active?)\n", eglGetError());
		return 102;
	}
	if (s == EGL_NO_SURFACE || !eglMakeCurrent(d, s, s, x)) {
		printf("surface/makecurrent failed 0x%x\n", eglGetError());
		return 103;
	}

	printf("GL_RENDERER=%s\nGL_VERSION=%s\nGL_SHADING_LANGUAGE_VERSION=%s\n",
	       glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
	static const struct { GLenum e; const char *n; } lim[] = {
		{ GL_MAX_DRAW_BUFFERS, "GL_MAX_DRAW_BUFFERS" },
		{ GL_MAX_COLOR_ATTACHMENTS, "GL_MAX_COLOR_ATTACHMENTS" },
		{ GL_MAX_SAMPLES, "GL_MAX_SAMPLES" },
		{ GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS, "GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS" },
		{ GL_MAX_UNIFORM_BUFFER_BINDINGS, "GL_MAX_UNIFORM_BUFFER_BINDINGS" },
		{ GL_MAX_ARRAY_TEXTURE_LAYERS, "GL_MAX_ARRAY_TEXTURE_LAYERS" },
		{ GL_MAX_3D_TEXTURE_SIZE, "GL_MAX_3D_TEXTURE_SIZE" },
	};
	for (unsigned i = 0; i < sizeof(lim) / sizeof(lim[0]); i++) {
		GLint v = -1;
		glGetIntegerv(lim[i].e, &v);
		printf("%s=%d\n", lim[i].n, v);
	}
	while (glGetError() != GL_NO_ERROR)
		;
	printf("\n");

	/* GLES3TEST_ONLY=name runs one subtest (e.g. "mrt"), for tracing. */
	static const struct { const char *n; void (*f)(void); } tests[] = {
		{ "essl300", t_essl300 }, { "int_rt", t_int_rt }, { "float_tex", t_float_tex },
		{ "instancing", t_instancing }, { "xfb", t_xfb }, { "ubo", t_ubo },
		{ "tex_array", t_tex_array }, { "msaa", t_msaa }, { "occlusion", t_occlusion },
		{ "prim_restart", t_prim_restart }, { "mrt", t_mrt },
	};
	const char *only = getenv("GLES3TEST_ONLY");
	for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
		if (!only || !strcmp(only, tests[i].n))
			tests[i].f();

	printf("\n%d failure(s)\n", fails);
	return fails;
}
