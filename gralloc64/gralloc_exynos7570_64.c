/*
 * 64-bit gralloc0 *mapper* module for the Samsung Galaxy J4 (Exynos 7570).
 *
 * The stock vendor ships only a 32-bit gralloc.exynos7570.so (Samsung's Mali
 * gralloc3) behind a binderized IAllocator 2.0 and a passthrough IMapper 2.0.
 * A 64-bit userspace (arm64 GSI, 64-bit SurfaceFlinger/apps) loads the mapper
 * in-process, so it needs a 64-bit module that understands the handles the
 * stock 32-bit allocator service produces.
 *
 * This module never allocates (the stock 32-bit allocator keeps doing that,
 * so buffers stay compatible with the stock 32-bit hwcomposer). It provides:
 *  - registerBuffer / unregisterBuffer / lock / unlock / lock_ycbcr (+ async);
 *  - Mesa's CrOS perform op (CROS_GRALLOC_DRM_GET_BUFFER_INFO), so Mesa's
 *    u_gralloc gets exact fd/offset/stride/fourcc (module name "CrOS Gralloc").
 *
 * Handle layout: Samsung Mali gralloc3 private_handle_t, verified against the
 * stock J4 blob (J400FXXS9CVE4): numFds+numInts == 65, magic 0x3141592 at byte
 * 32, backing_store_id at byte 184; 32- and 64-bit layouts are identical.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <android/log.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>

#define LOG_TAG "gralloc.exynos7570-64"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define PRIV_MAGIC 0x3141592
#define PRIV_FLAGS_USES_HFR_MODE (1u << 6)
#define MAX_FDS 5

typedef struct {
   uint32_t offset, byte_stride, alloc_width, alloc_height;
} plane_info_t;

struct priv_handle {
   /* native_handle */
   int version, numFds, numInts;
   int fds[MAX_FDS];              /* 12: ion fds..., last = attr fd */
   int magic;                     /* 32 */
   int flags;                     /* 36 */
   int width, height;             /* 40, 44 */
   int req_format;                /* 48 */
   int _pad0;
   uint64_t producer_usage;       /* 56 */
   uint64_t consumer_usage;       /* 64 */
   uint64_t internal_format;      /* 72 */
   uint64_t alloc_format;         /* 80 */
   plane_info_t plane_info[3];    /* 88 */
   int sizes[3];                  /* 136 */
   uint32_t stride;               /* 148 */
   uint32_t layer_count;          /* 152 */
   int _pad1;
   uint64_t bases[3];             /* 160 */
   uint64_t backing_store_id;     /* 184 */
   int backing_store_size;        /* 192 */
   int cpu_read, cpu_write;       /* 196, 200 */
   int allocating_pid, remote_pid;/* 204, 208 */
   int ref_count;                 /* 212 */
   uint64_t attr_base;            /* 216 */
   int yuv_info;                  /* 224 */
   int fb_fd;                     /* 228 */
   uint64_t offset;               /* 232 */
   int min_pgsz, is_compressible; /* 240, 244 */
   int ion_handles[3];            /* 248 */
   int private_1, private_2;      /* 260, 264 */
   int plane_count;               /* 268 */
};
_Static_assert(sizeof(struct priv_handle) == 272, "size");
_Static_assert(__builtin_offsetof(struct priv_handle, magic) == 32, "magic");
_Static_assert(__builtin_offsetof(struct priv_handle, alloc_format) == 80, "alloc_format");
_Static_assert(__builtin_offsetof(struct priv_handle, plane_info) == 88, "plane_info");
_Static_assert(__builtin_offsetof(struct priv_handle, sizes) == 136, "sizes");
_Static_assert(__builtin_offsetof(struct priv_handle, stride) == 148, "stride");
_Static_assert(__builtin_offsetof(struct priv_handle, bases) == 160, "bases");
_Static_assert(__builtin_offsetof(struct priv_handle, backing_store_id) == 184, "backing_store_id");
_Static_assert(__builtin_offsetof(struct priv_handle, offset) == 232, "offset");

static struct priv_handle *
priv(buffer_handle_t h)
{
   struct priv_handle *p = (struct priv_handle *)h;
   if (!p || p->version != sizeof(native_handle_t) ||
       p->numFds + p->numInts != (int)(sizeof(*p) - sizeof(native_handle_t)) / 4 ||
       p->numFds < 1 || p->numFds > MAX_FDS || p->magic != PRIV_MAGIC)
      return NULL;
   return p;
}

static int
num_ion_fds(const struct priv_handle *p)
{
   return p->numFds - 1; /* last fd is the shared attribute region */
}

static int
map_buffer(struct priv_handle *p)
{
   if (p->flags & PRIV_FLAGS_USES_HFR_MODE)
      return -EINVAL;
   for (int i = 0; i < num_ion_fds(p) && i < 3; i++) {
      if (p->fds[i] < 0 || p->bases[i] || p->sizes[i] <= 0)
         continue;
      void *m = mmap(NULL, p->sizes[i], PROT_READ | PROT_WRITE, MAP_SHARED, p->fds[i], 0);
      if (m == MAP_FAILED) {
         ALOGE("mmap fd%d (%d bytes): %s", i, p->sizes[i], strerror(errno));
         return -errno;
      }
      p->bases[i] = (uintptr_t)m + (i == 0 ? p->offset : 0);
   }
   return 0;
}

static void
unmap_buffer(struct priv_handle *p)
{
   for (int i = 0; i < 3; i++) {
      if (!p->bases[i])
         continue;
      uintptr_t m = (uintptr_t)p->bases[i] - (i == 0 ? p->offset : 0);
      munmap((void *)m, p->sizes[i]);
      p->bases[i] = 0;
   }
}

static int
wait_fence(int fence)
{
   if (fence < 0)
      return 0;
   struct pollfd pfd = { .fd = fence, .events = POLLIN };
   int r;
   do {
      r = poll(&pfd, 1, 3000);
   } while (r < 0 && errno == EINTR);
   close(fence);
   return r > 0 ? 0 : -ETIME;
}

static int
gr_register(gralloc_module_t const *m, buffer_handle_t h)
{
   struct priv_handle *p = priv(h);
   if (!p)
      return -EINVAL;
   /* Pointers in an imported handle are meaningless in this process. */
   p->bases[0] = p->bases[1] = p->bases[2] = 0;
   p->attr_base = 0;
   return 0; /* map lazily on first lock */
}

static int
gr_unregister(gralloc_module_t const *m, buffer_handle_t h)
{
   struct priv_handle *p = priv(h);
   if (!p)
      return -EINVAL;
   unmap_buffer(p);
   return 0;
}

static int
gr_lock(gralloc_module_t const *m, buffer_handle_t h, int usage, int l, int t,
        int w, int hh, void **vaddr)
{
   struct priv_handle *p = priv(h);
   if (!p)
      return -EINVAL;
   int r = map_buffer(p);
   if (r)
      return r;
   *vaddr = (void *)(uintptr_t)p->bases[0];
   return 0;
}

static int
gr_unlock(gralloc_module_t const *m, buffer_handle_t h)
{
   return priv(h) ? 0 : -EINVAL;
}

/* HAL formats with a CPU YCbCr view. */
/* NV21/NV12 ids: HAL, Samsung Exynos HAL (exynos_format.h; "_M" = one ION
 * fd per plane) and Mali gralloc3 internal (RANGE_BASE 0x200). */
#define HAL_NV21 0x11    /* HAL_PIXEL_FORMAT_YCrCb_420_SP */
#define MALI_NV12 0x200
#define MALI_NV21 0x201
static bool is_nv12(uint32_t f) { return f == MALI_NV12 || f == 0x105 || f == 0x107 || f == 0x120 || f == 0x121 || f == 0x123; }
static bool is_nv21(uint32_t f) { return f == MALI_NV21 || f == HAL_NV21 || f == 0x11D || f == 0x11E; }
static bool is_yv12(uint32_t f);
#define HAL_YV12 0x32315659
static bool is_yv12(uint32_t f) { return f == HAL_YV12 || f == 0x11C; }
#define HAL_YCBCR_420_888 0x23

static int
gr_lock_ycbcr(gralloc_module_t const *m, buffer_handle_t h, int usage, int l,
              int t, int w, int hh, struct android_ycbcr *yc)
{
   struct priv_handle *p = priv(h);
   if (!p || !yc)
      return -EINVAL;
   int r = map_buffer(p);
   if (r)
      return r;

   uint32_t fmt = p->alloc_format & 0xffffffff;
   uint8_t *b0 = (uint8_t *)(uintptr_t)p->bases[0];
   uint8_t *b1 = (num_ion_fds(p) > 1 && p->bases[1]) ? (uint8_t *)(uintptr_t)p->bases[1] : b0;
   memset(yc, 0, sizeof(*yc));
   yc->y = b0 + p->plane_info[0].offset;
   yc->ystride = p->plane_info[0].byte_stride;

   if (is_yv12(fmt)) {
      yc->cstride = p->plane_info[1].byte_stride;
      yc->chroma_step = 1;
      yc->cr = b0 + p->plane_info[1].offset;
      yc->cb = b0 + p->plane_info[2].offset;
      return 0;
   }
   /* Semi-planar: chroma plane interleaved. */
   uint8_t *c = (b1 == b0 ? b0 : b1) + p->plane_info[1].offset;
   yc->cstride = p->plane_info[1].byte_stride;
   if (!yc->cstride && b1 == b0) {
      /* Plain single-fd NV21 (alloc 0x11, e.g. what the stock allocator
       * picks for YCbCr_420_888 camera/CPU buffers) only fills plane 0.
       * Stock lock_ycbcr (32-bit, verified 2048x1536/1920x1080/640x481/
       * 176x144): chroma at Y + stride * height (unaligned), cstride =
       * stride. */
      c = yc->y + (size_t)yc->ystride * p->height;
      yc->cstride = yc->ystride;
   }
   yc->chroma_step = 2;
   if (is_nv21(fmt)) {
      yc->cr = c;
      yc->cb = c + 1;
   } else {
      yc->cb = c;
      yc->cr = c + 1;
   }
   return 0;
}

static int
gr_lock_async(gralloc_module_t const *m, buffer_handle_t h, int usage, int l,
              int t, int w, int hh, void **vaddr, int fence)
{
   int r = wait_fence(fence);
   return r ? r : gr_lock(m, h, usage, l, t, w, hh, vaddr);
}

static int
gr_unlock_async(gralloc_module_t const *m, buffer_handle_t h, int *fence)
{
   *fence = -1;
   return gr_unlock(m, h);
}

static int
gr_lock_async_ycbcr(gralloc_module_t const *m, buffer_handle_t h, int usage,
                    int l, int t, int w, int hh, struct android_ycbcr *yc, int fence)
{
   int r = wait_fence(fence);
   return r ? r : gr_lock_ycbcr(m, h, usage, l, t, w, hh, yc);
}

/* ---- Mesa u_gralloc CrOS perform -------------------------------------- */

#define CROS_GRALLOC_DRM_GET_BUFFER_INFO 4
#define CROS_GRALLOC_DRM_GET_USAGE 5

struct cros_gralloc0_buffer_info {
   uint32_t drm_fourcc;
   int num_fds;
   int fds[4];
   uint64_t modifier;
   int offset[4];
   int stride[4];
};

#define fourcc(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static uint32_t
hal_to_fourcc(uint32_t f)
{
   switch (f) {
   case 1: return fourcc('A', 'B', '2', '4');   /* RGBA_8888 -> ABGR8888 */
   case 2: return fourcc('X', 'B', '2', '4');   /* RGBX_8888 -> XBGR8888 */
   case 3: return fourcc('B', 'G', '2', '4');   /* RGB_888 */
   case 4: return fourcc('R', 'G', '1', '6');   /* RGB_565 */
   case 5: return fourcc('A', 'R', '2', '4');   /* BGRA_8888 -> ARGB8888 */
   case 0x2b: return fourcc('A', 'B', '3', '0');/* RGBA_1010102 */
   default:
      if (is_nv21(f)) return fourcc('N', 'V', '2', '1');
      if (is_nv12(f)) return fourcc('N', 'V', '1', '2');
      if (is_yv12(f)) return fourcc('Y', 'V', '1', '2');
      return 0;
   }
}

static int
gr_perform(struct gralloc_module_t const *m, int op, ...)
{
   va_list ap;
   va_start(ap, op);
   int ret = -EINVAL;

   if (op == CROS_GRALLOC_DRM_GET_BUFFER_INFO) {
      buffer_handle_t h = va_arg(ap, buffer_handle_t);
      struct cros_gralloc0_buffer_info *info = va_arg(ap, struct cros_gralloc0_buffer_info *);
      struct priv_handle *p = priv(h);
      if (p && info) {
         uint32_t fmt = p->alloc_format & 0xffffffff;
         uint32_t fc = hal_to_fourcc(fmt);
         if (!fc)
            fc = hal_to_fourcc(p->req_format);
         if (fc) {
            memset(info, 0, sizeof(*info));
            info->drm_fourcc = fc;
            info->modifier = 0; /* DRM_FORMAT_MOD_LINEAR; T720 has no AFBC */
            int planes = (fc == fourcc('N', 'V', '1', '2') || fc == fourcc('N', 'V', '2', '1')) ? 2
                       : fc == fourcc('Y', 'V', '1', '2') ? 3 : 1;
            info->num_fds = planes;
            if (planes > 1)
               ALOGI("YUV buffer: alloc %#llx req %#x %dx%d ion fds %d plane0 off %u stride %u plane1 off %u stride %u",
                     (unsigned long long)p->alloc_format, p->req_format, p->width, p->height,
                     num_ion_fds(p), p->plane_info[0].offset, p->plane_info[0].byte_stride,
                     p->plane_info[1].offset, p->plane_info[1].byte_stride);
            for (int i = 0; i < planes; i++) {
               bool sep = i > 0 && num_ion_fds(p) > i && p->fds[i] >= 0;
               info->fds[i] = sep ? p->fds[i] : p->fds[0];
               info->offset[i] = p->plane_info[i].offset + (sep ? 0 : (int)p->offset);
               info->stride[i] = p->plane_info[i].byte_stride;
            }
            ret = 0;
         } else {
            ALOGE("GET_BUFFER_INFO: unmapped format alloc %#llx req %#x (%dx%d, %d ion fds)",
                  (unsigned long long)p->alloc_format, p->req_format, p->width, p->height,
                  num_ion_fds(p));
         }
      }
   } else if (op == CROS_GRALLOC_DRM_GET_USAGE) {
      int what = va_arg(ap, int);
      uint32_t *out = va_arg(ap, uint32_t *);
      (void)what;
      if (out) {
         *out = 0; /* no front-buffer rendering */
         ret = 0;
      }
   }
   va_end(ap);
   return ret;
}

static int
gr_open(const hw_module_t *module, const char *name, hw_device_t **device)
{
   /* Allocation is done by the stock 32-bit IAllocator service. */
   return -EINVAL;
}

static struct hw_module_methods_t gr_methods = { .open = gr_open };

__attribute__((visibility("default"))) gralloc_module_t HAL_MODULE_INFO_SYM = {
   .common = {
      .tag = HARDWARE_MODULE_TAG,
      .module_api_version = GRALLOC_MODULE_API_VERSION_0_3,
      .hal_api_version = HARDWARE_HAL_API_VERSION,
      .id = GRALLOC_HARDWARE_MODULE_ID,
      .name = "CrOS Gralloc",  /* Mesa u_gralloc keys its perform API on this */
      .author = "j4-debian-attempt",
      .methods = &gr_methods,
   },
   .registerBuffer = gr_register,
   .unregisterBuffer = gr_unregister,
   .lock = gr_lock,
   .unlock = gr_unlock,
   .perform = gr_perform,
   .lock_ycbcr = gr_lock_ycbcr,
   .lockAsync = gr_lock_async,
   .unlockAsync = gr_unlock_async,
   .lockAsync_ycbcr = gr_lock_async_ycbcr,
};
