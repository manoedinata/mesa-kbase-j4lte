# mesa-kbase-j4lte

64-bit OpenGL ES for the Samsung Galaxy J4 (SM-J400F, `j4lte`, Exynos 7570,
Mali-T720) on the **stock Samsung kernel and its kbase r29p0 Mali driver**.

The stock vendor partition only ships 32-bit Mali blobs. Arm's 64-bit blobs
don't fit this kernel, so an arm64 Android ROM (GSI or a LineageOS build) has no
64-bit EGL/GLES and cannot boot. This repo builds the missing pieces:

| File (in `/vendor`) | What it is |
|---|---|
| `lib64/egl/libEGL_mesa.so`, `libGLESv1_CM_mesa.so`, `libGLESv2_mesa.so`, `lib64/libgallium_dri.so`, `lib64/libglapi.so` | Mesa 24.3.4, Panfrost Gallium driver, Android platform (OpenGL ES 3.0 on T720) |
| `lib64/libdrm.so` | **libdrm-kbase**: a libdrm replacement that presents `/dev/mali0` (kbase) as a panfrost DRM render node |
| `lib64/hw/gralloc.exynos7570.so` | 64-bit gralloc0 mapper module for Samsung's Mali gralloc3 buffers |
| `lib64/libc++_shared.so` | from NDK r21e (Mesa needs it) |
| `etc/init/gpu-dvfs.rc` | Mali DVFS floor (830 MHz) |

Ready-made images: a flashable vendor zip is published as release
[`stock-arm64`](https://github.com/manoedinata/proprietary_vendor_samsung_j4lte/releases/tag/stock-arm64)
of `manoedinata/proprietary_vendor_samsung_j4lte`. Its binaries are built by
this repo; `./build.sh` reproduces all of them byte for byte.

## Build

Needs Docker and about 4 GB of disk.

```sh
./build.sh
```

The script downloads Android NDK r21e (1.1 GB), Mesa 24.3.4 and libdrm 2.4.123
into `work/`, checking their sha256. To skip the NDK download, put
`android-ndk-r21e-linux-x86_64.zip` in `work/` first.
The result is staged in `out/vendor/` with `out/SHA256SUMS`.

Steps it runs in the container (`docker/Dockerfile`, Debian bookworm, meson 1.5.2):

1. upstream libdrm 2.4.123, built only for its headers and pkg-config file;
2. Mesa 24.3.4 with `patches/mesa/*.patch`, `-Dgallium-drivers=panfrost -Dplatforms=android -Dandroid-stub=true -Dplatform-sdk-version=30`;
3. `libdrm-kbase/libdrm_kbase.c` → `libdrm.so`;
4. `gralloc64/gralloc_exynos7570_64.c` → `gralloc.exynos7570.so`;
5. staging into `out/vendor`.

`./build.sh shell` opens a shell in the build container.
The work tree is mounted at `/w`, because that path is baked into the Mesa
binaries (a build-tree RUNPATH). Keep it to get identical output.

## Install on the vendor partition

Besides the files above, the stock vendor needs these changes (all in the
`stock-arm64` branch of the vendor repo):

- **32-bit EGL**: `lib/egl/libGLES_mesa.so` → symlink to `libGLES_mali.so`.
  The Android 11 EGL loader has no wildcard fallback, so the 32-bit side also
  needs a file that matches `ro.hardware.egl`. 32-bit apps keep the Mali blob (ES 3.2).
- **64-bit mapper**: `lib64/hw/android.hardware.graphics.mapper@2.0-impl.so`,
  the AOSP 64-bit mapper passthrough. It is not built here; ours came from a
  Samsung H-VENDOR. The stock 32-bit binderized allocator service stays.
- `build.prop`: `ro.hardware.egl=mesa`, `ro.opengles.version=131072`,
  `ro.hardware.gralloc=exynos7570`.
- For ES 3.0 apps: `ro.opengles.version=196608`. Mesa reports ES 3.0 on its
  own since `patches/mesa/0003` (emulated multiple render targets, see
  [docs/MRT-RE.md](docs/MRT-RE.md)). `mesa.gles.version.override` is no longer
  needed; if used, the key must be the bare `mesa.*` form: Mesa builds the
  property name in a 32-byte buffer, so the `debug.`/`vendor.` forms are
  truncated.
- `default.prop`: `ro.zygote=zygote64_32`. With `zygote32`, system_server dies
  at boot_completed because there is no secondary zygote.
- SELinux label for all new libs: `u:object_r:same_process_hal_file:s0`.
- The boot image must not use dm-verity on `/vendor` (remove `,verify` from
  the fstab entries in the DTB).

## Tests

`tests/` holds small programs used during bring-up. Build them with the NDK
clang (`aarch64-linux-android30-clang -fuse-ld=lld`) against the Mesa build:

- `egltest_mesa.c`: EGL surfaceless clear + triangle, with pixel readback;
- `glstress.c`: draw-call throughput (`glstress [frames] [draws]`; `NOFINAL`/`NOCLEAR` env give correctness grids);
- `hwbtest.c`: AHardwareBuffer allocate/lock/plane layout through the mapper;
- `gralloc64/gralloc_test.c`: ION buffer, CPU write, GPU import, GPU clear, CPU readback.
- `gles3test.c`: ES 3.0 feature probe (ESSL 3.00, integer/float textures, instancing,
  transform feedback, UBO, array textures, MSAA, queries, primitive restart, MRT).
  `GLES3TEST_ONLY=<name>` runs one subtest.
- `mrtprobe.c`: one MRT scenario per run (`draw`, `rt3`, `rt4`, `clear`, `clrdraw`,
  `blend`, `depth`, `mixdepth`, `mixed2`, `int`, `sizes`, `level`, `msaa`).
- Build both with `./build.sh tests`.

## Runtime knobs (libdrm-kbase)

- `DRM_KBASE_DEBUG=1`: verbose log (logcat tag `drm-kbase`).
- `DRM_KBASE_SPLIT=group|job|none`: how a vertex/tiler job chain becomes
  kbase atoms. The default is `group`; `none` misrenders.
- `DRM_KBASE_LOW_VA=1`: reserve [4 GiB, 8 GiB) so BOs land below 4 GiB.
- `setprop debug.gpu.min_lock <MHz>`: change the DVFS floor at runtime
  (350 415 553 666 830; 0 removes it).

## Status

Tested on SM-J400F with Lineage 18.1 (an arm64 GSI and a j4lte device build).
SurfaceFlinger runs on "Mali-T720 (Panfrost), OpenGL ES 2.0 Mesa 24.3.4".
Apps, camera preview and photos work, with no GPU faults seen.
Launcher frame p50 is 8 ms. See [docs/NOTES.md](docs/NOTES.md) for how it
works and for the kbase details learned on the way.

`patches/mesa-linux-prototype/` is the first prototype: a Mesa drm-shim
(LD_PRELOAD) that ran Panfrost on kbase under glibc Linux. It is kept for
reference only and is not used by `build.sh`.

## License

`libdrm-kbase` is MIT, like libdrm/Mesa. The Mesa patches follow Mesa's license.
