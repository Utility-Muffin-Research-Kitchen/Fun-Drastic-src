# Contributing — adding a CFW / device

Fun Drastic is one hook (`src/funhook.c`) that adapts to each device through a
small compile-time header and a per-target folder. Adding support for a new
firmware is mostly description, not new engine code. `targets/leaf/` is the
fully built-out example — copy its shape; `targets/template/` is the skeleton.

## The moving parts

1. **A platform header** — copy `src/platforms/platform_template.h` to
   `src/platforms/platform_<target>.h` and set the screen size, button map, and
   feature flags. `platform_leaf.h` is the worked example. If the pad doesn't
   arrive as SDL input, see [docs/DEVICE-INPUT.md](docs/DEVICE-INPUT.md) — it
   walks the three input paths (SDL joystick, SDL keyboard, raw evdev) and how
   to bring up a device, with the h700 as the raw-evdev example.

2. **A `targets/<target>/` folder** — whatever is specific to that firmware
   (copy `targets/template/`):
   - `config/drastic.cfg` — the button map, if the device needs its own (must
     be a **full** cfg — a partial one breaks input and flips on the fps counter)
   - `launch.sh` — only if you must override the shared default
     (`shared/launch.sh`); see `targets/leaf/launch.sh`
   - `theme/custom.cfg` — an optional branded theme (+ its `.ttf` font)
   - anything else the package needs that isn't shared (a bundled SDL stack, a
     manifest — see `targets/leaf/`)

3. **A build + pack rule** — add `<target>` to the two rules in the
   `Makefile` (the `leaf h700:` build line and the `pack-...` line).
   For a 32-bit (armhf) device, drop an empty `armhf` marker file in
   `targets/<target>/` — that is what tells `build.sh` to use the armhf
   compiler with `-DDRASTIC_ARM32`; a 64-bit device needs no marker. The
   Windows `build.bat` / `pack.bat` accept any target name automatically.
   Anything the whole fleet shares — fonts, cursor, overlays, mic sound —
   already lives in `shared/` and is packaged automatically; don't duplicate it.

## Build

```
make image             # build the toolchain image (once)
make <target>          # build the hook
make pack-<target>     # assemble the package
```

On Windows use `build.bat <target>` / `pack.bat <target>` (same Docker underneath).

The full build walkthrough — cross-compilers, SDL, packaging, deploy paths —
is in [docs/BUILDING.md](docs/BUILDING.md).

## Ground rules

- **DraStic is bundled; Nintendo assets are not.** DraStic (Exophase's
  proprietary freeware) ships under `emulator/`. Never add Nintendo code, art,
  BIOS, or ROMs.
- **No Nintendo assets.** Theme names are color-palette names only — no
  character art, sprites, or logos anywhere in the repo or the packages.
- **Test on device.** `targets/leaf/PORTING.md` shows the kind of traps to
  expect (Wayland focus, library over-linking, input mapping) — a short notes
  file for your own device helps the next person.

## Building 32-bit (armhf)

`funhook.c` is one source for both architectures. A 64-bit device builds it as
is; a 32-bit (armhf) device builds it with `-DDRASTIC_ARM32`, which selects the
armhf DraStic's offsets (a different, non-PIE binary - offsets are absolute).
`h700` (Anbernic h700) is the worked 32-bit target. Input, touch,
save/load states, quit and audio use disassembly-verified offsets; a few
functions with no armhf offset yet (in-place reset, native cheat apply, the
is-saving probe) are guarded off so they are inert on 32-bit rather than
crashing. As with any hook change, smoke-test on the device - see
docs/BUILDING.md.
