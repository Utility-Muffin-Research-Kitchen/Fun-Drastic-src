#!/bin/sh
# Fun Drastic — package assembler (POSIX; Linux / macOS / WSL).
#
#   ./pack.sh leaf        assemble the Leaf package -> dist/leaf/drastic
#   ./pack.sh <target>    assemble a generic NDS.pak -> dist/<target>/NDS.pak
#
# Build the hook first (see the Makefile): make <target>.
#
# DraStic (Exophase's proprietary freeware) is bundled under emulator/:
#
#   emulator/bin/drastic64            aarch64 emulator binary
#   emulator/bin/drastic              armhf emulator binary (32-bit targets)
#   emulator/bin/game_database.xml    DraStic save-type database
#   emulator/bin/system/drastic_bios_arm7.bin
#   emulator/bin/system/drastic_bios_arm9.bin
#   emulator/usrcheat.dat             (optional) DS cheat database
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
target=${1:-}
[ -n "$target" ] || { echo "usage: ./pack.sh <target>   (e.g. leaf)" >&2; exit 1; }

emu="$here/emulator/bin"
cheat="$here/emulator/usrcheat.dat"
hook="$here/build/$target/libfundrastic.so"

require() { [ -e "$1" ] || { echo "ERROR: missing $1${2:+ — $2}" >&2; exit 1; }; }
require "$hook" "build it first: make $target"
require "$emu/game_database.xml" "supply DraStic under emulator/ (see header)"

# Common payload shared by every package.
copy_shared() {
    dst=$1
    cp "$here/shared/drastic_logo_0.raw" "$here/shared/drastic_logo_1.raw" "$dst/"
    cp "$emu/game_database.xml" "$dst/"
    mkdir -p "$dst/fonts";      cp "$here/shared/fonts/Translate.otf"        "$dst/fonts/"
    mkdir -p "$dst/language";   cp "$here/shared/language/"*.txt             "$dst/language/"
    mkdir -p "$dst/res/cursor"; cp "$here/shared/res/cursor/1.png"           "$dst/res/cursor/"
    mkdir -p "$dst/microphone"; cp "$here/shared/microphone/microphone.wav"  "$dst/microphone/"
    if [ -f "$cheat" ]; then cp "$cheat" "$dst/config/usrcheat.dat"
    else echo "NOTE: emulator/usrcheat.dat absent — cheats DB omitted."; fi
}

# Copy the overlay Template set for one resolution (WxH), if present.
copy_overlays() {
    dst=$1; res=$2
    if [ -d "$here/shared/overlays/$res/Template" ]; then
        mkdir -p "$dst/Overlays/$res/Template"
        cp "$here/shared/overlays/$res/Template/"*.png "$dst/Overlays/$res/Template/"
    fi
}

# ── Leaf: a flat emulator package (bin/ lib/ config/ system/) ──────────────
if [ "$target" = "leaf" ]; then
    lib="$here/targets/leaf/lib_own"
    theme="$here/targets/leaf/theme"
    require "$lib/libSDL2-2.0.so.0" "the bundled Leaf SDL2 stack"
    require "$here/targets/leaf/config_mlp1/drastic.cfg" "the Leaf button map"
    require "$theme/custom.cfg" "the Leaf theme"
    require "$emu/drastic64"

    out="$here/dist/leaf/drastic"
    rm -rf "$out"; mkdir -p "$out/bin" "$out/lib" "$out/config" "$out/system" "$out/themes"

    cp "$here/targets/leaf/launch.sh" "$here/targets/leaf/manifest.json" "$out/"
    cp "$emu/drastic64" "$out/bin/drastic64"

    # bundled SDL2 stack + the hook. libasound.so.2 is an empty stub —
    # drastic64 lists it as NEEDED but imports no symbols. See PORTING.md.
    for so in libSDL2-2.0.so.0 libxkbcommon.so.0 libwayland-cursor.so.0 libasound.so.2; do
        cp "$lib/$so" "$out/lib/"
    done
    cp "$hook" "$out/lib/"

    cp "$here/targets/leaf/config_mlp1/drastic.cfg" "$out/config/drastic.cfg"
    cp "$emu/system/drastic_bios_arm7.bin" "$emu/system/drastic_bios_arm9.bin" "$out/system/"

    copy_shared "$out"
    copy_overlays "$out" 960x720
    # Leaf theme + its font
    cp "$theme/custom.cfg" "$out/themes/custom.cfg"
    cp "$theme/Nunito-Bold.ttf" "$out/fonts/"

    echo "Packed: dist/leaf/drastic"
    echo "Deploy: copy it to the Leaf card at"
    echo "  .system/leaf/platforms/mlp1/emulators/drastic/"
    exit 0
fi

# ── Generic NDS.pak (a new target using the shared defaults) ───────────────
# armhf targets ship the 32-bit drastic; everyone else drastic64.
binsrc="$emu/drastic64"; [ -f "$here/targets/$target/armhf" ] && binsrc="$emu/drastic"
require "$binsrc" "supply DraStic under emulator/ (see header)"

out="$here/dist/$target/NDS.pak"
rm -rf "$out"; mkdir -p "$out/lib" "$out/system" "$out/config"

cp "$here/shared/launch.sh" "$out/launch.sh"
[ -f "$here/targets/$target/launch.sh" ] && cp "$here/targets/$target/launch.sh" "$out/launch.sh"
cp "$binsrc" "$out/drastic"
cp "$emu/system/"* "$out/system/" 2>/dev/null || true
cp "$hook" "$out/lib/"
# per-target button map, else the bundled default so every package has one
if [ -f "$here/targets/$target/config/drastic.cfg" ]; then
    cp "$here/targets/$target/config/drastic.cfg" "$out/config/drastic.cfg"
elif [ -f "$here/emulator/bin/config/drastic.cfg" ]; then
    cp "$here/emulator/bin/config/drastic.cfg" "$out/config/drastic.cfg"
fi

copy_shared "$out"
# ship every overlay template; the hook loads the one matching the panel.
for d in "$here/shared/overlays"/*x*/; do copy_overlays "$out" "$(basename "$d")"; done
# optional per-target theme
if [ -f "$here/targets/$target/theme/custom.cfg" ]; then
    mkdir -p "$out/themes"; cp "$here/targets/$target/theme/custom.cfg" "$out/themes/"
    [ -f "$here/targets/$target/theme/"*.ttf ] && cp "$here/targets/$target/theme/"*.ttf "$out/fonts/" 2>/dev/null || true
fi

echo "Packed: dist/$target/NDS.pak"
