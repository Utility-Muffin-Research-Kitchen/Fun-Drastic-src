# h700 — Allwinner H700 family (32-bit / armhf)

The 32-bit example target. The h700 runs the armhf DraStic, so this builds
`funhook.c` with `-DDRASTIC_ARM32` (the `armhf` marker in this folder tells the
build and packager to use the 32-bit lane). It otherwise ships the **shared
defaults** (`shared/` launcher, config, assets). To customize, drop either of
these here and the packer picks them up:

- `launch.sh`            — a device-specific launcher override
- `config/drastic.cfg`   — a device-specific button map (must be a FULL cfg)

See `targets/leaf/` for a fully built-out example and `../../CONTRIBUTING.md`.

## Verify on hardware (first h700 boot)

The 32-bit hook is built from the same source as the proven 64-bit one and uses
disassembly-verified armhf offsets, but a build only proves it compiles — run
this once on an h700 to confirm the offsets behave:

1. Launch a DS ROM — it should boot into the game with the themed hook UI.
2. Save a state (menu or shortcut), then load it back — the game should restore.
3. Quit from the menu, relaunch — a saved auto-resume state should load cleanly
   (not black-screen or start fresh: that would mean the save was torn down).
4. Move the touch cursor and tap — touch should register in-game.

If any of these misbehave, an offset is wrong for your DraStic build — compare
against `src/funhook.c`'s `#ifdef DRASTIC_ARM32` values.

## Input

The h700 image exposes no SDL joystick or keyboard, so the hook reads the pad
from the raw evdev nodes directly (`NDS_EVDEV_PAD` in the platform header; the
bridge is in `funhook.c`). The stick drives the touch cursor and a stick-click
(L3) taps. If you bring up a different h700 variant, re-derive the evdev nodes
and code map with `evtest` — see [../../docs/DEVICE-INPUT.md](../../docs/DEVICE-INPUT.md).

## Gracefully disabled on 32-bit

A few things have no armhf offset yet, so they are guarded off — inert, never
crashing: in-place **Reset** (needs a launcher relaunch instead), instant
**cheat hot-apply** (cheats are written and take effect on the next game load),
and the **fake microphone**. Everything else — input, touch, save/load states,
quit, audio, themes, overlays, translations, fast-forward — runs normally.