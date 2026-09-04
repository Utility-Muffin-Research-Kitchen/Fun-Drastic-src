# Building Fun Drastic

Fun Drastic is a single shared library, `libfundrastic.so`, `LD_PRELOAD`-ed
into the stock DraStic binary. Building it means cross-compiling one C file
inside a Docker image, then assembling a package. The only requirement is
Docker — Linux, macOS, and WSL use the `Makefile`; Windows uses `build.bat` /
`pack.bat` (Docker Desktop). Both drive the same `build.sh` and `pack.sh`.

## 1. The toolchain image

One image, `fundrastic-build`, carries the cross-compilers and the SDL2 headers
the hook compiles against. Build it once:

```
make image            # Linux / macOS / WSL
build.bat image       # Windows
```

(either just runs `docker build -t fundrastic-build toolchain/`.)

It provides `aarch64-linux-gnu-gcc` (64-bit ARM devices, e.g. Leaf),
`arm-linux-gnueabihf-gcc` (32-bit ARM devices), and SDL2 2.28.5 public headers
under `/usr/local/aarch64-linux-gnu/include`.

The hook **never links SDL2**. It resolves DraStic's own SDL2 symbols at run
time through `LD_PRELOAD`, so it only needs SDL2's headers to compile. That is
why a single cross-built SDL2 header tree serves both the 64- and 32-bit lane,
and why the SDL2 backends are off in the image — nothing from the container's
SDL2 ends up in a package.

## 2. Building the hook

```
make leaf             # 64-bit (Leaf)          -> build/leaf/libfundrastic.so
make brick            # 64-bit (TrimUI Brick)  -> build/brick/libfundrastic.so
make h700             # 32-bit armhf (H700)    -> build/h700/libfundrastic.so
build.bat leaf        # Windows (any target: build.bat brick / h700)
```

There is one source, `src/funhook.c`. A 64-bit device compiles it as is; a
32-bit (armhf) device adds `-DDRASTIC_ARM32`, which selects the 32-bit DraStic
binary's offsets and patching. Per-device settings — screen size, button map,
feature flags — live in `src/platforms/platform_<target>.h`, pulled in with
`-include`. Adding a device is a new header, a `targets/<target>/` folder, a line in the
Makefile's build and pack rules, and — for a 32-bit device — an `armhf`
marker file; see [CONTRIBUTING.md](../CONTRIBUTING.md).

## 3. DraStic (included)

DraStic is bundled under `emulator/` — nothing to supply. The layout:

```
emulator/bin/drastic64                    aarch64 emulator binary
emulator/bin/drastic                      armhf emulator binary (32-bit targets)
emulator/bin/game_database.xml            DraStic save-type database
emulator/bin/system/drastic_bios_arm7.bin
emulator/bin/system/drastic_bios_arm9.bin
emulator/usrcheat.dat                     (optional) DS cheat database
```

The BIOS files are DraStic's own free ARM7/ARM9 replacements — no Nintendo BIOS
is required. `usrcheat.dat` is the community DS cheat database. DraStic is
Exophase's proprietary freeware, shipped unmodified.

## 4. Packaging

```
make pack-leaf          # Linux / macOS / WSL  -> dist/leaf/drastic
pack.bat leaf           # Windows
```

`make pack-leaf` runs `./pack.sh leaf` (POSIX; usable directly on Linux, macOS,
or WSL). Build the hook first. Packaging pulls the shared payload from
`shared/`, the panel-resolution overlay templates, the Leaf launcher, button
map, theme and SDL stack from `targets/leaf/`, and your DraStic files from
`emulator/`.

Deploy the Leaf package by copying `dist/leaf/drastic` to the card at
`.system/leaf/platforms/mlp1/emulators/drastic/`.

`pack.sh` also has a generic path (`./pack.sh <target>`) that assembles a
standard `NDS.pak` from `shared/` plus a new target's overrides — the starting
point for a second device.

## 5. The SDL / Wayland notes

Leaf is the target with real display-stack work, and every trap is written up
in [../targets/leaf/PORTING.md](../targets/leaf/PORTING.md): bundling a mainline
SDL2 (2.30.11) with the Wayland backend, keeping its versioned symbols at or
below the device's glibc floor, the empty `libasound.so.2` stub that satisfies
DraStic's overlinking, mandatory fullscreen for input focus, and the
virtual-gamepad HAT input mapping. Read it before bringing up a new Wayland
device — most of it generalizes. A framebuffer / KMSDRM device reuses the
device's own SDL2 at run time and ships only the hook and configs, no SDL stack.

## 6. Building for 32-bit (armhf)

`funhook.c` builds for both architectures from one source. `make h700`
builds the 32-bit hook - the armhf compiler plus `-DDRASTIC_ARM32`, which
selects the armhf DraStic's offsets. That binary is non-PIE, so its offsets are
absolute addresses (`find_exe_base()` returns 0). Input, touch, save/load
states, quit and audio use disassembly-verified offsets; a few functions with
no armhf offset (in-place reset, native cheat apply, the is-saving probe) are
guarded off so they are inert on 32-bit rather than crashing. After building for
a new armhf device, boot it once and check save/load - if an offset is wrong
DraStic black-screens, so confirm on hardware.
