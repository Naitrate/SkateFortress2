#!/usr/bin/env bash
# Build libvgmstream.so (vgmstream with a trimmed, statically linked FFmpeg for
# Skate 3's EA-XMA audio) inside Valve's Steam Runtime (sniper) container and
# install it beside libskate3.so in game/mod_tf/bin/linux64.
# With --windows, cross-compile libvgmstream.dll with MinGW-w64 instead and
# install it beside skate3.dll in game/mod_tf/bin/x64.
#
# libskate3 loads it during the in-game first-time setup to decode the
# disc's sounds. Only needed again when updating vgmstream.
#
# Usage: ./build-vgmstream.sh [--windows]
# Sources (fetched once) and build output go to SKATE_BUILD_CACHE, default
# ~/.cache/tf2-skate; point it at a roomy disk.
set -euo pipefail

here="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)"
windows=0
[[ "${1:-}" == "--windows" ]] && windows=1
cache="${SKATE_BUILD_CACHE:-$HOME/.cache/tf2-skate}"
src="$cache/thirdparty"
mkdir -p "$src"
[[ -d "$src/vgmstream" ]] || git clone --depth 1 https://github.com/vgmstream/vgmstream.git "$src/vgmstream"
[[ -d "$src/ffmpeg" ]] || git clone --depth 1 --branch n7.1 https://github.com/FFmpeg/FFmpeg.git "$src/ffmpeg"

# vgmstream builds FFmpeg statically; it is linked into a shared library, so
# it must be position independent. The container has no nasm: use C paths.
conf="$src/vgmstream/cmake/dependencies/ffmpeg.cmake"
# Use FFmpeg's own Opus decoder, never the system libopus, so the library
# needs nothing beyond libc/libm.
sed -i 's|set(USE_FFMPEG_LIBOPUS ON)|set(USE_FFMPEG_LIBOPUS OFF)|' "$conf"
grep -q -- "--enable-pic" "$conf" || sed -i 's|\t\t\t\t--disable-libdrm|\t\t\t\t--disable-libdrm\n\t\t\t\t--enable-pic\n\t\t\t\t--disable-x86asm|' "$conf"

if (( windows )); then
    # vgmstream only builds its own FFmpeg off Windows (on Windows it expects
    # prebuilt DLLs); let MinGW cross builds do the same, and use that FFmpeg.
    sed -i 's|if(USE_FFMPEG AND NOT WIN32 AND (NOT FFmpeg_FOUND|if(USE_FFMPEG AND (NOT WIN32 OR MINGW) AND (NOT FFmpeg_FOUND|' "$conf"
    grep -q -- "--cross-prefix=x86_64-w64-mingw32-" "$conf" || sed -i 's|^\t\t\tset(FFMPEG_LINK_PATH |\t\t\tif(MINGW)\n\t\t\t\tlist(APPEND FFMPEG_CONF_ARGS --enable-cross-compile --cross-prefix=x86_64-w64-mingw32- --target-os=mingw32 --arch=x86_64)\n\t\t\tendif()\n\t\t\tset(FFMPEG_LINK_PATH |' "$conf"
    sed -i 's|if(WIN32 AND NOT FFMPEG_LIBRARIES)$|if(WIN32 AND NOT FFMPEG_LIBRARIES AND NOT FFMPEG_COMPILE)|' "$src/vgmstream/cmake/vgmstream.cmake"
    # Its DLL export macro only knows MSVC; GCC on Windows needs dllexport too.
    sed -i 's|^#if defined(_MSC_VER) \|\| defined(__CYGWIN__)$|#if defined(_WIN32) \|\| defined(__CYGWIN__)|' "$src/vgmstream/src/libvgmstream.h"

    image="localhost/tf2-skate-windows-cross:2"
    if ! podman image exists "$image"; then
        podman build -t "$image" -f "$here/tools/containers/windows-cross.Containerfile" "$here/tools/containers"
    fi
    # Rootless podman: root in the container is you outside it. The runtime
    # (libgcc, winpthreads) is linked in, so the DLL needs only Windows' own.
    podman run --rm \
        --mount "type=bind,source=$cache,target=/cache" \
        --workdir /cache \
        "$image" \
        bash -c '
            set -euo pipefail
            cmake -S thirdparty/vgmstream -B vgmstream-build-windows \
                -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
                -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
                -DCMAKE_SHARED_LINKER_FLAGS="-static-libgcc -static" -DCMAKE_C_STANDARD_LIBRARIES="-lbcrypt" \
                -DBUILD_SHARED_LIBS=ON -DBUILD_CLI=OFF -DBUILD_V123=OFF -DBUILD_AUDACIOUS=OFF -DBUILD_WINAMP=OFF \
                -DUSE_MPEG=OFF -DUSE_VORBIS=OFF -DUSE_G7221=OFF -DUSE_G719=OFF \
                -DUSE_ATRAC9=OFF -DUSE_CELT=OFF -DUSE_SPEEX=OFF \
                -DUSE_FFMPEG=ON -DFFMPEG_PATH=/cache/thirdparty/ffmpeg
            cmake --build vgmstream-build-windows --target libvgmstream_shared -j"$(nproc)"
            built="$(find vgmstream-build-windows -name "libvgmstream.dll" -type f | head -1)"
            x86_64-w64-mingw32-strip --strip-unneeded "$built"
            echo "libvgmstream.dll imports:"
            x86_64-w64-mingw32-objdump -p "$built" | sed -n "s/^\s*DLL Name: /  /p"
        '
    built="$(find "$cache/vgmstream-build-windows" -name "libvgmstream.dll" -type f | head -1)"
    bin="$here/game/mod_tf/bin/x64"
    mkdir -p "$bin"
    cp "$built" "$bin/.libvgmstream.dll.new"
    mv -f "$bin/.libvgmstream.dll.new" "$bin/libvgmstream.dll"
    echo "Installed libvgmstream.dll ($(du -h "$bin/libvgmstream.dll" | cut -f1)) into $bin"
    exit 0
fi

podman run --rm \
    --userns=keep-id \
    --mount "type=bind,source=$cache,target=/cache" \
    --workdir /cache \
    registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest \
    bash -c '
        set -euo pipefail
        # vgmstream needs C23-era GCC; sniper ships gcc-14 beside the default 10.
        cmake -S thirdparty/vgmstream -B vgmstream-build \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-14 \
            -DBUILD_SHARED_LIBS=ON -DBUILD_CLI=OFF -DBUILD_V123=OFF -DBUILD_AUDACIOUS=OFF \
            -DUSE_MPEG=OFF -DUSE_VORBIS=OFF -DUSE_G7221=OFF -DUSE_G719=OFF \
            -DUSE_ATRAC9=OFF -DUSE_CELT=OFF -DUSE_SPEEX=OFF \
            -DUSE_FFMPEG=ON -DFFMPEG_PATH=/cache/thirdparty/ffmpeg
        cmake --build vgmstream-build --target libvgmstream_shared -j"$(nproc)"
    '

built="$(find "$cache/vgmstream-build" -name "libvgmstream.so*" -type f | head -1)"
bin="$here/game/mod_tf/bin/linux64"
mkdir -p "$bin"
strip --strip-unneeded -o "$bin/.libvgmstream.so.new" "$built" 2>/dev/null || cp "$built" "$bin/.libvgmstream.so.new"
mv -f "$bin/.libvgmstream.so.new" "$bin/libvgmstream.so"
echo "Installed libvgmstream.so ($(du -h "$bin/libvgmstream.so" | cut -f1)) into $bin"
