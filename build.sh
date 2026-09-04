#!/bin/sh
# Compile the Fun Drastic hook for one target. Runs INSIDE the toolchain
# image (it needs the cross-compilers); invoke it through the Makefile
# (`make <target>`) or build.bat (`build.bat <target>`), not on the host.
#
# The compiler is chosen by the target: a target folder with an `armhf`
# marker builds the 32-bit (armhf, -DDRASTIC_ARM32) hook; otherwise 64-bit.
set -eu
target=${1:?usage: build.sh <target>}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
hdr="$here/src/platforms/platform_$target.h"
[ -f "$hdr" ] || { echo "ERROR: missing src/platforms/platform_$target.h" >&2; exit 1; }

WARN="-Wall -Wno-unused-function -Wno-nonnull-compare -Wno-format-truncation"
INC="-I/usr/local/aarch64-linux-gnu/include -I/usr/local/aarch64-linux-gnu/include/SDL2"

if [ -f "$here/targets/$target/armhf" ]; then
    CC="arm-linux-gnueabihf-gcc -O2 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard -mcpu=cortex-a53 -fPIC -shared -DDRASTIC_ARM32"
else
    CC="aarch64-linux-gnu-gcc -O2 -march=armv8-a -mcpu=cortex-a53 -fPIC -shared"
fi

mkdir -p "$here/build/$target"
# shellcheck disable=SC2086
$CC -include "$hdr" $WARN $INC \
    "$here/src/funhook.c" -o "$here/build/$target/libfundrastic.so" \
    -ldl -lpthread -lm
echo "Built: build/$target/libfundrastic.so"
