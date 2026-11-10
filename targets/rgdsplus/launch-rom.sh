#!/bin/bash
# The menu reads the ROM header directly. Stage compressed games as raw .nds files.
. /etc/profile.d/001-functions
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
rom=$(realpath "${1:?Pass a Nintendo DS ROM}")
game_name=${rom##*/}
shader=$(get_setting fun_drastic_shader nds "$game_name")
if [ -z "$shader" ] || [ "$shader" = auto ]; then
    shader=$(get_setting shader nds "$game_name")
fi
export FUN_DRASTIC_SHADER="$shader"
case "$rom" in
    /storage/roms/nds/*.[nN][dD][sS]) exec "$root/launch.sh" "$rom" ;;
    /storage/roms/nds/*.[zZ][iI][pP]|/storage/roms/nds/*.[7][zZ]) ;;
    *) printf 'Fun Drastic: unsupported ROM path: %s\n' "$rom" >&2; exit 64 ;;
esac

mkdir -p "$root/roms"
work=$(mktemp -d "$root/roms/.archive.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
7za l -slt -- "$rom" > "$work/members"
member=$(awk '
    /^----------$/ { members = 1; next }
    members && /^Path = / {
        candidate = substr($0, 8)
        # macOS adds AppleDouble resource metadata beside archived ROMs.
        if (tolower(candidate) !~ /\.nds$/ ||
            candidate ~ /(^|[\/\\])__MACOSX([\/\\]|$)/ ||
            candidate ~ /(^|[\/\\])\._[^\/\\]*$/)
            next
        path = candidate
        count++
    }
    END { if (count == 1) print path; else exit 1 }
' "$work/members") || {
    printf 'Fun Drastic: expected exactly one .nds file in %s\n' "$rom" >&2
    exit 65
}
name=${rom##*/}
game="$work/${name%.*}.nds"
7za x -so -spd -- "$rom" "$member" > "$game"
test -s "$game"
"$root/launch.sh" "$game"
