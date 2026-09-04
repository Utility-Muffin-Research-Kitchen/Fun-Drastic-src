#!/bin/sh
# Template launcher — the entrypoint your CFW calls, e.g. `launch.sh <rom>`.
# Most devices can use shared/launch.sh unchanged; override here only if you
# must. See targets/leaf/launch.sh for a complete, real example.
#
# The one rule that bites everyone: scope LD_PRELOAD to the emulator command,
# never export it globally, or every helper the script runs gets the hook
# injected. e.g.:
#
#   LD_PRELOAD="$HERE/lib/libfundrastic.so" "$HERE/drastic" "$1"
