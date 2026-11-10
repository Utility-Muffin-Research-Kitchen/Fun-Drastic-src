#!/bin/sh
# Run Fun Drastic with private emulator data.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
rom=$(realpath "${1:?Pass a Nintendo DS ROM}")
case "$rom" in
    "$root"/roms/*.[nN][dD][sS]|/storage/roms/nds/*.[nN][dD][sS]) test -f "$rom" ;;
    *) printf 'Fun Drastic: unsupported ROM path: %s\n' "$rom" >&2; exit 64 ;;
esac

state="$root/state"
mkdir -p "$state"/backup "$state"/savestates "$state"/Saves "$state"/cheats \
    "$state"/profiles "$state"/unzip_cache "$state"/input_record "$state"/slot2 "$state"/scripts
# Seed the private data directory on first launch.
for entry in config system fonts language res microphone Overlays game_database.xml drastic_logo_0.raw drastic_logo_1.raw; do
    if [ -e "$root/$entry" ] && [ ! -e "$state/$entry" ]; then
        cp -R "$root/$entry" "$state/$entry"
    fi
done

cd "$state"
export FUN_DRASTIC_DIR="$state"
export SDCARD_PATH="$state"
export SDL_VIDEO_WAYLAND_WMCLASS=drastic
export SDL_TOUCH_MOUSE_EVENTS=0
binary="$root/drastic"
[ -x "$binary" ] || binary="$root/bin/drastic"
exec env LD_PRELOAD="$root/lib/libfundrastic.so" "$binary" "$rom" > "$root/session.log" 2>&1
