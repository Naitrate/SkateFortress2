#!/usr/bin/env bash
# Build only the TF2 client/server DLLs of this Source SDK inside Valve's Steam Runtime
# (sniper) container. Like src/buildallprojects, minus HL2MP and the TTY.
# Usage: ./build-sdk.sh [release|debug]
#
# The game's client.so/server.so are replaced by rename, never overwritten in
# place, so rebuilding while the mod is running cannot crash it (SIGBUS); the
# running game simply keeps the old copy until restarted.
set -euo pipefail

mode="${1:-release}"
here="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)"
ccache_dir="${CCACHE_DIR:-$HOME/.ccache}"
mkdir -p "$ccache_dir"

podman run --rm \
    --userns=keep-id \
    --env "CCACHE_DIR=$ccache_dir" \
    --env "VPC_NINJA_BUILD_MODE=$mode" \
    --mount "type=bind,source=$ccache_dir,target=$ccache_dir" \
    --mount "type=bind,source=$here,target=/my_mod" \
    --workdir /my_mod/src \
    registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest \
    bash -c '
        set -euo pipefail
        out="_vpc_/ninja/sdk_tf_$VPC_NINJA_BUILD_MODE"
        if [[ ! -e "$out.ninja" ]]; then
            devtools/bin/vpc /tf /linux64 /ninja /define:SOURCESDK +everything /mksln "$out"
            ninja -f "$out.ninja" -t compdb > compile_commands.json
            sed -i "s/-fpredictive-commoning//g; s/-fvar-tracking-assignments//g; s|/my_mod/src|.|g" compile_commands.json
        fi
        ninja -f "$out.ninja" -j"$(nproc)" \
            "_vpc_/ninja/$VPC_NINJA_BUILD_MODE/objs/client_tf/game/client/client.so" \
            "_vpc_/ninja/$VPC_NINJA_BUILD_MODE/objs/server_tf/game/server/server.so"
    '

bin="$here/game/mod_tf/bin/linux64"
mkdir -p "$bin"
for lib in client:client_tf/game/client server:server_tf/game/server; do
    name="${lib%%:*}"
    built="$here/src/_vpc_/ninja/$mode/objs/${lib#*:}/$name.so"
    cp "$built" "$bin/.$name.so.new"
    mv -f "$bin/.$name.so.new" "$bin/$name.so"
done
echo "Installed client.so and server.so into $bin"
