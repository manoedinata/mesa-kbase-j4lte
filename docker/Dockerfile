# Build environment for the Android (NDK r21e) Mesa + libdrm-kbase + gralloc
# build. The NDK itself is downloaded by build.sh and mounted at /ndk.
FROM debian:bookworm-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential ninja-build pkg-config python3 python3-pip python3-mako \
        python3-yaml python3-packaging bison flex git ca-certificates curl \
        xz-utils unzip file patch \
 && pip3 install --break-system-packages 'meson==1.5.2' \
 && rm -rf /var/lib/apt/lists/*
