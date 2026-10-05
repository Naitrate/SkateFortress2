#!/usr/bin/env bash
# Try the in-game first-time Skate 3 setup again, as a new player would.
#
#   tools/reset-skate-setup.sh            move the converted Skate 3 files aside;
#                                         the next launch opens the setup popup
#   tools/reset-skate-setup.sh --restore  put the most recent set back
#   tools/reset-skate-setup.sh --list     show the saved sets
#
# Nothing is deleted: files move to game/skate-setup-backups/<time>/
# (outside the mod folder, so the game doesn't see them). Your sound picker
# choices (sound/skate/events.txt, events.json) stay in place.
set -euo pipefail

here="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")/.." && pwd)"
mod="${SKATE_MOD_DIR:-$here/game/mod_tf}"
backups="${SKATE_SETUP_BACKUPS:-$here/game/skate-setup-backups}"
# Everything the setup writes into the mod folder.
generated=(skate3_data sound/skate models/skate materials/models/skate)
keep=(sound/skate/events.txt sound/skate/events.json)

# True if the mod holds a conversion (more than just the picker files).
has_conversion() {
    for path in skate3_data models/skate materials/models/skate; do
        [[ -e "$mod/$path" ]] && return 0
    done
    [[ -d "$mod/sound/skate" ]] && find "$mod/sound/skate" -type f ! -name "events.txt" ! -name "events.json" | grep -q .
}

move_aside() {
    if ! has_conversion; then
        echo "No converted Skate 3 files in $mod"
        return
    fi
    local target="$backups/$(date +%Y%m%d-%H%M%S)"
    local moved=0
    for path in "${generated[@]}"; do
        [[ -e "$mod/$path" ]] || continue
        mkdir -p "$(dirname "$target/$path")"
        mv "$mod/$path" "$target/$path"
        moved=1
    done
    if (( moved )); then
        for path in "${keep[@]}"; do
            if [[ -e "$target/$path" ]]; then
                mkdir -p "$(dirname "$mod/$path")"
                cp -a "$target/$path" "$mod/$path"
            fi
        done
        echo "Moved the converted Skate 3 files to $target"
    else
        echo "No converted Skate 3 files in $mod"
    fi
}

case "${1:-}" in
    "")
        if pgrep -f "mod_tf_linux64" > /dev/null; then
            echo "Close the game first." >&2
            exit 1
        fi
        move_aside
        echo "Next ./play.sh is a first run: the Skate 3 setup popup opens at the main menu."
        ;;
    --restore)
        latest="$(ls -1d "$backups"/*/ 2>/dev/null | sort | tail -1)"
        if [[ -z "$latest" ]]; then
            echo "No saved sets in $backups" >&2
            exit 1
        fi
        move_aside   # keep whatever is there now too
        for path in "${generated[@]}"; do
            [[ -e "$latest/$path" ]] || continue
            rm -rf "${mod:?}/$path.restoring"
            mkdir -p "$(dirname "$mod/$path")"
            if [[ -e "$mod/$path" ]]; then
                # only the kept picker files can be here
                cp -a "$mod/$path/." "$latest/$path/" 2>/dev/null || true
                mv "$mod/$path" "$mod/$path.restoring"
            fi
            mv "$latest/$path" "$mod/$path"
            rm -rf "${mod:?}/$path.restoring"
        done
        rmdir -p "$latest"/* "$latest" 2>/dev/null || true
        echo "Restored $latest"
        ;;
    --list)
        ls -1 "$backups" 2>/dev/null || echo "No saved sets"
        ;;
    *)
        sed -n 2,13p "$0"
        exit 1
        ;;
esac
