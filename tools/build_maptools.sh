#!/usr/bin/env bash
# Build the Linux map compilers (vbsp, vvis and vrad, from src/utils) inside
# Valve's Steam Runtime (sniper) container, into game/bin/linux64. Valve's SDK
# only builds them for Windows; the Linux port is marked "tf2-skate" in the
# sources (pthreads for the tool threads, materials read from the .vmt/.vtf
# instead of starting the material system, case-correct includes, plain
# executables instead of vvis/vrad's DLL and launcher, no VMPI).
#
#   tools/build_maptools.sh
#
# Then compile maps with tools/compile_map.sh.
set -euo pipefail

here="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")/.." && pwd)"
podman run --rm --userns=keep-id \
    --env VPC_NINJA_BUILD_MODE=release \
    --mount "type=bind,source=$here,target=/my_mod" \
    --workdir /my_mod/src \
    registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest \
    bash -c '
        set -euo pipefail
        devtools/bin/vpc /tf /linux64 /ninja /define:SOURCESDK +vbsp +vvis_dll +vrad_dll +fgdlib /mksln _vpc_/ninja/maptools_release >/dev/null
        ninja -f _vpc_/ninja/maptools_release.ninja -j"$(nproc)" -k 0
    '
echo "Built game/bin/linux64/vbsp, vvis and vrad"
