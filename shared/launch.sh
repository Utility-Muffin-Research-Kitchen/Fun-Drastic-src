#!/bin/sh
cd "$(dirname "$0")"
# The package's own libraries load first; the device's system libraries are the
# fallback (DraStic's own SDL2, loaded via LD_PRELOAD, stays first regardless).
SDCARD="${SDCARD_PATH:-/mnt/SDCARD}"
export LD_LIBRARY_PATH="/usr/lib:$PWD/lib:$SDCARD/.system/$PLATFORM/lib"
export HOME="$PWD"

# The device launcher does not capture package stdout, so send the hook's log
# to the package folder. Keep the previous run for a post-mortem after a crash.
LOGF="$PWD/fundrastic.log"
[ -f "$LOGF" ] && mv -f "$LOGF" "$LOGF.prev"
exec >"$LOGF" 2>&1
echo "=== Fun Drastic launch: $(date) rom=$1 ==="

# NDS firmware/BIOS are user-supplied on the card (Bios/NDS), never shipped
# in the pak. Sync into drastic's system dir when new or changed — tiny
# files, copied only on change.
PAKROOT="$(cd "$(dirname "$0")" && pwd)"
NDS_BIOS="${SDCARD_PATH:-/mnt/SDCARD}/Bios/NDS"
mkdir -p "$NDS_BIOS" "$PAKROOT/system"
for f in drastic_bios_arm7.bin drastic_bios_arm9.bin nds_bios_arm7.bin nds_bios_arm9.bin nds_firmware.bin; do
  if [ -f "$NDS_BIOS/$f" ] && { [ ! -f "$PAKROOT/system/$f" ] || [ "$NDS_BIOS/$f" -nt "$PAKROOT/system/$f" ]; }; then
    cp -f "$NDS_BIOS/$f" "$PAKROOT/system/$f"
  fi
done

LD_PRELOAD="$PWD/lib/libfundrastic.so" "$PWD/drastic" "$1"
