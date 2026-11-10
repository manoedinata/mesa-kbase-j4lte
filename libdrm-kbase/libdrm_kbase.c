/*
 * libdrm replacement that presents Arm kbase (/dev/mali0, Midgard r29p0,
 * UK 11.13, Samsung J4 3.18 kernel) as a panfrost DRM render node, so Mesa's
 * panfrost Gallium driver runs unmodified except for two weak hooks
 * (drmKbaseMapBO / drmKbaseIsBOMapping) used for BO CPU mappings.
 *
 * Exports only the libdrm entry points Mesa 24.3 (Android EGL + panfrost)
 * imports. Derived from the drm-shim prototype (panfrost_kbase.c).
 *
 * Model:
 *  - one kbase context per process; every /dev/mali0 fd Mesa opens is
 *    dup2()'d onto the primary context fd;
 *  - BOs are SAME_VA: CPU VA == GPU VA; [4 GiB, 8 GiB) is reserved so kbase
 *    places them below 4 GiB like panfrost.ko;
 *  - submits are asynchronous: every GPU atom depends on the previous one, so
 *    submits run in order and complete in order (submit sequence numbers);
 *    vertex/tiler chains are regrouped into a non-tiler atom and a tiler atom
 *    (one chain as one atom misrenders under kbase; see split_chain);
 *  - WAIT_BO waits for the last submit that referenced the BO; GEM_CLOSE of a
 *    busy BO is deferred until that submit is done;
 *  - syncobjs carry a submit sequence number or a sync file; exported sync
 *    files are kbase FENCE_TRIGGER soft atoms depending on the syncobj's last
 *    atom; imported sync files become FENCE_WAIT soft atoms in the next
 *    submit, so the GPU, not the caller, waits for them.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <drm.h>
#include "panfrost_drm.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "drm-kbase", __VA_ARGS__)
#define LOGD(...) do { if (debug) __android_log_print(ANDROID_LOG_DEBUG, "drm-kbase", __VA_ARGS__); } while (0)
#else
#define LOGE(...) fprintf(stderr, "drm-kbase: " __VA_ARGS__)
#define LOGD(...) do { if (debug) fprintf(stderr, "drm-kbase: " __VA_ARGS__); } while (0)
#endif

#define PUBLIC __attribute__((visibility("default")))
#define MALI_PATH "/dev/mali0"

/* ---- kbase r29p0 UAPI --------------------------------------------------- */

#define KBASE_IOCTL_TYPE 0x80
struct kbase_ioctl_version_check { uint16_t major, minor; };
struct kbase_ioctl_set_flags { uint32_t create_flags; };
struct kbase_ioctl_job_submit { uint64_t addr; uint32_t nr_atoms, stride; };
struct kbase_ioctl_get_gpuprops { uint64_t buffer; uint32_t size, flags; };
union kbase_ioctl_mem_alloc {
   struct { uint64_t va_pages, commit_pages, extent, flags; } in;
   struct { uint64_t flags, gpu_va; } out;
};
struct kbase_ioctl_mem_free { uint64_t gpu_addr; };
union kbase_ioctl_mem_import {
   struct { uint64_t flags, phandle; uint32_t type, padding; } in;
   struct { uint64_t flags, gpu_va, va_pages; } out;
};
#define KBASE_IOCTL_VERSION_CHECK _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_SET_FLAGS     _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
#define KBASE_IOCTL_JOB_SUBMIT    _IOW(KBASE_IOCTL_TYPE, 2, struct kbase_ioctl_job_submit)
#define KBASE_IOCTL_GET_GPUPROPS  _IOW(KBASE_IOCTL_TYPE, 3, struct kbase_ioctl_get_gpuprops)
#define KBASE_IOCTL_MEM_ALLOC     _IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)
#define KBASE_IOCTL_MEM_FREE      _IOW(KBASE_IOCTL_TYPE, 7, struct kbase_ioctl_mem_free)
#define KBASE_IOCTL_MEM_IMPORT    _IOWR(KBASE_IOCTL_TYPE, 22, union kbase_ioctl_mem_import)
struct kbase_ioctl_sticky_resource { uint64_t count, address; };
#define KBASE_IOCTL_STICKY_RESOURCE_MAP   _IOW(KBASE_IOCTL_TYPE, 29, struct kbase_ioctl_sticky_resource)
#define KBASE_IOCTL_STICKY_RESOURCE_UNMAP _IOW(KBASE_IOCTL_TYPE, 30, struct kbase_ioctl_sticky_resource)

#define BASE_MEM_PROT_CPU_RD (1ull << 0)
#define BASE_MEM_PROT_CPU_WR (1ull << 1)
#define BASE_MEM_PROT_GPU_RD (1ull << 2)
#define BASE_MEM_PROT_GPU_WR (1ull << 3)
#define BASE_MEM_PROT_GPU_EX (1ull << 4)
#define BASE_MEM_GROW_ON_GPF (1ull << 9)
#define BASE_MEM_SAME_VA     (1ull << 13)
#define BASE_MEM_IMPORT_TYPE_UMM 2

#define BASE_JD_REQ_FS             (1u << 0)
#define BASE_JD_REQ_CS             (1u << 1)
#define BASE_JD_REQ_T              (1u << 2)
#define BASE_JD_REQ_COHERENT_GROUP (1u << 6)
#define BASE_JD_REQ_SOFT_FENCE_TRIGGER ((1u << 9) | 0x2)
#define BASE_JD_REQ_SOFT_FENCE_WAIT    ((1u << 9) | 0x3)
#define BASE_JD_DEP_TYPE_DATA  1
#define BASE_JD_DEP_TYPE_ORDER 2
#define BASE_JD_EVENT_DONE 0x01

struct base_fence { struct { int fd; int stream_fd; } basep; };

struct base_jd_udata { uint64_t blob[2]; };
struct base_dependency { uint8_t atom_id, dependency_type; };
struct base_jd_atom_v2 {
   uint64_t jc;
   struct base_jd_udata udata;
   uint64_t extres_list;
   uint16_t nr_extres, compat_core_req;
   struct base_dependency pre_dep[2];
   uint8_t atom_number, prio, device_nr, padding[1];
   uint32_t core_req;
   uint32_t gles_ctx_handle, frame_number; /* Samsung SRUK systrace */
   uint64_t surfacep;
};
_Static_assert(sizeof(struct base_jd_atom_v2) == 64, "Samsung r29p0 atom is 64 bytes");
struct base_jd_event_v2 {
   uint32_t event_code;
   uint8_t atom_number;
   struct base_jd_udata udata;
};

enum {
   KP_RAW_SHADER_PRESENT = 25, KP_RAW_TILER_PRESENT, KP_RAW_L2_PRESENT,
   KP_RAW_STACK_PRESENT, KP_RAW_L2_FEATURES, KP_RAW_CORE_FEATURES,
   KP_RAW_MEM_FEATURES, KP_RAW_MMU_FEATURES, KP_RAW_AS_PRESENT,
   KP_RAW_JS_PRESENT, KP_RAW_JS_FEATURES_0, KP_RAW_TILER_FEATURES = 51,
   KP_RAW_TEXTURE_FEATURES_0 = 52, KP_RAW_GPU_ID = 55, KP_RAW_THREAD_MAX_THREADS,
   KP_RAW_THREAD_MAX_WORKGROUP_SIZE, KP_RAW_THREAD_MAX_BARRIER_SIZE,
   KP_RAW_THREAD_FEATURES, KP_RAW_COHERENCY_MODE, KP_MAX = 128,
};

/* ---- state -------------------------------------------------------------- */

static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static void reinit_lock(void)
{
   pthread_mutexattr_t a;
   pthread_mutexattr_init(&a);
   pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
   pthread_mutex_init(&lock, &a);
}
static int kfd = -1;            /* primary kbase context fd */
static int debug;
enum { SPLIT_JOB, SPLIT_GROUP, SPLIT_NONE }; /* see split_chain */
static int split_mode = SPLIT_GROUP;
static uint64_t props[KP_MAX];
static uint8_t next_atom = 1;
static int sw_sync_fd = -2;
static void kstream_fd_reset(void);
static void exec_reset(void);
static void jobs_reset(void);

struct kbo {
   bool used, imported;
   bool zombie;        /* GEM-closed while the GPU may still use it */
   uint64_t last_seq;  /* last submit referencing it */
   void *map;          /* kbase SAME_VA mapping: CPU VA == GPU VA */
   uint64_t size;
   int dmabuf_fd;      /* imported: our dup of the dma-buf */
   int ion_handle;     /* imported: ION handle identifying the buffer (dedupe) */
   int exec_page;      /* >=0: sub-allocated from the shared exec window */
   void *cpu_map;      /* imported: CPU view via the dma-buf (kbase's
                          SAME_VA mapping of UMM memory is not CPU-backed) */
};
#define MAX_BO 65536
static struct kbo *bos;         /* index = GEM handle */
static uint32_t bo_hint = 1;

/* CPU address -> BO handle map (see amap_get) */
#define AMAP_SIZE (1u << 17)
static struct { uintptr_t key; uint32_t h; } amap[AMAP_SIZE];
static unsigned amap_fill; /* live + deleted slots */

/* Mesa's fds for /dev/mali0 only identify the device: every kbase call goes
 * through our own private context fd (kfd), which Mesa never sees and so
 * cannot close (HWUI's eglTerminate makes Mesa close its fds; the numbers are
 * then reused). A fd is "ours" iff it is the mali char device, by st_rdev. */
static dev_t mali_rdev;

static bool
fd_is_mali(int fd)
{
   struct stat st;
   if (fd < 0 || fstat(fd, &st) || !S_ISCHR(st.st_mode))
      return false;
   if (!mali_rdev) {
      struct stat ms;
      if (stat(MALI_PATH, &ms) || !S_ISCHR(ms.st_mode))
         return false;
      mali_rdev = ms.st_rdev;
   }
   return st.st_rdev == mali_rdev;
}

static int
kbase_setup(int fd)
{
   debug = getenv("DRM_KBASE_DEBUG") != NULL;
   const char *sm = getenv("DRM_KBASE_SPLIT");
   if (sm && !strcmp(sm, "job"))
      split_mode = SPLIT_JOB;
   else if (sm && !strcmp(sm, "none"))
      split_mode = SPLIT_NONE;

   struct kbase_ioctl_version_check ver = { 11, 13 };
   if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &ver) < 0) {
      LOGE("VERSION_CHECK: %s\n", strerror(errno));
      return -1;
   }
   struct kbase_ioctl_set_flags fl = { 0 };
   if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &fl) < 0) {
      LOGE("SET_FLAGS: %s\n", strerror(errno));
      return -1;
   }

   /* kbase places SAME_VA top-down below 2^33 (T720 VA bits). Java processes
    * fill the low 4 GiB with ART heaps, so leave BOs in [4G,8G) (the triangle
    * bug was the job chain, not >4G addresses). DRM_KBASE_LOW_VA=1 forces
    * BOs below 4 GiB like panfrost.ko, for testing. */
   if (getenv("DRM_KBASE_LOW_VA")) {
      void *resv = mmap((void *)(1ull << 32), 1ull << 32, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                        -1, 0);
      if (resv == MAP_FAILED)
         LOGE("could not reserve [4G,8G): %s\n", strerror(errno));
   }

   /* kbase refuses MEM_ALLOC until the tracking page is mapped. */
   if (mmap(NULL, 4096, PROT_NONE, MAP_SHARED, fd, 3ull << 12) == MAP_FAILED) {
      LOGE("tracking page: %s\n", strerror(errno));
      return -1;
   }

   uint8_t buf[4096];
   struct kbase_ioctl_get_gpuprops gp = { (uintptr_t)buf, sizeof(buf), 0 };
   int n = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &gp);
   if (n < 0) {
      LOGE("GET_GPUPROPS: %s\n", strerror(errno));
      return -1;
   }
   for (int i = 0; i + 4 <= n;) {
      uint32_t key;
      memcpy(&key, buf + i, 4);
      i += 4;
      unsigned sz = 1u << (key & 3);
      uint64_t v = 0;
      memcpy(&v, buf + i, sz);
      i += sz;
      if ((key >> 2) < KP_MAX)
         props[key >> 2] = v;
   }
   /* Events are drained without blocking; waiters poll(). */
   /* (kbase's check_flags insists on O_CLOEXEC in the F_SETFL argument) */
   int fl_ = fcntl(fd, F_GETFL);
   if (fl_ < 0 || (!(fl_ & O_NONBLOCK) &&
                   fcntl(fd, F_SETFL, fl_ | O_NONBLOCK | O_CLOEXEC) < 0)) {
      LOGE("O_NONBLOCK: %s\n", strerror(errno));
      return -1;
   }
   if (!bos)
      bos = calloc(MAX_BO, sizeof(*bos));
   LOGD("kbase context ready, GPU_ID %#llx\n", (unsigned long long)props[KP_RAW_GPU_ID]);
   return 0;
}

/* A forked child inherits a kbase context bound to the parent's mm (tracking
 * page, SAME_VA) - unusable. Drop it; the child sets up its own on first use. */
static void
child_after_fork(void)
{
   if (kfd >= 0)
      close(kfd);
   kfd = -1;
   kstream_fd_reset();
   exec_reset();
   jobs_reset();
   if (bos)
      memset(bos, 0, MAX_BO * sizeof(*bos));
   memset(amap, 0, sizeof(amap));
   amap_fill = 0;
   reinit_lock();
}

__attribute__((constructor)) static void
register_atfork(void)
{
   pthread_atfork(NULL, NULL, child_after_fork);
}

/* Returns true if fd is a /dev/mali0 fd; sets up our private context once. */
static bool
mali_fd(int fd)
{
   if (!fd_is_mali(fd))
      return false;
   pthread_mutex_lock(&lock);
   if (kfd < 0) {
      int k = open(MALI_PATH, O_RDWR | O_CLOEXEC | O_NONBLOCK);
      if (k >= 0) {
         /* keep it out of the low fd range apps juggle */
         int hi = fcntl(k, F_DUPFD_CLOEXEC, 512);
         if (hi >= 0) {
            close(k);
            k = hi;
         }
      }
      if (k >= 0 && kbase_setup(k) == 0)
         kfd = k;
      else if (k >= 0)
         close(k);
   }
   bool ok = kfd >= 0;
   pthread_mutex_unlock(&lock);
   return ok;
}

/* ---- BOs ---------------------------------------------------------------- */

static void ion_put(int h);

/* CPU address -> BO handle (both kbase SAME_VA maps and dma-buf CPU maps),
 * for the Mesa hooks, which are called per BO map/unmap. Open addressing;
 * key 0 = empty, 1 = deleted. */

static unsigned
amap_slot(uintptr_t a)
{
   return (unsigned)(((a >> 12) * 0x9E3779B97F4A7C15ull) >> 47) & (AMAP_SIZE - 1);
}

static uint32_t
amap_get(const void *addr)
{
   uintptr_t a = (uintptr_t)addr;
   if (a <= 1)
      return 0;
   for (unsigned i = amap_slot(a), n = 0; n < AMAP_SIZE; i = (i + 1) & (AMAP_SIZE - 1), n++) {
      if (amap[i].key == a)
         return amap[i].h;
      if (amap[i].key == 0)
         return 0;
   }
   return 0;
}

static void amap_rebuild(void);

static void
amap_put(const void *addr, uint32_t h)
{
   uintptr_t a = (uintptr_t)addr;
   if (a <= 1)
      return;
   if (amap_fill + 1 > AMAP_SIZE / 4 * 3)
      amap_rebuild();
   for (unsigned i = amap_slot(a);; i = (i + 1) & (AMAP_SIZE - 1)) {
      if (amap[i].key <= 1 || amap[i].key == a) {
         if (amap[i].key == 0)
            amap_fill++;
         amap[i].key = a;
         amap[i].h = h;
         return;
      }
   }
}

static void
amap_del(const void *addr)
{
   uintptr_t a = (uintptr_t)addr;
   if (a <= 1)
      return;
   for (unsigned i = amap_slot(a), n = 0; n < AMAP_SIZE; i = (i + 1) & (AMAP_SIZE - 1), n++) {
      if (amap[i].key == a) {
         amap[i].key = 1;
         return;
      }
      if (amap[i].key == 0)
         return;
   }
}

static void
amap_rebuild(void)
{
   memset(amap, 0, sizeof(amap));
   amap_fill = 0;
   for (uint32_t h = 1; h < MAX_BO; h++) {
      if (!bos[h].used)
         continue;
      amap_put(bos[h].map, h);
      if (bos[h].cpu_map)
         amap_put(bos[h].cpu_map, h);
   }
}

static uint32_t
bo_new(void *map, uint64_t size, bool imported)
{
   for (uint32_t i = 0; i < MAX_BO - 1; i++) {
      uint32_t h = 1 + (bo_hint + i - 1) % (MAX_BO - 1);
      if (!bos[h].used) {
         bos[h] = (struct kbo){ .used = true, .imported = imported, .map = map,
                                .size = size, .dmabuf_fd = -1, .exec_page = -1 };
         bo_hint = h + 1;
         amap_put(map, h);
         return h;
      }
   }
   return 0;
}

static struct kbo *
bo_get(uint32_t h)
{
   return (h && h < MAX_BO && bos[h].used && !bos[h].zombie) ? &bos[h] : NULL;
}

static void *
kbase_map_cookie(uint64_t cookie, uint64_t size, uint64_t flags)
{
   int prot = 0;
   if (flags & BASE_MEM_PROT_CPU_RD)
      prot |= PROT_READ;
   if (flags & BASE_MEM_PROT_CPU_WR)
      prot |= PROT_WRITE;
   return mmap(NULL, size, prot ? prot : PROT_NONE, MAP_SHARED, kfd, cookie);
}

/* Midgard's program counter is 24 bits (KBASE_GPU_PC_SIZE_LOG2) and kbase
 * gives every executable region its own 16 MiB-aligned slot, so shaders in
 * different BOs never share a PC window. Midgard blend shaders return into
 * the fragment shader, so they must: otherwise INSTR_INVALID_PC (0x50).
 * All executable BOs are therefore carved out of one 16 MiB exec region. */
#define EXEC_WINDOW_PAGES 4096
static uint8_t *exec_base;
static uint8_t exec_used[EXEC_WINDOW_PAGES];
static bool exec_failed;

static void
exec_reset(void)
{
   exec_base = NULL;
   exec_failed = false;
   memset(exec_used, 0, sizeof(exec_used));
}

static void *
exec_alloc(uint64_t pages, int *first)
{
   if (!exec_base && !exec_failed) {
      uint64_t flags = BASE_MEM_SAME_VA | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX |
                       BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR;
      union kbase_ioctl_mem_alloc a = { .in = { EXEC_WINDOW_PAGES, EXEC_WINDOW_PAGES, 0, flags } };
      void *p = MAP_FAILED;
      if (ioctl(kfd, KBASE_IOCTL_MEM_ALLOC, &a) == 0)
         p = mmap(NULL, EXEC_WINDOW_PAGES * 4096ull, PROT_READ | PROT_WRITE, MAP_SHARED, kfd, a.out.gpu_va);
      if (p == MAP_FAILED) {
         LOGE("pid %d exec window alloc failed: %s\n", getpid(), strerror(errno));
         exec_failed = true;
      } else {
         exec_base = p;
         LOGD("exec window @%p\n", p);
      }
   }
   if (!exec_base)
      return NULL;
   for (uint64_t i = 0; i + pages <= EXEC_WINDOW_PAGES; i++) {
      uint64_t j = 0;
      while (j < pages && !exec_used[i + j])
         j++;
      if (j == pages) {
         memset(&exec_used[i], 1, pages);
         *first = i;
         memset(exec_base + i * 4096, 0, pages * 4096);
         return exec_base + i * 4096;
      }
      i += j;
   }
   LOGE("pid %d exec window full (%llu pages wanted)\n", getpid(), (unsigned long long)pages);
   return NULL;
}

static int
create_bo(struct drm_panfrost_create_bo *c)
{
   uint64_t size = (c->size + 4095) & ~4095ull, pages = size / 4096;
   uint64_t flags = BASE_MEM_SAME_VA | BASE_MEM_PROT_GPU_RD;
   uint64_t commit = pages, extent = 0;

   if (c->flags & PANFROST_BO_HEAP) {
      flags |= BASE_MEM_PROT_GPU_WR | BASE_MEM_GROW_ON_GPF | BASE_MEM_PROT_CPU_RD;
      commit = pages < 64 ? pages : 64;
      extent = 64;
   } else if (c->flags & PANFROST_BO_NOEXEC) {
      flags |= BASE_MEM_PROT_GPU_WR | BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR;
   } else {
      int first;
      void *e = exec_alloc(pages, &first);
      if (e) {
         uint32_t h = bo_new(e, size, false);
         if (!h)
            return -ENOMEM;
         bos[h].exec_page = first;
         c->handle = h;
         c->offset = (uintptr_t)e;
         return 0;
      }
      flags |= BASE_MEM_PROT_GPU_EX | BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR;
   }

   union kbase_ioctl_mem_alloc a = { .in = { pages, commit, extent, flags } };
   if (ioctl(kfd, KBASE_IOCTL_MEM_ALLOC, &a) < 0) {
      LOGE("pid %d MEM_ALLOC %llu flags %#llx: %s\n", getpid(), (unsigned long long)size,
           (unsigned long long)flags, strerror(errno));
      return -errno;
   }
   void *p = kbase_map_cookie(a.out.gpu_va, size, flags);
   if (p == MAP_FAILED) {
      int e = errno;
      LOGE("pid %d mmap cookie (%llu bytes): %s\n", getpid(), (unsigned long long)size, strerror(e));
      return -e;
   }
   uint32_t h = bo_new(p, size, false);
   if (!h)
      return -ENOMEM;
   c->handle = h;
   c->offset = (uintptr_t)p;
   LOGD("BO %u size %llu flags %#x -> %p\n", h, (unsigned long long)size, c->flags, p);
   return 0;
}

static void
free_bo_now(uint32_t h)
{
   struct kbo *b = &bos[h];
   if (!b->used)
      return;
   b->zombie = false;
   amap_del(b->map);
   if (b->cpu_map)
      amap_del(b->cpu_map);
   if (b->exec_page >= 0) {
      memset(&exec_used[b->exec_page], 0, b->size / 4096);
      b->used = false;
      return;
   }
   if (b->cpu_map)
      munmap(b->cpu_map, b->size);
   if (b->imported) {
      uint64_t va = (uintptr_t)b->map;
      struct kbase_ioctl_sticky_resource sr = { 1, (uintptr_t)&va };
      ioctl(kfd, KBASE_IOCTL_STICKY_RESOURCE_UNMAP, &sr);
   }
   if (b->dmabuf_fd >= 0)
      close(b->dmabuf_fd);
   ion_put(b->ion_handle);
   /* SAME_VA: unmapping the CPU view releases the kbase region. */
   munmap(b->map, b->size);
   b->used = false;
}

/* ---- jobs (asynchronous) ------------------------------------------------ */

/* Submits return once their atoms are queued. Every GPU atom depends on the
 * previous GPU atom (ORDER between submits, DATA inside a split chain), so
 * the GPU runs submits in order, and submit N is done once the event of its
 * last atom has been read (done_seq). kbase frees an atom number only when
 * userspace reads its event, and submitting a number that is still in use
 * blocks inside the ioctl - so busy numbers are tracked here. */

struct restore_list {
   unsigned n, cap;
   struct { volatile uint8_t *h; uint64_t next; uint16_t d1, d2; } e[];
};

static struct {
   bool busy, last;        /* last: this atom completes submit .seq */
   uint32_t gen;           /* bumped on every allocation of the number */
   uint64_t seq;
   struct restore_list *restore;
} ast[256];
static uint64_t submit_seq, done_seq;
static uint8_t tail_id;        /* last GPU atom queued, the chain tail */
static uint32_t tail_gen;
static bool reader_active;     /* a thread is in poll() with the lock dropped */
static pthread_cond_t ev_cond = PTHREAD_COND_INITIALIZER;
static uint32_t *zombies;
static unsigned nzombies, zombie_cap;
#define MAX_PEND 255
static struct base_jd_atom_v2 pend[MAX_PEND];
static unsigned npend;

static bool
atom_live(uint8_t id, uint32_t gen)
{
   return id && ast[id].busy && ast[id].gen == gen;
}

static int64_t
now_ns(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1000000000ll + t.tv_nsec;
}

/* Put back the job header links the chain split cleared. */
static void
restore_headers(struct restore_list *r)
{
   if (!r)
      return;
   for (unsigned i = 0; i < r->n; i++) {
      *(volatile uint64_t *)(r->e[i].h + 0x18) = r->e[i].next;
      *(volatile uint16_t *)(r->e[i].h + 0x14) = r->e[i].d1;
      *(volatile uint16_t *)(r->e[i].h + 0x16) = r->e[i].d2;
   }
   free(r);
}

static void
reap_zombies(void)
{
   unsigned j = 0;
   for (unsigned i = 0; i < nzombies; i++) {
      uint32_t h = zombies[i];
      if (!bos[h].used || !bos[h].zombie)
         continue; /* already freed, or revived by a re-import */
      if (bos[h].last_seq <= done_seq)
         free_bo_now(h);
      else
         zombies[j++] = h;
   }
   nzombies = j;
}

static void
event_done(const struct base_jd_event_v2 *ev)
{
   uint8_t id = ev->atom_number;
   if (ev->event_code != BASE_JD_EVENT_DONE)
      LOGE("atom %u event 0x%x\n", id, ev->event_code);
   if (!id || !ast[id].busy) {
      LOGE("event for idle atom %u\n", id);
      return;
   }
   restore_headers(ast[id].restore);
   ast[id].restore = NULL;
   if (ast[id].last && ast[id].seq > done_seq)
      done_seq = ast[id].seq;
   ast[id].busy = ast[id].last = false;
}

/* Read every queued event without blocking (kfd is O_NONBLOCK). */
static int
drain_events(void)
{
   int total = 0;
   for (;;) {
      struct base_jd_event_v2 ev[16];
      ssize_t r = read(kfd, ev, sizeof(ev));
      if (r < 0) {
         if (errno == EINTR)
            continue;
         if (errno != EAGAIN)
            LOGE("event read: %s\n", strerror(errno));
         break;
      }
      int n = r / sizeof(ev[0]);
      for (int i = 0; i < n; i++)
         event_done(&ev[i]);
      total += n;
      if (n < 16)
         break;
   }
   if (total) {
      reap_zombies();
      pthread_cond_broadcast(&ev_cond);
   }
   return total;
}

/* Called with the lock held exactly once. Waits until submit seq is done or
 * abs_ns (CLOCK_MONOTONIC; INT64_MAX = forever) passes. One thread polls the
 * kbase fd with the lock dropped; the others sleep on ev_cond. */
static int
wait_seq(uint64_t seq, int64_t abs_ns)
{
   for (;;) {
      if (!reader_active)
         drain_events();
      if (seq <= done_seq)
         return 0;
      int64_t now = now_ns();
      if (now >= abs_ns)
         return -ETIMEDOUT;
      int64_t ms = (abs_ns - now + 999999) / 1000000;
      if (ms > 50)
         ms = 50;
      if (!reader_active) {
         reader_active = true;
         pthread_mutex_unlock(&lock);
         struct pollfd p = { .fd = kfd, .events = POLLIN };
         poll(&p, 1, (int)ms);
         pthread_mutex_lock(&lock);
         reader_active = false;
         drain_events();
         pthread_cond_broadcast(&ev_cond);
      } else {
         struct timespec ts;
         clock_gettime(CLOCK_REALTIME, &ts);
         ts.tv_nsec += ms * 1000000;
         ts.tv_sec += ts.tv_nsec / 1000000000;
         ts.tv_nsec %= 1000000000;
         pthread_cond_timedwait(&ev_cond, &lock, &ts);
      }
   }
}

/* Block (lock held) until no atom is in flight. Error paths only. */
static void
wait_idle_locked(void)
{
   for (;;) {
      drain_events();
      bool any = false;
      for (int i = 1; i < 256 && !any; i++)
         any = ast[i].busy;
      if (!any)
         return;
      struct pollfd p = { .fd = kfd, .events = POLLIN };
      poll(&p, 1, 100);
   }
}

static int
flush_pending(void)
{
   if (!npend)
      return 0;
   __sync_synchronize();
   struct kbase_ioctl_job_submit js = { (uintptr_t)pend, npend, sizeof(pend[0]) };
   int ret = 0;
   if (ioctl(kfd, KBASE_IOCTL_JOB_SUBMIT, &js) < 0) {
      ret = -errno;
      LOGE("JOB_SUBMIT (%u atoms): %s\n", npend, strerror(errno));
      /* These atoms never ran: free their numbers, let whatever was queued
       * before finish, then put headers back and call their submits done. */
      uint64_t maxseq = 0;
      struct restore_list *rl[MAX_PEND];
      unsigned nrl = 0;
      for (unsigned i = 0; i < npend; i++) {
         uint8_t id = pend[i].atom_number;
         if (ast[id].restore)
            rl[nrl++] = ast[id].restore;
         if (ast[id].last && ast[id].seq > maxseq)
            maxseq = ast[id].seq;
         ast[id].restore = NULL;
         ast[id].busy = ast[id].last = false;
      }
      npend = 0;
      wait_idle_locked();
      for (unsigned i = 0; i < nrl; i++)
         restore_headers(rl[i]);
      if (maxseq > done_seq)
         done_seq = maxseq;
      reap_zombies();
   }
   npend = 0;
   return ret;
}

/* Allocate a free atom number. If all 255 are in flight, queue what is
 * pending and wait for completions with the lock held (rare). */
static uint8_t
atom_alloc(void)
{
   for (;;) {
      for (unsigned i = 0; i < 255; i++) {
         uint8_t id = next_atom;
         next_atom = next_atom == 255 ? 1 : next_atom + 1;
         if (!ast[id].busy) {
            ast[id].busy = true;
            ast[id].last = false;
            ast[id].seq = 0;
            ast[id].restore = NULL;
            ast[id].gen++;
            return id;
         }
      }
      flush_pending();
      struct pollfd p = { .fd = kfd, .events = POLLIN };
      poll(&p, 1, 100);
      drain_events();
   }
}

/* ---- syncobjs ----------------------------------------------------------- */

struct sobj {
   bool used;
   int fd;          /* >= 0: an imported sync file; signalled when it is */
   uint64_t seq;    /* else: signalled once submit seq is done */
   uint8_t atom;    /* last atom of submit seq (for fence dependencies) */
   uint32_t gen;
};
#define MAX_SOBJ 16384
static struct sobj sobjs[MAX_SOBJ];
static uint32_t sobj_hint = 1;

static void
jobs_reset(void)
{
   memset(ast, 0, sizeof(ast));
   submit_seq = done_seq = 0;
   tail_id = 0;
   tail_gen = 0;
   reader_active = false;
   pthread_cond_init(&ev_cond, NULL);
   nzombies = 0;
   npend = 0;
   next_atom = 1;
   memset(sobjs, 0, sizeof(sobjs));
}

static struct sobj *
sobj_get(uint32_t h)
{
   return (h && h < MAX_SOBJ && sobjs[h].used) ? &sobjs[h] : NULL;
}

static void
sobj_set_fd(struct sobj *so, int fd)
{
   if (so->fd >= 0)
      close(so->fd);
   so->fd = fd;
}

static int
save_header(struct restore_list **rl, volatile uint8_t *h)
{
   struct restore_list *r = *rl;
   if (!r || r->n == r->cap) {
      unsigned cap = r ? r->cap * 2 : 32;
      struct restore_list *n = realloc(r, sizeof(*r) + cap * sizeof(r->e[0]));
      if (!n)
         return -ENOMEM;
      if (!r)
         n->n = 0;
      n->cap = cap;
      *rl = r = n;
   }
   r->e[r->n].h = h;
   r->e[r->n].next = *(volatile uint64_t *)(h + 0x18);
   r->e[r->n].d1 = *(volatile uint16_t *)(h + 0x14);
   r->e[r->n].d2 = *(volatile uint16_t *)(h + 0x16);
   r->n++;
   return 0;
}

/* How a vertex/tiler chain becomes atoms (DRM_KBASE_SPLIT):
 *  group (default) two atoms: every non-tiler job in chain order, then every
 *        tiler job; dependency fields that cross the two are cleared (the
 *        atom dependency replaces them). ~2.4x faster than "job" at 100
 *        draws per batch (each atom costs ~85 us of GPU scheduling);
 *  job   one atom per job (the original workaround);
 *  none  the chain as one atom, as panfrost.ko does. Broken under kbase in
 *        some batches (e.g. with a preload tiler job at the chain head the
 *        tiler emits no primitives); root cause unknown. */
static uint64_t *starts;
static unsigned starts_cap;
static uint8_t tiler_idx[65536 / 8];
#define MALI_JOB_TYPE_TILER_V4 7

/* Append a chain start; n is the current count. Returns the new count. */
static int
push_start(int n, uint64_t jc)
{
   if (n < 0)
      return n;
   if ((unsigned)n == starts_cap) {
      unsigned cap = starts_cap ? starts_cap * 2 : 64;
      uint64_t *ns = realloc(starts, cap * sizeof(*ns));
      if (!ns)
         return -ENOMEM;
      starts = ns;
      starts_cap = cap;
   }
   starts[n] = jc;
   return n + 1;
}

/* Midgard job header: +0x10 bit 1..7 type, +0x12 index, +0x14 dep1,
 * +0x16 dep2, +0x18 next. Original links are saved in *rl for restore. */
static int
split_chain(uint64_t jc, struct restore_list **rl)
{
   int n = 0;
   if (split_mode == SPLIT_NONE)
      return push_start(0, jc);

   unsigned first = *rl ? (*rl)->n : 0;
   for (uint64_t j = jc; j;) {
      if (save_header(rl, (volatile uint8_t *)(uintptr_t)j) < 0)
         return -ENOMEM;
      j = (*rl)->e[(*rl)->n - 1].next;
   }
   struct restore_list *r = *rl;
   if (!r)
      return 0;

   if (split_mode == SPLIT_JOB) {
      for (unsigned i = first; i < r->n; i++) {
         volatile uint8_t *h = r->e[i].h;
         *(volatile uint64_t *)(h + 0x18) = 0;
         *(volatile uint16_t *)(h + 0x14) = 0;
         *(volatile uint16_t *)(h + 0x16) = 0;
         n = push_start(n, (uintptr_t)h);
      }
      return n;
   }

   for (unsigned i = first; i < r->n; i++) {
      volatile uint8_t *h = r->e[i].h;
      if (((h[0x10] >> 1) & 0x7f) == MALI_JOB_TYPE_TILER_V4) {
         uint16_t idx = *(volatile uint16_t *)(h + 0x12);
         tiler_idx[idx >> 3] |= 1 << (idx & 7);
      }
   }
#define IS_TILER_IDX(d) ((tiler_idx[(d) >> 3] >> ((d) & 7)) & 1)
   volatile uint8_t *last[2] = { NULL, NULL };
   uint64_t head[2] = { 0, 0 };
   for (unsigned i = first; i < r->n; i++) {
      volatile uint8_t *h = r->e[i].h;
      int t = ((h[0x10] >> 1) & 0x7f) == MALI_JOB_TYPE_TILER_V4;
      uint16_t d1 = r->e[i].d1, d2 = r->e[i].d2;
      if (d1 && IS_TILER_IDX(d1) != t)
         d1 = 0;
      if (d2 && IS_TILER_IDX(d2) != t)
         d2 = 0;
      *(volatile uint16_t *)(h + 0x14) = d1;
      *(volatile uint16_t *)(h + 0x16) = d2;
      *(volatile uint64_t *)(h + 0x18) = 0;
      if (last[t])
         *(volatile uint64_t *)(last[t] + 0x18) = (uintptr_t)h;
      else
         head[t] = (uintptr_t)h;
      last[t] = h;
   }
#undef IS_TILER_IDX
   for (unsigned i = first; i < r->n; i++) {
      uint16_t idx = *(volatile uint16_t *)(r->e[i].h + 0x12);
      tiler_idx[idx >> 3] = 0;
   }
   if (head[0])
      n = push_start(n, head[0]);
   if (head[1])
      n = push_start(n, head[1]);
   return n;
}

static int
submit(struct drm_panfrost_submit *s)
{
   static struct base_fence in_fences[MAX_PEND];
   static unsigned nin;

   if (!s->jc)
      return 0;
   uint64_t seq = ++submit_seq;
   bool fs = s->requirements & PANFROST_JD_REQ_FS;

   /* In-fences: one FENCE_WAIT soft atom per sync file, chained, so the GPU
    * waits for them instead of this thread. kbase takes its own reference
    * to the fence at submit. Seq-based in-syncs are already ordered by the
    * GPU chain. */
   uint8_t wait_id = 0;
   uint32_t wait_gen = 0;
   const uint32_t *in = (const uint32_t *)(uintptr_t)s->in_syncs;
   for (uint32_t i = 0; i < s->in_sync_count; i++) {
      struct sobj *so = sobj_get(in[i]);
      if (!so || so->fd < 0)
         continue;
      uint8_t id = atom_alloc();
      struct base_fence *f = &in_fences[nin++ % MAX_PEND];
      f->basep.fd = so->fd;
      f->basep.stream_fd = -1;
      struct base_jd_atom_v2 *a = &pend[npend++];
      memset(a, 0, sizeof(*a));
      a->jc = (uintptr_t)f;
      a->atom_number = id;
      a->core_req = BASE_JD_REQ_SOFT_FENCE_WAIT;
      if (atom_live(wait_id, wait_gen)) {
         a->pre_dep[0].atom_id = wait_id;
         a->pre_dep[0].dependency_type = BASE_JD_DEP_TYPE_ORDER;
      }
      wait_id = id;
      wait_gen = ast[id].gen;
   }

   struct restore_list *rl = NULL;
   int nstart;
   if (fs) {
      nstart = push_start(0, s->jc);
   } else {
      nstart = split_chain(s->jc, &rl);
   }
   if (nstart < 0)
      LOGE("out of memory splitting job chain\n");

   uint8_t prev = tail_id;
   uint32_t prev_gen = tail_gen;
   bool first = true;
   for (int i = 0; i < nstart; i++) {
      uint8_t id = atom_alloc();
      struct base_jd_atom_v2 *a = &pend[npend++];
      memset(a, 0, sizeof(*a));
      a->jc = starts[i];
      a->atom_number = id;
      a->core_req = fs ? BASE_JD_REQ_COHERENT_GROUP | BASE_JD_REQ_FS
                       : BASE_JD_REQ_COHERENT_GROUP | BASE_JD_REQ_CS | BASE_JD_REQ_T;
      if (atom_live(prev, prev_gen)) {
         a->pre_dep[0].atom_id = prev;
         a->pre_dep[0].dependency_type = first ? BASE_JD_DEP_TYPE_ORDER : BASE_JD_DEP_TYPE_DATA;
      }
      if (first && atom_live(wait_id, wait_gen)) {
         a->pre_dep[1].atom_id = wait_id;
         a->pre_dep[1].dependency_type = BASE_JD_DEP_TYPE_ORDER;
      }
      prev = id;
      prev_gen = ast[id].gen;
      first = false;
   }

   if (first) {
      /* nothing queued (allocation failure) */
      restore_headers(rl);
      flush_pending();
      wait_idle_locked();
      done_seq = seq;
      return -ENOMEM;
   }
   ast[prev].last = true;
   ast[prev].seq = seq;
   ast[prev].restore = rl;
   tail_id = prev;
   tail_gen = prev_gen;

   int ret = flush_pending();

   const uint32_t *bh = (const uint32_t *)(uintptr_t)s->bo_handles;
   for (uint32_t i = 0; i < s->bo_handle_count; i++) {
      struct kbo *b = bo_get(bh[i]);
      if (b)
         b->last_seq = seq;
   }
   struct sobj *so = sobj_get(s->out_sync);
   if (so) {
      sobj_set_fd(so, -1);
      so->seq = seq;
      so->atom = prev;
      so->gen = prev_gen;
   }
   return ret;
}

static void
free_bo(uint32_t h)
{
   struct kbo *b = bo_get(h);
   if (!b)
      return;
   if (b->last_seq > done_seq) {
      if (nzombies == zombie_cap) {
         unsigned cap = zombie_cap ? zombie_cap * 2 : 256;
         uint32_t *z = realloc(zombies, cap * sizeof(*z));
         if (!z) {
            /* cannot defer: wait for the GPU instead */
            wait_seq(b->last_seq, INT64_MAX);
            free_bo_now(h);
            return;
         }
         zombies = z;
         zombie_cap = cap;
      }
      b->zombie = true;
      zombies[nzombies++] = h;
      return;
   }
   free_bo_now(h);
}

/* ---- panfrost ioctls ---------------------------------------------------- */

static int
get_param(struct drm_panfrost_get_param *gp)
{
   uint64_t id = props[KP_RAW_GPU_ID];
   switch (gp->param) {
   case DRM_PANFROST_PARAM_GPU_PROD_ID:     gp->value = id >> 16; return 0;
   case DRM_PANFROST_PARAM_GPU_REVISION:    gp->value = id & 0xffff; return 0;
   case DRM_PANFROST_PARAM_SHADER_PRESENT:  gp->value = props[KP_RAW_SHADER_PRESENT]; return 0;
   case DRM_PANFROST_PARAM_TILER_PRESENT:   gp->value = props[KP_RAW_TILER_PRESENT]; return 0;
   case DRM_PANFROST_PARAM_L2_PRESENT:      gp->value = props[KP_RAW_L2_PRESENT]; return 0;
   case DRM_PANFROST_PARAM_STACK_PRESENT:   gp->value = props[KP_RAW_STACK_PRESENT]; return 0;
   case DRM_PANFROST_PARAM_AS_PRESENT:      gp->value = props[KP_RAW_AS_PRESENT]; return 0;
   case DRM_PANFROST_PARAM_JS_PRESENT:      gp->value = props[KP_RAW_JS_PRESENT]; return 0;
   case DRM_PANFROST_PARAM_L2_FEATURES:     gp->value = props[KP_RAW_L2_FEATURES]; return 0;
   case DRM_PANFROST_PARAM_CORE_FEATURES:   gp->value = props[KP_RAW_CORE_FEATURES]; return 0;
   case DRM_PANFROST_PARAM_TILER_FEATURES:  gp->value = props[KP_RAW_TILER_FEATURES]; return 0;
   case DRM_PANFROST_PARAM_MEM_FEATURES:    gp->value = props[KP_RAW_MEM_FEATURES]; return 0;
   case DRM_PANFROST_PARAM_MMU_FEATURES:    gp->value = props[KP_RAW_MMU_FEATURES]; return 0;
   case DRM_PANFROST_PARAM_THREAD_FEATURES: gp->value = props[KP_RAW_THREAD_FEATURES]; return 0;
   case DRM_PANFROST_PARAM_MAX_THREADS:     gp->value = props[KP_RAW_THREAD_MAX_THREADS]; return 0;
   case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ:
      gp->value = props[KP_RAW_THREAD_MAX_WORKGROUP_SIZE]; return 0;
   case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ:
      gp->value = props[KP_RAW_THREAD_MAX_BARRIER_SIZE]; return 0;
   case DRM_PANFROST_PARAM_COHERENCY_FEATURES:
      gp->value = props[KP_RAW_COHERENCY_MODE]; return 0;
   case DRM_PANFROST_PARAM_TEXTURE_FEATURES0:
   case DRM_PANFROST_PARAM_TEXTURE_FEATURES1:
   case DRM_PANFROST_PARAM_TEXTURE_FEATURES2:
      gp->value = props[KP_RAW_TEXTURE_FEATURES_0 + gp->param - DRM_PANFROST_PARAM_TEXTURE_FEATURES0];
      return 0;
   case DRM_PANFROST_PARAM_TEXTURE_FEATURES3:
   case DRM_PANFROST_PARAM_THREAD_TLS_ALLOC:
   case DRM_PANFROST_PARAM_AFBC_FEATURES:
      gp->value = 0; return 0;
   case DRM_PANFROST_PARAM_NR_CORE_GROUPS:
      gp->value = 1; return 0;
   case DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP_FREQUENCY: {
      uint64_t f;
      __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
      gp->value = f; return 0;
   }
   case DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP: {
      uint64_t t;
      __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
      gp->value = t; return 0;
   }
   default:
      if (gp->param >= DRM_PANFROST_PARAM_JS_FEATURES0 &&
          gp->param <= DRM_PANFROST_PARAM_JS_FEATURES15) {
         gp->value = props[KP_RAW_JS_FEATURES_0 + gp->param - DRM_PANFROST_PARAM_JS_FEATURES0];
         return 0;
      }
      return -EINVAL;
   }
}

static int
kbase_ioctl(unsigned long req, void *arg)
{
   switch (req) {
   case DRM_IOCTL_PANFROST_GET_PARAM:
      return get_param(arg);
   case DRM_IOCTL_PANFROST_CREATE_BO:
      return create_bo(arg);
   case DRM_IOCTL_PANFROST_MMAP_BO: {
      struct drm_panfrost_mmap_bo *m = arg;
      struct kbo *b = bo_get(m->handle);
      if (!b)
         return -ENOENT;
      m->offset = (uintptr_t)b->map;
      return 0;
   }
   case DRM_IOCTL_PANFROST_GET_BO_OFFSET: {
      struct drm_panfrost_get_bo_offset *g = arg;
      struct kbo *b = bo_get(g->handle);
      if (!b)
         return -ENOENT;
      g->offset = (uintptr_t)b->map;
      return 0;
   }
   case DRM_IOCTL_PANFROST_SUBMIT:
      return submit(arg);
   case DRM_IOCTL_PANFROST_WAIT_BO: {
      /* timeout_ns is absolute CLOCK_MONOTONIC, as in panfrost.ko */
      struct drm_panfrost_wait_bo *w = arg;
      struct kbo *b = bo_get(w->handle);
      if (!b)
         return -ENOENT;
      int r = wait_seq(b->last_seq, w->timeout_ns);
      if (r == -ETIMEDOUT && w->timeout_ns == 0)
         r = -EBUSY;
      return r;
   }
   case DRM_IOCTL_PANFROST_MADVISE:
      ((struct drm_panfrost_madvise *)arg)->retained = 1;
      return 0;
   case DRM_IOCTL_GEM_CLOSE:
      free_bo(((struct drm_gem_close *)arg)->handle);
      return 0;
   default:
      LOGE("unhandled ioctl %#lx\n", req);
      return -EINVAL;
   }
}

/* ---- libdrm API --------------------------------------------------------- */

PUBLIC int
drmIoctl(int fd, unsigned long request, void *arg)
{
   if (!mali_fd(fd)) {
      int ret;
      do {
         ret = ioctl(fd, request, arg);
      } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
      return ret;
   }
   pthread_mutex_lock(&lock);
   int r = kbase_ioctl(request, arg);
   pthread_mutex_unlock(&lock);
   if (r < 0) {
      errno = -r;
      return -1;
   }
   return 0;
}

/* Mesa hooks (weak in Mesa): CPU VA of a BO is its GPU VA, already mapped. */
PUBLIC void *
drmKbaseMapBO(int fd, uint64_t offset)
{
   if (!mali_fd(fd))
      return NULL;
   void *ret = (void *)(uintptr_t)offset;
   pthread_mutex_lock(&lock);
   uint32_t h = amap_get(ret);
   struct kbo *b = h ? &bos[h] : NULL;
   if (b && b->used && b->map == ret && b->imported) {
      if (!b->cpu_map) {
         void *m = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->dmabuf_fd, 0);
         if (m == MAP_FAILED) {
            LOGE("dma-buf CPU mmap: %s\n", strerror(errno));
         } else {
            b->cpu_map = m;
            amap_put(m, h);
         }
      }
      ret = b->cpu_map;
   }
   pthread_mutex_unlock(&lock);
   return ret;
}

PUBLIC int
drmKbaseIsBOMapping(void *addr)
{
   if (!bos)
      return 0;
   pthread_mutex_lock(&lock);
   uint32_t h = amap_get(addr);
   int r = h && bos[h].used && (bos[h].map == addr || bos[h].cpu_map == addr);
   pthread_mutex_unlock(&lock);
   return r;
}

PUBLIC drmVersionPtr
drmGetVersion(int fd)
{
   drmVersionPtr v = calloc(1, sizeof(*v));
   if (mali_fd(fd)) {
      v->version_major = 1;
      v->version_minor = 2;
      v->name = strdup("panfrost");
      v->date = strdup("20260103");
      v->desc = strdup("panfrost on kbase r29p0");
   } else {
      struct drm_version dv = { 0 };
      char name[64] = "", date[64] = "", desc[128] = "";
      dv.name = name; dv.name_len = sizeof(name) - 1;
      dv.date = date; dv.date_len = sizeof(date) - 1;
      dv.desc = desc; dv.desc_len = sizeof(desc) - 1;
      if (ioctl(fd, DRM_IOCTL_VERSION, &dv)) {
         free(v);
         return NULL;
      }
      v->version_major = dv.version_major;
      v->version_minor = dv.version_minor;
      v->version_patchlevel = dv.version_patchlevel;
      v->name = strdup(name);
      v->date = strdup(date);
      v->desc = strdup(desc);
   }
   v->name_len = strlen(v->name);
   v->date_len = strlen(v->date);
   v->desc_len = strlen(v->desc);
   return v;
}

PUBLIC void
drmFreeVersion(drmVersionPtr v)
{
   if (!v)
      return;
   free(v->name);
   free(v->date);
   free(v->desc);
   free(v);
}

PUBLIC int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   switch (capability) {
   case DRM_CAP_PRIME:
      *value = DRM_PRIME_CAP_IMPORT;
      return 0;
   case DRM_CAP_SYNCOBJ:
   case DRM_CAP_SYNCOBJ_TIMELINE:
      *value = 1;
      return 0;
   default:
      *value = 0;
      return 0;
   }
}

static drmDevicePtr
make_device(void)
{
   drmDevicePtr d = calloc(1, sizeof(*d));
   d->nodes = calloc(DRM_NODE_MAX, sizeof(char *));
   d->nodes[DRM_NODE_RENDER] = strdup(MALI_PATH);
   d->available_nodes = 1 << DRM_NODE_RENDER;
   d->bustype = DRM_BUS_PLATFORM;
   d->businfo.platform = calloc(1, sizeof(*d->businfo.platform));
   strcpy(d->businfo.platform->fullname, "/soc/mali@11400000");
   d->deviceinfo.platform = calloc(1, sizeof(*d->deviceinfo.platform));
   d->deviceinfo.platform->compatible = calloc(2, sizeof(char *));
   d->deviceinfo.platform->compatible[0] = strdup("arm,mali-t720");
   return d;
}

PUBLIC void
drmFreeDevice(drmDevicePtr *device)
{
   if (!device || !*device)
      return;
   drmDevicePtr d = *device;
   if (d->nodes) {
      for (int i = 0; i < DRM_NODE_MAX; i++)
         free(d->nodes[i]);
      free(d->nodes);
   }
   if (d->deviceinfo.platform) {
      free(d->deviceinfo.platform->compatible[0]);
      free(d->deviceinfo.platform->compatible);
      free(d->deviceinfo.platform);
   }
   free(d->businfo.platform);
   free(d);
   *device = NULL;
}

PUBLIC void
drmFreeDevices(drmDevicePtr devices[], int count)
{
   for (int i = 0; i < count; i++)
      drmFreeDevice(&devices[i]);
}

PUBLIC int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
   if (access(MALI_PATH, R_OK | W_OK))
      return 0;
   if (devices && max_devices > 0)
      devices[0] = make_device();
   return 1;
}

PUBLIC int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
   if (!mali_fd(fd))
      return -ENODEV;
   *device = make_device();
   return 0;
}

PUBLIC int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
   return a && b && a->bustype == b->bustype &&
          a->nodes && b->nodes && a->nodes[DRM_NODE_RENDER] && b->nodes[DRM_NODE_RENDER] &&
          !strcmp(a->nodes[DRM_NODE_RENDER], b->nodes[DRM_NODE_RENDER]);
}

PUBLIC char *
drmGetRenderDeviceNameFromFd(int fd)
{
   return mali_fd(fd) ? strdup(MALI_PATH) : NULL;
}

PUBLIC char *
drmGetPrimaryDeviceNameFromFd(int fd)
{
   return NULL;
}

PUBLIC int
drmCloseBufferHandle(int fd, uint32_t handle)
{
   struct drm_gem_close c = { .handle = handle };
   return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
}

/* ION legacy UAPI (3.18): importing the same buffer twice into one ION
 * client returns the same handle (with an extra ref), which identifies the
 * buffer; dma-buf files themselves all share one anon inode on 3.18. */
struct ion_fd_data { int handle, fd; };
struct ion_handle_data { int handle; };
#define ION_IOC_FREE   _IOWR('I', 1, struct ion_handle_data)
#define ION_IOC_IMPORT _IOWR('I', 5, struct ion_fd_data)
static int ion_fd = -2;

static int
ion_identify(int prime_fd)
{
   if (ion_fd == -2)
      ion_fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
   if (ion_fd < 0)
      return 0;
   struct ion_fd_data d = { 0, prime_fd };
   return ioctl(ion_fd, ION_IOC_IMPORT, &d) == 0 ? d.handle : 0;
}

static void
ion_put(int h)
{
   if (h && ion_fd >= 0) {
      struct ion_handle_data d = { h };
      ioctl(ion_fd, ION_IOC_FREE, &d);
   }
}

/* dma-buf (gralloc/ION) import: kbase UMM, SAME_VA via cookie mmap.
 * Like DRM PRIME, importing an already-imported buffer returns the existing
 * handle (Mesa caches BOs per handle; a new import per swap leaks GPU VA). */
PUBLIC int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   if (!mali_fd(fd))
      return -ENODEV;
   off_t sz = lseek(prime_fd, 0, SEEK_END); /* informational; may fail on 3.18 */
   pthread_mutex_lock(&lock);
   int ionh = ion_identify(prime_fd);
   if (ionh) {
      for (uint32_t h = 1; h < MAX_BO; h++) {
         if (bos[h].used && bos[h].imported && bos[h].ion_handle == ionh) {
            ion_put(ionh); /* drop the extra ref this lookup took */
            bos[h].zombie = false; /* closed but not yet freed: revive */
            *handle = h;
            pthread_mutex_unlock(&lock);
            return 0;
         }
      }
   }
   int pfd = prime_fd;
   uint64_t flags = BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                    BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR;
   union kbase_ioctl_mem_import im = { .in = { flags, (uintptr_t)&pfd, BASE_MEM_IMPORT_TYPE_UMM, 0 } };
   int ret = 0;
   if (ioctl(kfd, KBASE_IOCTL_MEM_IMPORT, &im) < 0) {
      ret = -errno;
      LOGE("MEM_IMPORT: %s\n", strerror(errno));
   } else {
      uint64_t size = im.out.va_pages * 4096;
      void *p = kbase_map_cookie(im.out.gpu_va, size, im.out.flags);
      if (p == MAP_FAILED) {
         ret = -errno;
         LOGE("import mmap: %s\n", strerror(errno));
      } else {
         /* kbase maps UMM memory into the GPU MMU only while an atom
          * references it, unless it is made a sticky resource. */
         uint64_t va = (uintptr_t)p;
         struct kbase_ioctl_sticky_resource sr = { 1, (uintptr_t)&va };
         if (ioctl(kfd, KBASE_IOCTL_STICKY_RESOURCE_MAP, &sr) < 0)
            LOGE("STICKY_RESOURCE_MAP: %s\n", strerror(errno));
         *handle = bo_new(p, size, true);
         if (*handle) {
            bos[*handle].dmabuf_fd = fcntl(prime_fd, F_DUPFD_CLOEXEC, 0);
            bos[*handle].ion_handle = ionh;
         } else {
            ion_put(ionh);
         }
         LOGD("imported dma-buf fd %d (%lld bytes) -> BO %u @%p\n", prime_fd,
              (long long)sz, *handle, p);
      }
   }
   pthread_mutex_unlock(&lock);
   return ret;
}

PUBLIC int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   /* kbase cannot export native allocations as dma-buf. */
   return -ENOSYS;
}

/* ---- syncobj API (state: see struct sobj) ------------------------------- */

PUBLIC int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
   if (!mali_fd(fd))
      return -ENODEV;
   pthread_mutex_lock(&lock);
   int ret = -ENOMEM;
   for (uint32_t i = 0; i < MAX_SOBJ - 1; i++) {
      uint32_t h = 1 + (sobj_hint + i - 1) % (MAX_SOBJ - 1);
      if (!sobjs[h].used) {
         /* created signalled: seq 0 is always done */
         sobjs[h] = (struct sobj){ .used = true, .fd = -1 };
         sobj_hint = h + 1;
         *handle = h;
         ret = 0;
         break;
      }
   }
   pthread_mutex_unlock(&lock);
   if (ret)
      LOGE("out of syncobjs\n");
   return ret;
}

PUBLIC int
drmSyncobjDestroy(int fd, uint32_t handle)
{
   pthread_mutex_lock(&lock);
   struct sobj *so = sobj_get(handle);
   if (so) {
      sobj_set_fd(so, -1);
      so->used = false;
   }
   pthread_mutex_unlock(&lock);
   return 0;
}

PUBLIC int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
   pthread_mutex_lock(&lock);
   for (uint32_t i = 0; i < count; i++) {
      struct sobj *so = sobj_get(handles[i]);
      if (so) {
         sobj_set_fd(so, -1);
         so->seq = 0;
      }
   }
   pthread_mutex_unlock(&lock);
   return 0;
}

PUBLIC int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
   return drmSyncobjCreate(fd, 0, handle);
}

PUBLIC int
drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_point,
                   uint32_t src, uint64_t src_point, uint32_t flags)
{
   pthread_mutex_lock(&lock);
   struct sobj *d = sobj_get(dst), *s = sobj_get(src);
   if (d && s && d != s) {
      sobj_set_fd(d, s->fd >= 0 ? fcntl(s->fd, F_DUPFD_CLOEXEC, 0) : -1);
      d->seq = s->seq;
      d->atom = s->atom;
      d->gen = s->gen;
   }
   pthread_mutex_unlock(&lock);
   return 0;
}

/* Wait for a sync file until abs_ns (CLOCK_MONOTONIC). No lock held. */
static int
poll_fd_until(int fd, int64_t abs_ns)
{
   for (;;) {
      int ms = -1;
      if (abs_ns != INT64_MAX) {
         int64_t now = now_ns();
         if (now >= abs_ns)
            ms = 0;
         else
            ms = (abs_ns - now + 999999) / 1000000 > 1000000 ? 1000000 :
                 (int)((abs_ns - now + 999999) / 1000000);
      }
      struct pollfd p = { .fd = fd, .events = POLLIN };
      int r = poll(&p, 1, ms);
      if (r > 0)
         return 0;
      if (r < 0 && errno == EINTR)
         continue;
      if (r < 0)
         return -errno;
      if (ms == 0 || (abs_ns != INT64_MAX && now_ns() >= abs_ns))
         return -ETIMEDOUT;
   }
}

/* WAIT_ALL semantics for every call (Mesa only waits on one syncobj). */
static int
syncobj_wait(const uint32_t *handles, unsigned num, int64_t abs_ns)
{
   int ret = 0;
   pthread_mutex_lock(&lock);
   for (unsigned i = 0; i < num && !ret; i++) {
      struct sobj *so = sobj_get(handles[i]);
      if (!so)
         continue;
      if (so->fd >= 0) {
         int f = fcntl(so->fd, F_DUPFD_CLOEXEC, 0);
         pthread_mutex_unlock(&lock);
         ret = f >= 0 ? poll_fd_until(f, abs_ns) : -errno;
         if (f >= 0)
            close(f);
         pthread_mutex_lock(&lock);
      } else if (kfd >= 0) {
         ret = wait_seq(so->seq, abs_ns);
      }
   }
   pthread_mutex_unlock(&lock);
   return ret == -ETIMEDOUT ? -ETIME : ret;
}

PUBLIC int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num, int64_t timeout,
               unsigned flags, uint32_t *first)
{
   if (first)
      *first = 0;
   return syncobj_wait(handles, num, timeout);
}

PUBLIC int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num,
                       int64_t timeout, unsigned flags, uint32_t *first)
{
   if (first)
      *first = 0;
   return syncobj_wait(handles, num, timeout);
}

/* Legacy Android sw_sync (CONFIG_SW_SYNC_USER) for pre-signalled fences. */
struct sw_sync_create_fence_data { uint32_t value; char name[32]; int32_t fence; };
#define SW_SYNC_IOC_CREATE_FENCE _IOWR('W', 0, struct sw_sync_create_fence_data)
#define SW_SYNC_IOC_INC          _IOW('W', 1, uint32_t)

/* Native fences for Android: a kbase soft job (FENCE_TRIGGER) on a kbase
 * sync stream returns a sync fd that signals when the atom completes, i.e.
 * once its dependency (the last atom of a submit) is done. This is how Arm's
 * own driver makes Android fences, and /dev/mali0 is accessible to apps
 * (gpu_device), unlike /dev/sw_sync (root 0600, SELinux "device"). kbase
 * creates the fd at submit time, even while the dependency is pending. */
struct kbase_ioctl_stream_create { char name[32]; };
#define KBASE_IOCTL_STREAM_CREATE _IOW(KBASE_IOCTL_TYPE, 24, struct kbase_ioctl_stream_create)
static int kstream_fd = -2;

static void
kstream_fd_reset(void)
{
   kstream_fd = -2;
}

static int
kbase_fence(uint8_t dep, uint32_t dep_gen)
{
   if (kstream_fd == -2) {
      struct kbase_ioctl_stream_create sc;
      memset(&sc, 0, sizeof(sc));
      strcpy(sc.name, "drm-kbase");
      kstream_fd = ioctl(kfd, KBASE_IOCTL_STREAM_CREATE, &sc);
      if (kstream_fd < 0)
         LOGE("STREAM_CREATE: %s\n", strerror(errno));
   }
   if (kstream_fd < 0)
      return -1;

   static struct base_fence fence;
   uint8_t id = atom_alloc();
   fence.basep.fd = -1;
   fence.basep.stream_fd = kstream_fd;
   struct base_jd_atom_v2 *a = &pend[npend++];
   memset(a, 0, sizeof(*a));
   a->jc = (uintptr_t)&fence;
   a->atom_number = id;
   a->core_req = BASE_JD_REQ_SOFT_FENCE_TRIGGER;
   if (atom_live(dep, dep_gen)) {
      a->pre_dep[0].atom_id = dep;
      a->pre_dep[0].dependency_type = BASE_JD_DEP_TYPE_ORDER;
   }
   if (flush_pending() < 0)
      return -1;
   return fence.basep.fd;
}

PUBLIC int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
   if (!mali_fd(fd))
      return -ENODEV;
   pthread_mutex_lock(&lock);
   int ret = -ENOSYS;
   struct sobj *so = sobj_get(handle);
   int f = -1;
   if (so && so->fd >= 0) {
      f = fcntl(so->fd, F_DUPFD_CLOEXEC, 0);
   } else {
      if (!reader_active)
         drain_events();
      bool pending = so && so->seq > done_seq;
      f = kbase_fence(pending ? so->atom : 0, pending ? so->gen : 0);
      if (f < 0) {
         /* Fallback: wait, then a pre-signalled legacy sw_sync fence (works
          * for root only on this device). */
         if (pending)
            wait_seq(so->seq, INT64_MAX);
         if (sw_sync_fd == -2)
            sw_sync_fd = open("/dev/sw_sync", O_RDWR | O_CLOEXEC);
         if (sw_sync_fd >= 0) {
            struct sw_sync_create_fence_data d = { .value = 1 };
            strcpy(d.name, "kbase-done");
            if (ioctl(sw_sync_fd, SW_SYNC_IOC_CREATE_FENCE, &d) == 0) {
               uint32_t one = 1;
               ioctl(sw_sync_fd, SW_SYNC_IOC_INC, &one);
               f = d.fence;
            }
         }
      }
   }
   if (f >= 0) {
      *sync_file_fd = f;
      ret = 0;
   }
   pthread_mutex_unlock(&lock);
   return ret;
}

/* Keep the sync file; the next submit using this syncobj as an in-sync turns
 * it into a FENCE_WAIT atom, and waits/exports use it directly. */
PUBLIC int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
   pthread_mutex_lock(&lock);
   struct sobj *so = sobj_get(handle);
   int ret = 0;
   if (!so) {
      ret = -ENOENT;
   } else {
      int d = fcntl(sync_file_fd, F_DUPFD_CLOEXEC, 0);
      if (d < 0)
         ret = -errno;
      sobj_set_fd(so, d);
      so->seq = 0;
   }
   pthread_mutex_unlock(&lock);
   return ret;
}

/* Mesa's Android stub cutils/trace.h (newer AOSP) inlines calls to
 * atrace_get_enabled_tags(); the device's VNDK v29 libcutils (Android 10)
 * lacks it and libEGL_mesa then fails to dlopen. libdrm is in Mesa's
 * dependency closure, so provide it here: tracing reported as disabled. */
PUBLIC uint64_t
atrace_get_enabled_tags(void)
{
   return 0;
}
