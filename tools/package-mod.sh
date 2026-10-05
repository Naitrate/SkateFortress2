#!/usr/bin/env bash
# Package the mod folder for players: game/mod_tf as the SDK repo would
# commit it (tracked + not ignored), plus one platform's binaries. Skate 3's
# converted data is never included; each player's in-game setup makes it from
# their own disc.
#
#   tools/package-mod.sh linux   [out.zip]   bin/linux64: client.so server.so libskate3.so libvgmstream.so
#   tools/package-mod.sh windows [out.zip]   bin/x64: client.dll server.dll skate3.dll libvgmstream.dll
#
# Windows client.dll/server.dll come from the SDK fork's "Windows build"
# GitHub Actions run (artifact mod_tf-win64; with the gh CLI:
#   gh run download --repo Naitrate/SkateFortress2 -n mod_tf-win64 -D game/mod_tf/bin
#   (the artifact holds x64/)
# ); skate3.dll and libvgmstream.dll from ./build-skate-lib-windows.sh and
# ./build-vgmstream.sh --windows.
set -euo pipefail

here="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")/.." && pwd)"
platform="${1:-}"
case "$platform" in
    linux)   bindir=linux64; libs=(client.so server.so libskate3.so libvgmstream.so) ;;
    windows) bindir=x64;     libs=(client.dll server.dll skate3.dll libvgmstream.dll) ;;
    *) sed -n 2,17p "$0"; exit 1 ;;
esac
out="${2:-$here/tf2-skate-$platform.zip}"
mod="$here/game/mod_tf"

missing=0
for lib in "${libs[@]}"; do
    if [[ ! -f "$mod/bin/$bindir/$lib" ]]; then
        echo "missing bin/$bindir/$lib" >&2
        missing=1
    fi
done
(( missing )) && exit 1

# Linux builds carry their debug info (hundreds of MB); players get stripped
# copies. (Windows keeps it in .pdb files, which aren't packaged.)
stage="$(mktemp -d "$(dirname "$out")/.package-XXXXXX")"
trap 'rm -rf "$stage"' EXIT
for lib in "${libs[@]}"; do
    mkdir -p "$stage/game/mod_tf/bin/$bindir"
    if [[ "$platform" == linux ]]; then
        strip --strip-debug -o "$stage/game/mod_tf/bin/$bindir/$lib" "$mod/bin/$bindir/$lib"
    else
        cp "$mod/bin/$bindir/$lib" "$stage/game/mod_tf/bin/$bindir/$lib"
    fi
done

cd "$here"
{
    git ls-files -z game/mod_tf
    git ls-files -z --others --exclude-standard game/mod_tf
} | python3 -c '
import sys, zipfile, os
out = sys.argv[1]
out, stage = sys.argv[1], sys.argv[2]
paths = sorted({p for p in sys.stdin.buffer.read().decode().split("\0") if p and os.path.isfile(p)})
staged = sorted(os.path.relpath(os.path.join(d, f), stage) for d, _, fs in os.walk(stage) for f in fs)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for p in paths:
        z.write(p, p[len("game/"):])   # the zip holds mod_tf/...
    for p in staged:
        z.write(os.path.join(stage, p), p[len("game/"):])
print(f"{out}: {len(paths) + len(staged)} files, {os.path.getsize(out) / 1e6:.1f} MB")
' "$out" "$stage"
