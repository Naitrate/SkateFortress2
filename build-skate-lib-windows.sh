#!/usr/bin/env bash
# Build skate3.dll (the Skate 3 simulation for Windows players, same crate as
# libskate3.so) by cross-compiling from Linux with MinGW-w64, and install it in
# game/mod_tf/bin/x64 beside the Windows client.dll/server.dll.
#
# Usage: ./build-skate-lib-windows.sh
#
# Uses the same Rust cache as build-skate-lib.sh (SKATE_BUILD_CACHE; point it
# at a roomy disk). The first run builds a small Debian container with
# MinGW (tools/containers/windows-cross.Containerfile).
set -euo pipefail

here="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)"
cache="${SKATE_BUILD_CACHE:-$HOME/.cache/tf2-skate}"
image="localhost/tf2-skate-windows-cross:2"
mkdir -p "$cache/rustup" "$cache/cargo" "$cache/target-windows"

if ! podman image exists "$image"; then
    podman build -t "$image" -f "$here/tools/containers/windows-cross.Containerfile" "$here/tools/containers"
fi

# Rootless podman: root in the container is you outside it.
podman run --rm \
    --env "RUSTUP_HOME=/cache/rustup" \
    --env "CARGO_HOME=/cache/cargo" \
    --env "CARGO_TARGET_DIR=/cache/target-windows" \
    --env "CARGO_PROFILE_RELEASE_DEBUG=0" \
    --env "CARGO_PROFILE_RELEASE_STRIP=symbols" \
    --env "CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc" \
    --mount "type=bind,source=$cache,target=/cache" \
    --mount "type=bind,source=$here/skate-engine,target=/src" \
    --workdir /src \
    "$image" \
    bash -c '
        set -euo pipefail
        if [[ ! -x /cache/cargo/bin/cargo ]]; then
            curl -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal --default-toolchain stable
        fi
        export PATH="/cache/cargo/bin:$PATH"
        rustup target add x86_64-pc-windows-gnu >/dev/null
        cargo build --release --locked -p skate3-lib --target x86_64-pc-windows-gnu
        echo "skate3.dll imports:"
        x86_64-w64-mingw32-objdump -p /cache/target-windows/x86_64-pc-windows-gnu/release/skate3.dll | sed -n "s/^\s*DLL Name: /  /p"
    '

bin="$here/game/mod_tf/bin/x64"
mkdir -p "$bin"
cp "$cache/target-windows/x86_64-pc-windows-gnu/release/skate3.dll" "$bin/.skate3.dll.new"
mv -f "$bin/.skate3.dll.new" "$bin/skate3.dll"
echo "Installed skate3.dll ($(du -h "$bin/skate3.dll" | cut -f1)) into $bin"
