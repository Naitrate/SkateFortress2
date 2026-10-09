#!/usr/bin/env bash
# Build the Linux map compiler (vbsp, from src/utils/vbsp) inside Valve's
# Steam Runtime (sniper) container, into game/bin/linux64/vbsp. Valve's SDK
# only builds it for Windows; the Linux port is marked "tf2-skate" in the
# sources (pthreads for the tool threads, materials read from the .vmt/.vtf
# instead of starting the material system, case-correct includes).
#
#   tools/build_maptools.sh
#
# Then compile maps with tools/compile_map.sh. vvis and vrad aren't ported
# yet, so maps come out unlit (fullbright) and without visibility culling.
set -euo pipefail

here="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")/.." && pwd)"
podman run --rm --userns=keep-id \
    --env VPC_NINJA_BUILD_MODE=release \
    --mount "type=bind,source=$here,target=/my_mod" \
    --workdir /my_mod/src \
    registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest \
    bash -c '
        set -euo pipefail
        devtools/bin/vpc /tf /linux64 /ninja /define:SOURCESDK +vbsp +fgdlib /mksln _vpc_/ninja/maptools_release >/dev/null
        ninja -f _vpc_/ninja/maptools_release.ninja -j"$(nproc)"
    '
echo "Built $(ls -la "$here/game/bin/linux64/vbsp" | awk "{print \$5}") byte game/bin/linux64/vbsp"
