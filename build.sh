#!/bin/sh
# Build the 64-bit GLES stack for the Samsung Galaxy J4 (j4lte, Exynos 7570,
# Mali-T720, stock kbase r29p0) and stage it as a /vendor overlay in out/vendor.
#
#   ./build.sh            download sources, build everything, stage out/vendor
#   ./build.sh shell      open a shell in the build container
#
# Needs: docker, ~1.5 GB download (NDK), ~4 GB disk. Everything goes to
# $WORK (default ./work); the result goes to ./out.
set -eu

TOP=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$TOP/work}
OUT=${OUT:-$TOP/out}
IMAGE=${IMAGE:-mesa-kbase-j4lte:bookworm}
JOBS=${JOBS:-$(nproc)}

NDK_ZIP=android-ndk-r21e-linux-x86_64.zip
NDK_URL=https://dl.google.com/android/repository/$NDK_ZIP
NDK_SHA256=ad7ce5467e18d40050dc51b8e7affc3e635c85bd8c59be62de32352328ed467e
MESA_VER=24.3.4
MESA_URL=https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz
MESA_SHA256=e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025
LIBDRM_VER=2.4.123
LIBDRM_URL=https://dri.freedesktop.org/libdrm/libdrm-$LIBDRM_VER.tar.xz
LIBDRM_SHA256=a2b98567a149a74b0f50e91e825f9c0315d86e7be9b74394dae8b298caadb79e

mkdir -p "$WORK" "$OUT"

fetch() { # url file sha256
    if [ ! -f "$WORK/$2" ]; then
        echo "== downloading $2"
        curl -fL --retry 3 -o "$WORK/$2.part" "$1"
        mv "$WORK/$2.part" "$WORK/$2"
    fi
    echo "$3  $WORK/$2" | sha256sum -c -
}

run() { # run a command in the build container
    docker run --rm -u "$(id -u):$(id -g)" \
        -v "$TOP:/src:ro" -v "$WORK:/w" -v "$WORK/ndk:/ndk:ro" -v "$OUT:/out" \
        -e JOBS="$JOBS" -w /w "$IMAGE" sh -ec "$1"
}

docker build -q -t "$IMAGE" "$TOP/docker" >/dev/null

if [ "${1:-}" = shell ]; then
    exec docker run --rm -it -u "$(id -u):$(id -g)" -v "$TOP:/src:ro" \
        -v "$WORK:/w" -v "$WORK/ndk:/ndk:ro" -v "$OUT:/out" -w /w "$IMAGE" bash
fi

fetch "$NDK_URL" "$NDK_ZIP" "$NDK_SHA256"
fetch "$MESA_URL" "mesa-$MESA_VER.tar.xz" "$MESA_SHA256"
fetch "$LIBDRM_URL" "libdrm-$LIBDRM_VER.tar.xz" "$LIBDRM_SHA256"

if [ ! -d "$WORK/ndk/android-ndk-r21e" ]; then
    echo "== unpacking NDK"
    mkdir -p "$WORK/ndk"
    (cd "$WORK/ndk" && unzip -q "../$NDK_ZIP")
fi

T=/ndk/android-ndk-r21e/toolchains/llvm/prebuilt/linux-x86_64
CC64=$T/bin/aarch64-linux-android30-clang

# 1. Upstream libdrm, built only so Mesa's meson finds libdrm headers and a
#    pkg-config file. The libdrm.so shipped on the phone is libdrm-kbase (3).
echo "== libdrm $LIBDRM_VER (headers / pkg-config for Mesa)"
run "
rm -rf libdrm-$LIBDRM_VER android-stage drm-stage
tar -xJf libdrm-$LIBDRM_VER.tar.xz
cd libdrm-$LIBDRM_VER
meson setup build-android --cross-file /src/android-arm64.ini --prefix=/opt/android-drm --libdir=lib \
  -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled \
  -Dfreedreno=disabled -Dvc4=disabled -Detnaviv=disabled -Dexynos=disabled -Domap=disabled \
  -Dtegra=disabled -Dtests=false -Dman-pages=disabled -Dvalgrind=disabled -Dcairo-tests=disabled \
  -Dudev=false >/dev/null
ninja -C build-android -j\$JOBS >/dev/null
DESTDIR=/w/android-stage ninja -C build-android install >/dev/null
"

# 2. Mesa: panfrost Gallium driver for Android with the libdrm-kbase hooks.
echo "== mesa $MESA_VER (panfrost, Android)"
run "
rm -rf mesa-$MESA_VER
tar -xJf mesa-$MESA_VER.tar.xz
cd mesa-$MESA_VER
for p in /src/patches/mesa/*.patch; do patch -p1 -s < \$p; done
PKG_CONFIG_LIBDIR=/w/android-stage/opt/android-drm/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=/w/android-stage \
meson setup build-android --cross-file /src/android-arm64.ini --prefix=/vendor --libdir=lib64 --buildtype=release \
  -Dplatforms=android -Dandroid-stub=true -Dplatform-sdk-version=30 -Dandroid-libbacktrace=disabled \
  -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dglx=disabled -Degl=enabled -Dgles1=enabled -Dgles2=enabled -Dopengl=false \
  -Degl-lib-suffix=_mesa -Dgles-lib-suffix=_mesa -Dgbm=disabled -Dllvm=disabled -Dshared-glapi=enabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dexpat=disabled -Dbuild-tests=false \
  -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled -Dgallium-nine=false \
  -Dgallium-opencl=disabled -Dgallium-rusticl=false >/dev/null
ninja -C build-android -j\$JOBS
"

# 3. libdrm-kbase: the libdrm.so that Mesa loads on the phone.
echo "== libdrm-kbase"
run "
mkdir -p build
$CC64 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fPIC -shared -fvisibility=hidden \
  -fuse-ld=lld -Wl,-soname,libdrm.so -o build/libdrm.so /src/libdrm-kbase/libdrm_kbase.c \
  -I/w/libdrm-$LIBDRM_VER -I/w/libdrm-$LIBDRM_VER/include/drm -I/w/mesa-$MESA_VER/include/drm-uapi -llog
"

# 4. 64-bit gralloc0 mapper module for Samsung's Mali gralloc3 buffers.
echo "== gralloc.exynos7570 (64-bit mapper)"
run "
$CC64 -O2 -fPIC -shared -fuse-ld=lld -Wall -Wno-unused-parameter \
  -I/w/mesa-$MESA_VER/include/android_stub -o build/gralloc.exynos7570.so \
  /src/gralloc64/gralloc_exynos7570_64.c -llog
"

# 5. Stage the vendor overlay.
echo "== staging out/vendor"
run "
B=/w/mesa-$MESA_VER/build-android
V=/out/vendor
rm -rf \$V
mkdir -p \$V/lib64/egl \$V/lib64/hw \$V/etc/init
cp \$B/src/egl/libEGL_mesa.so \$B/src/mapi/es1api/libGLESv1_CM_mesa.so \$B/src/mapi/es2api/libGLESv2_mesa.so \$V/lib64/egl/
cp \$B/src/gallium/targets/dri/libgallium_dri.so \$B/src/mapi/shared-glapi/libglapi.so \$V/lib64/
cp build/libdrm.so \$V/lib64/
cp build/gralloc.exynos7570.so \$V/lib64/hw/
cp $T/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so \$V/lib64/
cp /src/vendor/etc/init/gpu-dvfs.rc \$V/etc/init/
cd \$V && find . -type f | sort | xargs sha256sum > /out/SHA256SUMS
"
echo "== done: $OUT/vendor"
cat "$OUT/SHA256SUMS"
