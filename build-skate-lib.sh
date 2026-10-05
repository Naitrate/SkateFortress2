#!/usr/bin/env bash
# Build libskate3.so (the Skate 3 simulation, skate-engine crates/skate3-lib)
# inside Valve's Steam Runtime (sniper) container and install it beside
# client.so/server.so in game/mod_tf/bin/linux64. server.so loads it.
#
# Built against sniper's glibc, it runs inside the Steam runtime on any
# current distro. It only links libc, libm and libgcc_s.
#
# Usage: ./build-skate-lib.sh
#
# Rust lives in a cache outside the container (rustup on first run needs
# internet). Large outputs go to SKATE_BUILD_CACHE, default
# ~/.cache/tf2-skate; point it at a roomy disk.
set -euo pipefail

here="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)"
cache="${SKATE_BUILD_CACHE:-$HOME/.cache/tf2-skate}"
mkdir -p "$cache/rustup" "$cache/cargo" "$cache/target-sniper"

podman run --rm \
    --userns=keep-id \
    --env "RUSTUP_HOME=/cache/rustup" \
    --env "CARGO_HOME=/cache/cargo" \
    --env "CARGO_TARGET_DIR=/cache/target-sniper" \
    --env "CARGO_PROFILE_RELEASE_DEBUG=0" \
    --env "CARGO_PROFILE_RELEASE_STRIP=symbols" \
    --mount "type=bind,source=$cache,target=/cache" \
    --mount "type=bind,source=$here/skate-engine,target=/src" \
    --workdir /src \
    registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest \
    bash -c '
        set -euo pipefail
        if [[ ! -x /cache/cargo/bin/cargo ]]; then
            curl -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal --default-toolchain stable
        fi
        export PATH="/cache/cargo/bin:$PATH"
        cargo build --release --locked -p skate3-lib
    '

bin="$here/game/mod_tf/bin/linux64"
mkdir -p "$bin"
# Rename into place: a running game keeps its mapped copy (no SIGBUS).
cp "$cache/target-sniper/release/libskate3.so" "$bin/.libskate3.so.new"
mv -f "$bin/.libskate3.so.new" "$bin/libskate3.so"
rm -f "$bin/skate3-sidecar"   # the old out-of-process sidecar
echo "Installed libskate3.so ($(du -h "$bin/libskate3.so" | cut -f1)) into $bin"
