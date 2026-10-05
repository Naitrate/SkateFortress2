# Cross-compiling the mod's Windows libraries (skate3.dll, libvgmstream.dll)
# from Linux with MinGW-w64. Built on demand by build-skate-lib-windows.sh and
# build-vgmstream.sh --windows; Rust itself comes from the shared cache.
FROM docker.io/library/debian:trixie
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc libc6-dev gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64 \
      cmake make git ca-certificates curl pkg-config python3 \
 && rm -rf /var/lib/apt/lists/*
