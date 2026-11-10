/* EGL surfaceless pbuffer + GLES2 test: clear, then draw a red triangle. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

static GLuint sh(GLenum t, const char *s) {
	GLuint o = glCreateShader(t); glShaderSource(o, 1, &s, NULL); glCompileShader(o);
	GLint ok; glGetShaderiv(o, GL_COMPILE_STATUS, &ok);
	if (!ok) { char l[512]; glGetShaderInfoLog(o, 512, NULL, l); printf("shader: %s\n", l); }
	return o;
}

int main(void) {
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	EGLint maj, min;
	if (!eglInitialize(d, &maj, &min)) { printf("eglInitialize failed 0x%x\n", eglGetError()); return 1; }
	printf("EGL %d.%d %s\n", maj, min, eglQueryString(d, EGL_VENDOR));
	const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	EGLConfig c; EGLint n;
	if (!eglChooseConfig(d, ca, &c, 1, &n) || !n) { printf("no config 0x%x\n", eglGetError()); return 2; }
	const EGLint sa[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
	EGLSurface s = eglCreatePbufferSurface(d, c, sa);
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
	if (s == EGL_NO_SURFACE || x == EGL_NO_CONTEXT || !eglMakeCurrent(d, s, s, x)) { printf("ctx/surface failed 0x%x\n", eglGetError()); return 3; }
	printf("GL_RENDERER=%s\nGL_VERSION=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

	unsigned char px[4];
	glClearColor(0, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT); glFinish();
	glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	int ok1 = px[0] == 0 && px[1] == 255 && px[2] == 0;
	printf("clear pixel = %02x %02x %02x %02x -> %s\n", px[0], px[1], px[2], px[3], ok1 ? "PASS" : "FAIL");

	GLuint p = glCreateProgram();
	glAttachShader(p, sh(GL_VERTEX_SHADER, "attribute vec2 pos; void main(){ gl_Position = vec4(pos,0.0,1.0); }"));
	glAttachShader(p, sh(GL_FRAGMENT_SHADER, "precision mediump float; void main(){ gl_FragColor = vec4(1.0,0.0,0.0,1.0); }"));
	glBindAttribLocation(p, 0, "pos"); glLinkProgram(p); glUseProgram(p);
	const float v[] = { -1, -1, 1, -1, 0, 1 };
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v); glEnableVertexAttribArray(0);
	if (getenv("SAMEFRAME")) { glClearColor(0, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT); }
	glDrawArrays(GL_TRIANGLES, 0, 3); glFinish();
	unsigned char in[4], out[4];
	glReadPixels(32, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, in);   /* inside triangle */
	glReadPixels(2, 60, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);   /* outside (top-left corner) */
	int ok2 = in[0] == 255 && in[1] == 0 && out[1] == 255 && out[0] == 0;
	printf("triangle inside = %02x %02x %02x, outside = %02x %02x %02x -> %s\n",
	       in[0], in[1], in[2], out[0], out[1], out[2], ok2 ? "PASS" : "FAIL");
	return !(ok1 && ok2);
}
