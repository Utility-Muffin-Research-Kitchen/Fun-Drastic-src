# Fun Drastic

A themed, standalone **DraStic** frontend for retro handhelds. Five
Nintendo-inspired color themes (MARIO, KOOPA, PEACH, WARIO, YOSHI — palettes
only, no character art), a self-contained SDL2 menu, save states, cheats,
screen-layout overlays, a touch-cursor, translations, and per-device tuning.

The whole thing is a single shared library (`libfundrastic.so`) that rides into
the stock DraStic binary via `LD_PRELOAD` — **no emulator patching**.

Not affiliated with DraStic's author (Exophase) or Nintendo. See
[CREDITS.md](CREDITS.md) for everything Fun Drastic builds on.

## Status

Fun Drastic is the hook and its visuals. It builds from one source for three
worked targets: **Leaf** (Miniloong Pocket 1, 64-bit), **brick** (TrimUI Brick /
Smart Pro, 64-bit, NextUI), and **h700** (Allwinner H700 family, 32-bit armhf).
A platform-header template and a target template are in place for adding more.

## Layout

```
src/
  funhook.c         the hook — one source for every device
  platforms/        one compile-time header per device
    platform_leaf.h       64-bit example (Leaf)
    platform_brick.h      64-bit example (TrimUI Brick / NextUI)
    platform_h700.h       32-bit armhf example (Allwinner H700)
    platform_template.h   skeleton for a new device
  lib/              third-party single-header libs (stb_image, stb_truetype)

shared/             payload every package ships: fonts, languages, cursor,
                    mic sound, boot-logo art, overlay templates, base launcher

targets/
  leaf/             the fully built-out example: SDL2/Wayland stack, button
                    map, launcher, manifest, its own theme, PORTING.md
  brick/            TrimUI Brick / Smart Pro (shared defaults)
  h700/             Allwinner H700 family (shared defaults)
  template/         skeleton for adding a new CFW/device

emulator/           DraStic binaries + BIOS + game/cheat DB (bundled)
toolchain/          Dockerfile for the cross-compile image
build/  dist/       hook output / packed packages (regenerated)

Makefile            build + package on Linux / macOS / WSL (via Docker)
build.bat pack.bat  the same, for Windows (Docker Desktop)
build.sh pack.sh    the build + packaging logic, shared by both
docs/BUILDING.md    build + toolchain + SDL walkthrough
docs/DEVICE-INPUT.md how a new device's pad is brought up
```

## Build

You need Docker — nothing else. Build the toolchain image once, then build
and package.

**Linux / macOS / WSL:**

```
make image        # build the fundrastic-build toolchain image (once)
make leaf         # build a hook (or: make brick, make h700, make all)
make pack-leaf    # assemble the package (pack-brick, pack-h700)
```

**Windows** (Docker Desktop, no `make` needed):

```
build.bat image
build.bat leaf
pack.bat leaf
```

Both drive the same Docker image and the same `build.sh` / `pack.sh`. Full
walkthrough — toolchain, cross-SDL2, packaging, deploy paths — in
**[docs/BUILDING.md](docs/BUILDING.md)**. Adding your own CFW/device? See
**[CONTRIBUTING.md](CONTRIBUTING.md)**, with `targets/leaf/` as the example.

## Batteries included

DraStic is bundled under `emulator/` — its binaries, free ARM7/ARM9 BIOS, game
database, and the community cheat database — so the workspace is turnkey: build,
package, and deploy without hunting for the emulator. DraStic is Exophase's
proprietary freeware; Fun Drastic ships the publicly distributed binaries
unmodified and is not affiliated with or endorsed by Exophase.

Theme names are color-palette names only — no character art, sprites, or logos
live in this repo or the packages.

## License

Fun Drastic's own code, scripts, and packaging are under the **PolyForm
Noncommercial License 1.0.0** (see [LICENSE](LICENSE)) — use, modify, and share
freely for noncommercial purposes; no commercial use. Third-party components
keep their own licenses — see [CREDITS.md](CREDITS.md).
