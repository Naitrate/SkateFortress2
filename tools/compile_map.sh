#!/usr/bin/env bash
# Compile a .vmf into game/mod_tf/maps with the Linux builds of vbsp, vvis
# and vrad (src/utils, built by tools/build_maptools.sh), inside Valve's
# Steam Runtime (sniper) container. vrad lights the map twice, LDR then HDR,
# like the Windows launcher's -both.
#
#   tools/compile_map.sh game/mod_tf/mapsrc/skate_park.vmf
#   SKATE_MAP_FAST=1 tools/compile_map.sh ...   (quick vis and lighting)
#
# vbsp needs TF2's runtime libraries beside it (filesystem_stdio, vphysics,
# tier0, vstdlib) and the game's content to look up materials. TF2 and the
# Source SDK Base are found under TF2_DIR / SDKBASE_DIR (default: the Steam
# library holding TF2). Scratch files go to SKATE_BUILD_CACHE.
set -euo pipefail

here="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")/.." && pwd)"
vmf="$(readlink -f -- "${1:?usage: tools/compile_map.sh path/to/map.vmf}")"
name="$(basename "$vmf" .vmf)"
steam="${STEAM_LIBRARY:-/mnt/NVME Storage/Steam/steamapps/common}"
tf2="${TF2_DIR:-$steam/Team Fortress 2}"
sdkbase="${SDKBASE_DIR:-$steam/Source SDK Base 2013 Multiplayer}"
cache="${SKATE_BUILD_CACHE:-$HOME/.cache/tf2-skate}"
tools="$here/game/bin/linux64"
for tool in vbsp vvis vrad; do
    [[ -x "$tools/$tool" ]] || { echo "No $tools/$tool: build it with tools/build_maptools.sh" >&2; exit 1; }
done
fast=""
[[ -n "${SKATE_MAP_FAST:-}" ]] && fast="-fast"
[[ -f "$tf2/bin/linux64/libtier0.so" ]] || { echo "TF2 not found at $tf2 (set TF2_DIR)" >&2; exit 1; }

work="$cache/mapcompile"
rm -rf "$work"
mkdir -p "$work/bin/linux64" "$work/game"
# The tools load their modules (filesystem_stdio, vphysics) from their own
# folder, which must be a bin/linux64.
cp "$tools/vbsp" "$tools/vvis" "$tools/vrad" "$work/bin/linux64/"
# Inside the container TF2, the SDK Base and the mod sit under /work in
# lowercase: Valve's Linux filesystem lowercases paths outside its base
# folder, so "/mnt/NVME Storage/..." would become "/mnt/Nvme storage/...".
for lib in "$tf2/bin/linux64/"*.so; do ln -sf "/work/tf2/bin/linux64/$(basename "$lib")" "$work/bin/linux64/"; done
# The mod's gameinfo with Steam app ids resolved to the install folders.
# Values are quoted, since the folders may hold spaces.
python3 - "$here/game/mod_tf/gameinfo.txt" "$work/game/gameinfo.txt" /work/tf2 /work/sdkbase /work/mod <<'PY'
import re, sys
src, dst, tf2, sdkbase, mod = sys.argv[1:]
subs = {"|appid_440|": tf2 + "/", "|appid_243750|": sdkbase + "/", "|gameinfo_path|": mod + "/"}
out = []
for line in open(src, encoding="latin-1"):
    m = re.match(r'^(\s*)(\S+)(\s+)(\|[^|]+\|)(\S*)\s*$', line)
    if m and m.group(4) in subs:
        line = f'{m.group(1)}{m.group(2)}{m.group(3)}"{subs[m.group(4)]}{m.group(5)}"\n'
    out.append(line)
open(dst, "w", encoding="latin-1").writelines(out)
PY
cp "$vmf" "$work/$name.vmf"

podman run --rm --userns=keep-id \
    --mount "type=bind,source=$work,target=/work" \
    --mount "type=bind,source=$here/game/mod_tf,target=/work/mod,readonly" \
    --mount "type=bind,source=$tf2,target=/work/tf2,readonly" \
    --mount "type=bind,source=$sdkbase,target=/work/sdkbase,readonly" \
    --workdir /work \
    registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest \
    bash -c "set -e; export LD_LIBRARY_PATH=/work/bin/linux64; cd /work/bin/linux64
        ./vbsp -game /work/game /work/$name.vmf
        ./vvis $fast -game /work/game /work/$name.bsp
        ./vrad $fast -ldr -game /work/game /work/$name.bsp
        ./vrad $fast -hdr -game /work/game /work/$name.bsp"

mkdir -p "$here/game/mod_tf/maps"
cp "$work/$name.bsp" "$here/game/mod_tf/maps/$name.bsp"
echo "Installed game/mod_tf/maps/$name.bsp ($(du -h "$here/game/mod_tf/maps/$name.bsp" | cut -f1))"
