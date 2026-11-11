#!/bin/sh
# Build blobspy (32-bit Android LD_PRELOAD) and blobdecode (host) in the
# build container. Needs ./build.sh to have run once (NDK, Mesa source).
# Output: out/blobtrace/{libblobspy.so,blobdecode}
set -eu
TOP=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$TOP/out/blobtrace"
docker run --rm -u "$(id -u):$(id -g)" -v "$TOP:/src:ro" -v "$TOP/work:/w" \
    -v "$TOP/work/ndk:/ndk:ro" -v "$TOP/out:/out" -w /w mesa-kbase-j4lte:bookworm sh -ec '
T=/ndk/android-ndk-r21e/toolchains/llvm/prebuilt/linux-x86_64/bin
$T/armv7a-linux-androideabi30-clang -O2 -Wall -fPIC -shared -fuse-ld=lld \
  -o /out/blobtrace/libblobspy.so /src/tools/blobtrace/blobspy.c -ldl

M=/w/mesa-24.3.4
if [ ! -f $M/build-host/build.ninja ]; then
  (cd /w/libdrm-2.4.123 && meson setup build-host --prefix=/w/host-drm --libdir=lib \
    -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled \
    -Dfreedreno=disabled -Dvc4=disabled -Detnaviv=disabled -Dexynos=disabled -Domap=disabled \
    -Dtegra=disabled -Dtests=false -Dman-pages=disabled -Dvalgrind=disabled -Dcairo-tests=disabled \
    -Dudev=false >/dev/null && ninja -C build-host install >/dev/null)
  (cd $M && PKG_CONFIG_PATH=/w/host-drm/lib/pkgconfig meson setup build-host --buildtype=debug -Db_ndebug=true \
    -Dshader-cache=disabled -Dplatforms= -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dglx=disabled \
    -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled \
    -Dzlib=disabled -Dexpat=disabled -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
    -Dbuild-tests=false -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled \
    -Dgallium-nine=false -Dgallium-opencl=disabled -Dgallium-rusticl=false -Dshared-glapi=disabled >/dev/null)
fi
B=$M/build-host
LIBS="src/panfrost/lib/genxml/libpanfrost_decode.a
src/panfrost/lib/genxml/libpandecode-arch-v4.a src/panfrost/lib/genxml/libpandecode-arch-v5.a
src/panfrost/lib/genxml/libpandecode-arch-v6.a src/panfrost/lib/genxml/libpandecode-arch-v7.a
src/panfrost/lib/genxml/libpandecode-arch-v9.a src/panfrost/lib/genxml/libpandecode-arch-v10.a
src/panfrost/midgard/libpanfrost_midgard_disasm.a src/panfrost/midgard/libpanfrost_midgard.a
src/panfrost/util/libpanfrost_util.a src/panfrost/shared/libpanfrost_shared.a
src/compiler/nir/libnir.a src/compiler/libcompiler.a src/compiler/glsl/libglsl_util.a
src/util/libmesa_util.a src/util/libmesa_util_sse41.a src/c11/impl/libmesa_util_c11.a
src/util/blake3/libblake3.a src/util/libxmlconfig.a"
ninja -C $B $LIBS >/dev/null
gcc -O1 -g -o /out/blobtrace/blobdecode /src/tools/blobtrace/blobdecode.c \
  -Wl,--start-group $(for l in $LIBS; do echo $B/$l; done) -Wl,--end-group -lm -lpthread -ldl
'
ls -la "$TOP/out/blobtrace"
