/* End-to-end test: ION buffer + Samsung gralloc3 handle -> 64-bit gralloc
 * module (lock/perform) -> Mesa/Panfrost on kbase via dma-buf EGLImage. */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <hardware/gralloc.h>

struct ion_allocation_data { size_t len, align; unsigned heap_id_mask, flags; int handle; };
struct ion_fd_data { int handle, fd; };
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_SHARE _IOWR('I', 4, struct ion_fd_data)

typedef struct { uint32_t offset, byte_stride, alloc_width, alloc_height; } plane_info_t;
struct priv_handle {
   int version, numFds, numInts, fds[5], magic, flags, width, height, req_format, _pad0;
   uint64_t producer_usage, consumer_usage, internal_format, alloc_format;
   plane_info_t plane_info[3];
   int sizes[3]; uint32_t stride, layer_count; int _pad1;
   uint64_t bases[3], backing_store_id;
   int backing_store_size, cpu_read, cpu_write, allocating_pid, remote_pid, ref_count;
   uint64_t attr_base; int yuv_info, fb_fd; uint64_t offset;
   int min_pgsz, is_compressible, ion_handles[3], private_1, private_2, plane_count;
};

struct cros_info { uint32_t drm_fourcc; int num_fds; int fds[4]; uint64_t modifier; int offset[4]; int stride[4]; };

#define W 64
#define H 64

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   const char *modpath = argc > 1 ? argv[1] : "/vendor/lib64/hw/gralloc.exynos7570.so";
   int ion = open("/dev/ion", O_RDWR);
   struct ion_allocation_data a = { W * H * 4, 4096, 1, 0, 0 };
   if (ion < 0 || ioctl(ion, ION_IOC_ALLOC, &a)) { perror("ion alloc"); return 1; }
   struct ion_fd_data s = { a.handle, -1 };
   if (ioctl(ion, ION_IOC_SHARE, &s)) { perror("ion share"); return 1; }
   printf("ION buffer fd %d (%zu bytes)\n", s.fd, a.len);

   struct priv_handle *p = calloc(1, sizeof(*p));
   p->version = 12; p->numFds = 2; p->numInts = 65 - 2;
   p->fds[0] = s.fd; p->fds[1] = dup(s.fd); /* last fd = attr region (dummy) */
   p->magic = 0x3141592; p->width = W; p->height = H; p->req_format = 1; p->alloc_format = 1;
   p->plane_info[0] = (plane_info_t){ 0, W * 4, W, H };
   p->sizes[0] = W * H * 4; p->stride = W; p->layer_count = 1; p->plane_count = 1;

   void *lib = dlopen(modpath, RTLD_NOW);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   gralloc_module_t *m = dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR);
   printf("module '%s' api %#x\n", m->common.name, m->common.module_api_version);
   if (m->registerBuffer(m, (buffer_handle_t)p)) { printf("register failed\n"); return 1; }

   struct cros_info ci;
   int r = m->perform(m, 4, (buffer_handle_t)p, &ci);
   printf("perform(GET_BUFFER_INFO)=%d fourcc %.4s fds %d fd0 %d off %d stride %d\n", r,
          (char *)&ci.drm_fourcc, ci.num_fds, ci.fds[0], ci.offset[0], ci.stride[0]);

   uint32_t *px;
   if (m->lock(m, (buffer_handle_t)p, 0x33, 0, 0, W, H, (void **)&px)) { printf("lock failed\n"); return 1; }
   for (int i = 0; i < W * H; i++) px[i] = 0xff00ff00; /* ABGR: green */
   m->unlock(m, (buffer_handle_t)p);
   printf("CPU wrote green via lock()\n");

   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   eglInitialize(d, NULL, NULL);
   printf("EGL exts has dma_buf_import: %s\n", strstr(eglQueryString(d, EGL_EXTENSIONS), "EGL_EXT_image_dma_buf_import") ? "yes" : "no");
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext x = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, xa);
   if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, x)) { printf("makecurrent 0x%x\n", eglGetError()); return 1; }

   const EGLint ia[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)ci.drm_fourcc,
      EGL_DMA_BUF_PLANE0_FD_EXT, ci.fds[0], EGL_DMA_BUF_PLANE0_OFFSET_EXT, ci.offset[0],
      EGL_DMA_BUF_PLANE0_PITCH_EXT, ci.stride[0], EGL_NONE };
   PFNEGLCREATEIMAGEKHRPROC ci_ = (void *)eglGetProcAddress("eglCreateImageKHR");
   EGLImageKHR img = ci_(d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, ia);
   if (img == EGL_NO_IMAGE_KHR) { printf("eglCreateImage failed 0x%x\n", eglGetError()); return 1; }
   PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC rbs = (void *)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
   GLuint rb, fb;
   glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb); rbs(GL_RENDERBUFFER, img);
   glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   printf("FBO status %#x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));

   unsigned char g[4];
   glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, g);
   int ok1 = g[0] == 0 && g[1] == 255 && g[2] == 0;
   printf("GPU sees CPU pixel = %02x %02x %02x %02x -> %s\n", g[0], g[1], g[2], g[3], ok1 ? "PASS" : "FAIL");

   glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); glFinish();
   m->lock(m, (buffer_handle_t)p, 0x33, 0, 0, W, H, (void **)&px);
   uint32_t v = px[10 * W + 10];
   m->unlock(m, (buffer_handle_t)p);
   int ok2 = v == 0xff0000ff;
   printf("CPU sees GPU clear = %08x -> %s\n", v, ok2 ? "PASS" : "FAIL");
   m->unregisterBuffer(m, (buffer_handle_t)p);
   return !(ok1 && ok2);
}
