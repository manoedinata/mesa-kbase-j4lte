/* libdrm-kbase async-submit stress/perf test (surfaceless pbuffer, GLES2).
 * Per frame: clear, a texture re-upload (CPU write into a BO the GPU may
 * still read), DRAWS quads with client-side vertex arrays (transient BOs),
 * glFlush, and a native fence fd; the CPU waits for the fence from two frames
 * back, like a BufferQueue. Ends with glFinish and pixel checks.
 * usage: glstress [frames] [draws] */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

static double now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec + t.tv_nsec * 1e-9;
}

static GLuint sh(GLenum t, const char *s)
{
   GLuint o = glCreateShader(t);
   glShaderSource(o, 1, &s, NULL);
   glCompileShader(o);
   GLint ok;
   glGetShaderiv(o, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char l[512];
      glGetShaderInfoLog(o, 512, NULL, l);
      printf("shader: %s\n", l);
   }
   return o;
}

int main(int argc, char **argv)
{
   int frames = argc > 1 ? atoi(argv[1]) : 300;
   int draws = argc > 2 ? atoi(argv[2]) : 60;
   const int W = 720, H = 1280;

   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint maj, min;
   if (!eglInitialize(d, &maj, &min)) {
      printf("eglInitialize failed 0x%x\n", eglGetError());
      return 1;
   }
   const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
   EGLConfig c;
   EGLint n;
   if (!eglChooseConfig(d, ca, &c, 1, &n) || !n) {
      printf("no config\n");
      return 2;
   }
   const EGLint sa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
   EGLSurface s = eglCreatePbufferSurface(d, c, sa);
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
   if (s == EGL_NO_SURFACE || x == EGL_NO_CONTEXT || !eglMakeCurrent(d, s, s, x)) {
      printf("ctx/surface failed 0x%x\n", eglGetError());
      return 3;
   }
   PFNEGLCREATESYNCKHRPROC createSync = (void *)eglGetProcAddress("eglCreateSyncKHR");
   PFNEGLDESTROYSYNCKHRPROC destroySync = (void *)eglGetProcAddress("eglDestroySyncKHR");
   PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupFence = (void *)eglGetProcAddress("eglDupNativeFenceFDANDROID");
   int have_fence = createSync && dupFence && strstr(eglQueryString(d, EGL_EXTENSIONS), "EGL_ANDROID_native_fence_sync");
   printf("%s, %dx%d, %d frames x %d draws, native fences %s\n", glGetString(GL_RENDERER), W, H,
          frames, draws, have_fence ? "yes" : "no");

   GLuint p = glCreateProgram();
   glAttachShader(p, sh(GL_VERTEX_SHADER,
      "attribute vec2 pos; attribute vec2 uv; varying vec2 v; void main(){ v = uv; gl_Position = vec4(pos,0.0,1.0); }"));
   glAttachShader(p, sh(GL_FRAGMENT_SHADER,
      "precision mediump float; uniform sampler2D t; uniform vec4 col; varying vec2 v;"
      "void main(){ gl_FragColor = texture2D(t, v) * col; }"));
   glBindAttribLocation(p, 0, "pos");
   glBindAttribLocation(p, 1, "uv");
   glLinkProgram(p);
   glUseProgram(p);
   GLint ucol = glGetUniformLocation(p, "col");
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

   GLuint tex;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   static uint32_t texel[256 * 256];
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

   int fences[2] = { -1, -1 };
   double t0 = now(), cpu_wait = 0;
   for (int f = 0; f < frames; f++) {
      /* texture contents = frame number in the red channel */
      for (int i = 0; i < 256 * 256; i++)
         texel[i] = 0xff000000u | (uint32_t)(f & 0xff);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, texel);

      if (!getenv("NOCLEAR") || f == 0) {
         glClearColor(0, 0, 1, 1);
         glClear(GL_COLOR_BUFFER_BIT);
      }
      for (int k = 0; k < draws; k++) {
         float x0 = -1 + 2.0f * (k % 10) / 10, y0 = -1 + 2.0f * ((k / 10) % 10) / 10;
         float v[] = { x0, y0, x0 + 0.2f, y0, x0, y0 + 0.2f, x0 + 0.2f, y0 + 0.2f };
         float uv[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
         glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
         glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
         glUniform4f(ucol, 1, 1, 1, 0.5f);
         glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      }
      /* a full-screen opaque quad last: the final image is deterministic */
      float fv[] = { -1, -1, 1, -1, -1, 1, 1, 1 }, fuv[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
      if (!getenv("NOFINAL")) {
         glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, fv);
         glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, fuv);
         glUniform4f(ucol, 1, 1, 1, 1);
         glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      }

      if (have_fence) {
         EGLSyncKHR sy = createSync(d, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
         glFlush();
         int fd = dupFence(d, sy);
         destroySync(d, sy);
         double w0 = now();
         if (fences[f & 1] >= 0) {
            struct pollfd pf = { fences[f & 1], POLLIN, 0 };
            if (poll(&pf, 1, 5000) <= 0)
               printf("frame %d: fence from frame %d not signalled after 5 s\n", f, f - 2);
            close(fences[f & 1]);
         }
         cpu_wait += now() - w0;
         fences[f & 1] = fd;
      } else {
         glFlush();
      }
      eglSwapBuffers(d, s);
   }
   glFinish();
   double t1 = now();
   for (int i = 0; i < 2; i++)
      if (fences[i] >= 0)
         close(fences[i]);

   unsigned char px[4];
   int bad = 0;
   if (getenv("NOFINAL")) {
      /* Every quad k covers cell (k%10, (k/10)%10) of a 10x10 grid with 50%
       * texture red over blue; cells with no quad stay pure blue. Count. */
      int covered = 0, wrong = 0, want = (frames - 1) & 0xff;
      for (int cy = 0; cy < 10; cy++)
         for (int cx = 0; cx < 10; cx++) {
            int k = cy * 10 + cx, layers = 0;
            for (int q = k; q < draws; q += 100)
               layers++;
            glReadPixels(cx * W / 10 + W / 20, cy * H / 10 + H / 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            /* expected: red = want*(1-0.5^layers), blue = 255*0.5^layers */
            double keep = 1.0;
            for (int l = 0; l < layers; l++)
               keep *= 0.5;
            int er = (int)(want * (1 - keep) + 0.5), eb = (int)(255 * keep + 0.5);
            if (layers)
               covered++;
            if (abs(px[0] - er) > 3 || abs(px[2] - eb) > 3) {
               if (wrong < 5)
                  printf("cell %d,%d: %02x %02x %02x, want ~%02x 00 %02x (%d layers)\n",
                         cx, cy, px[0], px[1], px[2], er, eb, layers);
               wrong++;
            }
         }
      printf("grid: %d/100 cells covered, %d wrong\n", covered, wrong);
      bad += wrong;
      double t1b = now();
      printf("%d frames in %.3f s = %.1f fps -> %s\n", frames, t1b - t0, frames / (t1b - t0), bad ? "FAIL" : "PASS");
      return bad;
   }
   int pts[][2] = { { 1, 1 }, { W / 2, H / 2 }, { W - 2, H - 2 } };
   for (int i = 0; i < 3; i++) {
      glReadPixels(pts[i][0], pts[i][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      int want = (frames - 1) & 0xff;
      if (abs(px[0] - want) > 1 || px[1] || px[2])
         bad++;
      printf("pixel (%d,%d) = %02x %02x %02x %02x (want red %02x)\n", pts[i][0], pts[i][1],
             px[0], px[1], px[2], px[3], want);
   }
   printf("%d frames in %.3f s = %.1f fps, %.2f ms/frame, CPU fence wait %.2f ms/frame -> %s\n",
          frames, t1 - t0, frames / (t1 - t0), (t1 - t0) * 1000 / frames, cpu_wait * 1000 / frames,
          bad ? "FAIL" : "PASS");
   return bad;
}
