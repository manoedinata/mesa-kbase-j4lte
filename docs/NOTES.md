# Notes: Mesa Panfrost on Samsung kbase (Galaxy J4)

All of this was verified on an SM-J400F running the stock 3.18 kernel, with
Mali kbase r29p0 (UK 11.13) and the stock vendor (J400FXXS9CVE4).

## Why not an Arm blob

- The 64-bit H-VENDOR r28p0 blob is built for T830 only. It fails the DDK
  product check. With the check patched out, the GPU reports DATA_INVALID_FAULT.
- The arm64 T720 blobs that exist are r7p0/r12p1 (UK 10.x) and don't talk to
  r29p0.
- So: Mesa Panfrost, which on T720 (Midgard v4) exposes OpenGL ES 2.0.

## How it fits together

Mesa's panfrost driver only knows the panfrost DRM UAPI. `libdrm-kbase` is a
`libdrm.so` that exports just the `drm*` symbols Mesa imports and implements
the panfrost ioctls on top of `/dev/mali0`:

- `drmGetDevices2` presents `/dev/mali0` as a "panfrost" render node.
- One private kbase context per process. Its fd is dup'd to >= 512. Mesa's own
  `/dev/mali0` fds are recognised by `st_rdev`, so they can be closed: HWUI's
  `eglTerminate` closes them, and with a shared fd that killed the context
  ("Failed to choose config, error = EGL_NOT_INITIALIZED").
- After fork, the child drops the inherited context (it is bound to the
  parent's mm) and creates its own on first use.
- BOs are kbase SAME_VA allocations: CPU VA == GPU VA. Two weak hooks in Mesa
  (`drmKbaseMapBO`, `drmKbaseIsBOMapping`, see `patches/mesa/`) make Mesa
  use those mappings instead of mmapping the DRM fd. With normal libdrm the
  hooks are NULL, so the patched Mesa still works with panfrost.ko.
- All exec BOs are sub-allocated from ONE 16 MiB exec region. The Midgard
  shader PC is 24 bits, and kbase gives every exec region its own 16 MiB slot.
  With separate regions, blend shaders could not return into the fragment
  shader: INSTR_INVALID_PC (0x50) faults and display glitches.
- dma-buf import: kbase `MEM_IMPORT` (UMM). Each import must be
  `STICKY_RESOURCE_MAP`'d (ioctl 29), or the GPU takes a translation fault.
  Imports are deduped by the ION handle (`ION_IOC_IMPORT`); a new import per
  swap exhausted GPU VA and crashed bootanimation.
- Submits are asynchronous. Every GPU atom depends (ORDER) on the previous one,
  so submits complete in order (submit seq / done seq). `WAIT_BO` and syncobj
  waits are real. Closing a busy BO is deferred until its submit is done.
- Fences: out-fences are a `BASE_JD_REQ_SOFT_FENCE_TRIGGER` soft atom on a
  kbase stream (`STREAM_CREATE`, ioctl 24), with an ORDER dep on the syncobj's
  last atom. In-fences become `FENCE_WAIT` soft atoms, so the GPU waits, not
  the CPU. `/dev/sw_sync` can't be used: it is root 0600 with the SELinux
  label `device`, and its absence caused a SystemUI hardware-bitmap NPE.
- `atrace_get_enabled_tags` is exported from libdrm-kbase. Mesa's Android stub
  `trace.h` needs it, and VNDK v29 libcutils lacks it.

## kbase facts (r29p0, Samsung)

- Handshake: `VERSION_CHECK` 11.13, `SET_FLAGS` 0, then mmap the tracking page
  (offset `3 << 12`, `PROT_NONE`). Without that, `MEM_ALLOC` returns EINVAL.
- 64-bit contexts are forced to SAME_VA. mmap the cookie with `addr = NULL`;
  any hint gives EINVAL. The mapping prot must match the CPU_RD/WR flags
  ("inconsistent VM flags" gives EPERM).
- `GPU_EX` can't be combined with `GPU_WR` or `GROW_ON_GPF`. Exec BOs are GPU
  read-only; the tiler heap is GROW_ON_GPF.
- **Samsung's atom is 64 bytes** (16 extra SRUK systrace bytes). The kernel
  rejects any other stride.
- SAME_VA lands near 8 GiB (va_bits 33).
- An atom number is freed only when its event is read. Submitting a busy number
  blocks inside the ioctl, so track busy ids.
- `check_flags` rejects `F_SETFL` without `O_CLOEXEC`. Open `/dev/mali0` with
  `O_NONBLOCK` instead.
- kbase's SAME_VA mapping of UMM (dma-buf) memory is not CPU-backed (SIGBUS).
  CPU-map the dma-buf fd itself.
- **Chain split**: a tiler job in the same hardware chain as its vertex job
  emits no primitives under kbase. The polygon list body is only `a0000000`,
  and there is no fault. Workaround (`DRM_KBASE_SPLIT=group`, default): two
  atoms, all non-tiler jobs then all tiler jobs, with the dependency fields
  that cross the two cleared. Root cause unknown. `none` (whole chain in one
  atom) still misrenders. Per-atom GPU cost is about 85 us, so per-job atoms
  (`job`) are slow.

## Gralloc

The stock 7570 gralloc is Samsung's Mali gralloc3. The handle layout matches
samsungexynos9810 `hardware_samsung_slsi exynos/gralloc3 mali_gralloc_buffer.h`:

- `union { share_fd, fds[5] }`, 272 bytes, numFds + numInts = 65;
- magic `0x3141592` at byte 32, plane_info at 88, sizes at 136, bases at 160,
  offset at 232;
- the same layout in 32- and 64-bit.

The allocator is a binderized 32-bit service and stays stock. Only a 64-bit
*mapper* module is needed. It is named "CrOS Gralloc" and implements
`CROS_GRALLOC_DRM_GET_BUFFER_INFO`, so Mesa's u_gralloc gets fd, offset, stride
and fourcc.

YUV formats it maps:

- Exynos HAL NV12: 0x105, 0x107, 0x120, 0x121, 0x123.
- NV21: 0x11, 0x11D, 0x11E.
- YV12: 0x11C.
- Mali internal (base 0x200): 0x200 NV12, 0x201 NV21.

The camera preview buffer is 0x11D: NV21 with two ION fds (Y and CbCr).

Unmapped formats log `GET_BUFFER_INFO: unmapped format`.

For single-fd NV21 (0x11), only `plane_info[0]` is filled. `lock_ycbcr` uses
chroma = Y + stride * height and cstride = stride, like the stock 32-bit
mapper. Without that, Camera2 JPEGs have wrong colours.

## Performance

The stock Mali DVFS table only leaves 415 MHz above 98 % utilisation, which
Mesa's submit pattern never reaches. `gpu-dvfs.rc` sets the floor to 830 MHz.
553 to 830 MHz all run at 900 mV.

| Workload | Before | After |
|---|---|---|
| Launcher swipe, p50 frame | 57 ms (stock DVFS, sync submit) | 8 ms (830 MHz floor, async submit) |
| Launcher swipe, janky frames | 100 % | 0.34 % |
| `glstress`, 10 draws | 137 fps (sync) | 226 fps (async) |
| `glstress`, 100 draws | 42 fps (sync) | 108 fps (async) |

## Known issues

- 64-bit apps get OpenGL ES 2.0 (Mesa); 32-bit apps get ES 3.2 (Mali blob).
- S-BOOT forces `ro.boot.selinux=enforcing`. Some SELinux denials remain.
- The root cause of the chain-split misrendering is unknown.
