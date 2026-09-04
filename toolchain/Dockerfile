# Fun Drastic build toolchain.
#
# One image, both cross-compilers, and the SDL2 headers the hook compiles
# against. Build it once:
#
#     docker build -t fundrastic-build toolchain/
#
# The Makefile runs every target inside this image. See docs/BUILDING.md.
#
# The hook (libfundrastic.so) never links SDL2 — it resolves DraStic's own SDL2
# symbols at run time via LD_PRELOAD. It only needs SDL2's public headers to
# compile, so a single cross-built SDL2 header tree serves both the aarch64 and
# the armhf lane; the video/audio backends are irrelevant and left off.

FROM debian:bullseye

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
      build-essential \
      gcc-aarch64-linux-gnu \
      g++-aarch64-linux-gnu \
      gcc-arm-linux-gnueabihf \
      g++-arm-linux-gnueabihf \
      pkg-config \
      wget \
      ca-certificates \
      make \
    && rm -rf /var/lib/apt/lists/*

# SDL2 public headers (built for aarch64; headers are what the hook consumes).
# Installed under /usr/local/aarch64-linux-gnu — the include path both lanes use.
# Every backend is disabled: nothing from this SDL2 is shipped, only its headers.
RUN wget -q https://github.com/libsdl-org/SDL/releases/download/release-2.28.5/SDL2-2.28.5.tar.gz \
    && tar xf SDL2-2.28.5.tar.gz \
    && cd SDL2-2.28.5 \
    && ./configure --host=aarch64-linux-gnu \
         --prefix=/usr/local/aarch64-linux-gnu \
         --enable-shared --enable-static \
         --disable-video-opengl --disable-video-x11 --disable-video-wayland \
         --disable-video-vulkan --disable-video-kmsdrm --disable-video-dummy \
         --disable-video-rpi --disable-pulseaudio --disable-esd --disable-jack \
         --disable-oss --disable-alsa --disable-libsamplerate \
    && make -j"$(nproc)" \
    && make install \
    && cd .. && rm -rf SDL2-2.28.5 SDL2-2.28.5.tar.gz

WORKDIR /workspace
CMD ["/bin/bash"]
