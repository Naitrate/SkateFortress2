#!/usr/bin/env bash
# Launch the TF2 Skate mod inside Steam's sniper runtime.
# Steam must be running, with TF2 (440) and Source SDK Base 2013 MP (243750) installed.
#
#   ./play.sh                    # main menu
#   ./play.sh +map ctf_2fort     # any extra Source launch options
#
# Skate 3 data is converted in game on first run (or Options > Advanced >
# Skate 3 setup, or the skate_setup console command) and remembered in
# game/mod_tf/skate_setup/config.txt.
#
# Environment overrides:
#   SKATE3_ASSET_ROOT   prepared Skate 3 assets folder (.../installations/<id>/assets)
#   SKATE_DIFFICULTY    easy | normal | hardcore | motorized (default normal)
#   SNIPER_RUN          path to SteamLinuxRuntime_sniper/run
set -euo pipefail

here="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)"
simulation="$here/game/mod_tf/bin/linux64/libskate3.so"

if [[ -z "${SNIPER_RUN:-}" ]]; then
    for candidate in \
        "$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_sniper/run" \
        "$HOME/SteamGames/SteamLinuxRuntime_sniper/run"; do
        [[ -x "$candidate" ]] && SNIPER_RUN="$candidate" && break
    done
fi
if [[ ! -x "${SNIPER_RUN:-}" ]]; then
    echo "Could not find SteamLinuxRuntime_sniper/run; set SNIPER_RUN." >&2
    exit 1
fi

if [[ ! -f "$simulation" ]]; then
    echo "Build the Skate 3 simulation first: ./build-skate-lib.sh" >&2
    exit 1
fi

if [[ ! -f "$here/game/mod_tf/bin/linux64/libvgmstream.so" ]]; then
    echo "Note: libvgmstream.so is missing (./build-vgmstream.sh); first-time setup needs it for the sounds." >&2
fi

# The engine's bundled libfontconfig doesn't read /etc/fonts by itself; without
# this it runs with no font config (broken VGUI symbols like the close button
# and scroll arrows). See fontconfig/fonts.conf.
export FONTCONFIG_FILE="$here/fontconfig/fonts.conf"

cd "$here/game"
# NixOS has no FHS root for pressure-vessel's bubblewrap; steam-run provides one.
wrap=()
if [[ -e /etc/NIXOS ]] && command -v steam-run > /dev/null; then
    wrap=(steam-run)
fi
"${wrap[@]}" "$SNIPER_RUN" -- ./mod_tf_linux64 -game "$here/game/mod_tf" -novid -condebug "$@"
