#!/bin/sh
# Fun Drastic launcher (mlp1). Prepares the state dir, refreshes
# package-owned config, and runs drastic64 with the Fun Drastic hook.
set -u

# Knobs (env-overridable): FUN_HOOK=0 runs without the hook;
# FUN_EVLOG=1 logs input events + pacing diagnostics;
# FUN_CFG_REFRESH=0 keeps the state config; FUN_FF_SPEED=2..8.
FUN_HOOK="${FUN_HOOK:-1}"
FUN_DRIVER="${FUN_DRIVER:-wayland}"
export FUN_EVLOG="${FUN_EVLOG:-0}"
export SDL_VIDEO_WAYLAND_WMCLASS="${FUN_WMCLASS:-drastic}"
export SDL_JOYSTICK_DEVICE="${FUN_JOYDEV:-/dev/input/event5}"
export SDL_JOYSTICK_DISABLE_UDEV=1

SELF_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
# One support log per run: everything collects in RAM (/tmp) while the
# game runs - no SD writes during gameplay - and lands on the card as
# fundrastic.log at exit (launcher section is flushed pre-launch so a
# hard power-off still leaves evidence of the attempt).
FUNLOG="$SELF_DIR/fundrastic.log"
DBG="/tmp/fundrastic_run.log"
: > "$DBG" 2>/dev/null || DBG="$FUNLOG"
log() { echo "[$(date '+%H:%M:%S' 2>/dev/null)] $*" >> "$DBG" 2>/dev/null; }
rm -f "$SELF_DIR/debug.txt" "$SELF_DIR/game.log" 2>/dev/null

log "=== Fun Drastic launch ==="
log "SELF_DIR=$SELF_DIR"
log "args: $# : ${1:-<none>}"

if [ -f "$SELF_DIR/../../launcher/env.sh" ]; then
    . "$SELF_DIR/../../launcher/env.sh"
elif [ -n "${UMRK_ENV_FILE:-}" ] && [ -f "$UMRK_ENV_FILE" ]; then
    . "$UMRK_ENV_FILE"
else
    log "env: no env.sh found - using fallbacks"
fi

if [ "$#" -lt 1 ]; then
    log "FATAL: no rom argument"
    echo "usage: launch.sh <rom-path>" >&2
    exit 64
fi

ROM_PATH="$1"
[ -f "$ROM_PATH" ] || log "WARNING: rom does not exist at: $ROM_PATH"

STATE_ROOT="${FUNDRASTIC_STATE_ROOT:-${UMRK_INTERNAL_DATA_PATH:-${SDCARD_PATH:-/mnt/sdcard}/.umrk/mlp1}/fundrastic}"
log "STATE_ROOT=$STATE_ROOT"

mkdir -p \
    "$STATE_ROOT" \
    "$STATE_ROOT/backup" \
    "$STATE_ROOT/savestates" \
    "$STATE_ROOT/profiles" \
    "$STATE_ROOT/unzip_cache" \
    "$STATE_ROOT/input_record" \
    "$STATE_ROOT/cheats" \
    "$STATE_ROOT/slot2" \
    "$STATE_ROOT/microphone" \
    "$STATE_ROOT/scripts" 2>>"$DBG" || log "WARNING: mkdir chain reported failure"

copy_if_missing() {
    src="$1"; dst="$2"
    if [ -e "$src" ] && [ ! -e "$dst" ]; then
        cp -R "$src" "$dst" 2>>"$DBG" && log "seeded: $dst" || log "WARNING: seed failed: $src -> $dst"
    fi
}
copy_if_missing "$SELF_DIR/config" "$STATE_ROOT/config"
copy_if_missing "$SELF_DIR/system" "$STATE_ROOT/system"
copy_if_missing "$SELF_DIR/res" "$STATE_ROOT/res"
copy_if_missing "$SELF_DIR/res/cursor" "$STATE_ROOT/res/cursor"
copy_if_missing "$SELF_DIR/Overlays" "$STATE_ROOT/Overlays"
copy_if_missing "$SELF_DIR/game_database.xml" "$STATE_ROOT/game_database.xml"
copy_if_missing "$SELF_DIR/config/usrcheat.dat" "$STATE_ROOT/usrcheat.dat"
copy_if_missing "$SELF_DIR/drastic_logo_0.raw" "$STATE_ROOT/drastic_logo_0.raw"
copy_if_missing "$SELF_DIR/drastic_logo_1.raw" "$STATE_ROOT/drastic_logo_1.raw"

# Package overlay packs sync when the package changed (stamp-gated: the
# copy costs boot time and SD writes, so unchanged packages skip it).
# Stale packs from older packages are dropped; user-added packs are kept.
if [ -d "$SELF_DIR/Overlays/960x720" ]; then
    OSTAMP="$STATE_ROOT/.overlays_synced"
    if [ ! -e "$OSTAMP" ] || [ "$SELF_DIR/Overlays/960x720" -nt "$OSTAMP" ]; then
        mkdir -p "$STATE_ROOT/Overlays/960x720"
        for ov in "$SELF_DIR/Overlays/960x720"/*/; do
            [ -d "$ov" ] || continue
            ovn="$(basename "$ov")"
            rm -rf "$STATE_ROOT/Overlays/960x720/$ovn" 2>>"$DBG"
            cp -R "$ov" "$STATE_ROOT/Overlays/960x720/$ovn" 2>>"$DBG" \
                && log "overlay synced: $ovn" \
                || log "WARNING: overlay sync FAILED: $ovn"
        done
        touch "$OSTAMP" 2>>"$DBG"
    fi
fi

# Package-owned files refresh every boot: the controls config and splash.
if [ "${FUN_CFG_REFRESH:-1}" = "1" ] && [ -f "$SELF_DIR/config/drastic.cfg" ]; then
    mkdir -p "$STATE_ROOT/config"
    cp -f "$SELF_DIR/config/drastic.cfg" "$STATE_ROOT/config/drastic.cfg" 2>>"$DBG" \
        && log "cfg refreshed from package" \
        || log "WARNING: cfg refresh FAILED"
fi
log "cfg live: $(grep -a "controls_b\[CONTROL_INDEX_\(SELECT\|START\|MENU\)\]" "$STATE_ROOT/config/drastic.cfg" 2>/dev/null | tr '\n' ' ')"
for lg in drastic_logo_0.raw drastic_logo_1.raw; do
    [ -f "$SELF_DIR/$lg" ] && cp -f "$SELF_DIR/$lg" "$STATE_ROOT/$lg" 2>>"$DBG"
done

for f in "$SELF_DIR/bin/drastic64" "$SELF_DIR/lib/libfundrastic.so" \
         "$SELF_DIR/lib/libSDL2-2.0.so.0" "$SELF_DIR/lib/libxkbcommon.so.0" \
         "$SELF_DIR/lib/libwayland-cursor.so.0" "$SELF_DIR/lib/libasound.so.2"; do
    if [ -e "$f" ]; then log "preflight OK: $f"; else log "FATAL: MISSING $f"; fi
done
chmod +x "$SELF_DIR/bin/drastic64" 2>/dev/null || true

export HOME="$STATE_ROOT"
export FUN_DRASTIC_DIR="$STATE_ROOT"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export SDL_VIDEODRIVER="$FUN_DRIVER"
# LD_PRELOAD/LD_LIBRARY_PATH are scoped to the emulator command only -
# a global export injects the hook into every helper this script runs.
PRELOAD=""
[ "$FUN_HOOK" = "1" ] && PRELOAD="$SELF_DIR/lib/libfundrastic.so"
DRASTIC_BIN="$SELF_DIR/bin/drastic64"

log "launching: $DRASTIC_BIN '$ROM_PATH' (hook=$FUN_HOOK driver=$FUN_DRIVER)"
cp -f "$DBG" "$FUNLOG" 2>/dev/null; sync 2>/dev/null
cd "$STATE_ROOT"

ts=$(date +%s)
LD_LIBRARY_PATH="$SELF_DIR/lib" LD_PRELOAD="$PRELOAD" \
    "$DRASTIC_BIN" "$ROM_PATH" >>"$DBG" 2>&1
rc=$?
te=$(date +%s)
log "--- drastic64 ended: rc=$rc after $((te - ts))s ---"
# single card write at exit: the run log minus per-frame pacing spam
if [ "$DBG" != "$FUNLOG" ]; then
    grep -v -e "^vf ticks" -e "^ticks_delta:" "$DBG" > "$FUNLOG" 2>/dev/null
    rm -f "$DBG" 2>/dev/null
fi
exit "$rc"
