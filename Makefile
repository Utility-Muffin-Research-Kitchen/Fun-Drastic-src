# Fun Drastic — build the hook, then package it (Linux / macOS / WSL).
# Windows: use build.bat / pack.bat instead (same Docker underneath).
#
#   make image           build the toolchain image (once; needs Docker)
#   make leaf            build the Leaf hook   (64-bit)       -> build/leaf/
#   make brick           build the Brick hook  (64-bit)       -> build/brick/
#   make h700            build the H700 hook   (32-bit armhf) -> build/h700/
#   make all             build every hook
#   make pack-<target>   assemble a package                  -> dist/<target>/
#
# The build logic lives in build.sh and the packaging in pack.sh — one source
# of truth, shared with the Windows wrappers. build.sh runs inside the image
# (it needs the cross-compilers) and picks the compiler from the target: a
# target folder with an `armhf` marker builds 32-bit (-DDRASTIC_ARM32), else
# 64-bit. Per-device settings: src/platforms/platform_<target>.h. See
# docs/BUILDING.md and CONTRIBUTING.md.

ROOT  := $(CURDIR)
IMAGE := fundrastic-build
DK     = docker run --rm --user root -v "$(ROOT)":/workspace $(IMAGE)

.PHONY: all image help leaf h700 brick pack-leaf pack-h700 pack-brick

help:
	@echo "build:  make image | make leaf | make h700 | make brick | make all"
	@echo "pack:   make pack-leaf | make pack-h700 | make pack-brick"
	@echo "Windows: build.bat / pack.bat.  See docs/BUILDING.md."

all: leaf h700 brick

image:
	docker build -t $(IMAGE) toolchain/

leaf h700 brick:
	$(DK) bash /workspace/build.sh $@

pack-leaf pack-h700 pack-brick:
	./pack.sh $(patsubst pack-%,%,$@)
