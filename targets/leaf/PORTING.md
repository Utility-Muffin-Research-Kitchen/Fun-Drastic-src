# Fun Drastic on Leaf (Miniloong Pocket 1 / mlp1) — porting notes

How this port works and every trap we hit bringing it up. Source-tree notes
only; nothing here ships in the emulator package.

## Package shape

Leaf emulator packages live at
`.system/leaf/platforms/mlp1/emulators/<id>/` with `manifest.json` and a
`launch.sh <rom-path>` entrypoint. `launch.sh` sources
`../../launcher/env.sh`, keeps all runtime state under `.umrk/mlp1/<id>/`
(`HOME` and `FUN_DRASTIC_DIR` point there), and logs land next to the
package (`debug.txt` for the launcher, `game.log` for the emulator).

The package is flat: `bin/drastic64`, `lib/` (bundled SDL stack + hook),
`config/`, `system/`, `res/`, `Overlays/`, `game_database.xml`, splash raws.
The launcher seeds state on first run and force-refreshes package-owned
files (controls config, splash, overlay packs) every boot, so package
updates actually take effect.

## Video: own SDL2 on Wayland

The display is a Wayland compositor. This port bundles a mainline SDL2
(2.30.11) built for aarch64 with the `wayland` video backend and all
optional deps loaded via dlopen (`wayland-client/egl/cursor`, `xkbcommon`,
`asound`, PulseAudio shared). `readelf -d` shows NEEDED only libc/libm/ld,
and every versioned symbol stays at or below GLIBC_2.38 (the device
floor). A real libxkbcommon 1.6 is bundled; `libwayland-cursor.so.0` is a
five-symbol NULL stub (nothing draws a system cursor).

**Fullscreen is mandatory** (`FUN_FORCE_FULLSCREEN`): the compositor grants
input focus to fullscreen toplevels only. A windowed surface renders but
never receives input.

## The libasound trap

`drastic64` lists `libasound.so.2` as NEEDED but imports **zero** `snd_*`
symbols (overlinking) — audio actually runs SDL2 → PulseAudio. The package
ships an empty stub `libasound.so.2` to satisfy the loader. Lesson: audit
library dependencies with `readelf -d` / `nm -D`, never by filename — an
earlier third-party shim secretly pulled in two more libraries and broke
the loader (rc=127) when they were removed.

## Input

The compositor hides the real gamepad and republishes a virtual "Loong
Gamepad" on `/dev/input/event5`. SDL needs both:

```
SDL_JOYSTICK_DEVICE=/dev/input/event5
SDL_JOYSTICK_DISABLE_UDEV=1
```

Buttons arrive as joystick buttons 0-11 (A=1 B=0 X=2 Y=3 L1=4 R1=5 L2=6
R2=7 SELECT=8 START=9 MENU=10 L3=11). The **d-pad arrives as HAT events**;
the hook's hat mode (platform header `JOY_UP..RIGHT = -1`) republishes hat
motion as virtual buttons 28-31, which the shipped `drastic.cfg` maps as
`controls_b` 1052-1055. The Menu button arrives **twice** — joystick
button 10 and a synthetic HOME key — so the keyboard alias is disabled
(`FUN_KB_MENU2 = -2`) and the joystick owns it; without that, a held combo
reopened the menu on release. Left stick is consumed by the hook for the
touch cursor.

## Launcher rules learned the hard way

- **Never `export LD_PRELOAD` globally.** Scope it to the emulator command
  (`LD_PRELOAD=... "$BIN" "$ROM"`), or every helper the script runs (date,
  grep, cp) gets the hook injected and dies quietly.
- The state config is refreshed from the package each boot; stale configs
  from older builds otherwise pin dead button numbers (the hook also
  versions its own config files for the same reason).
- The emulator's per-frame tick output is stripped from `game.log` after
  the run, keeping only meaningful lines.

## Emulator-side notes (see the platform header)

- `NDS_NO_VSYNC`: vsync is stripped (compile flag + runtime); pacing is
  handled by the hook's frame-sync valve. Fast-forward is governed against
  the emulator's own clock (`FUN_FF_SPEED`x real time, default 2).
- No `NDS_RESUME_WAIT_3D` on this host: boot-resume loads are clean here,
  and the deferred load only added a visible hold.
- Canvas is the native 960x720 panel; pixel-perfect scales 3/2/1.

## Debug knobs (env, see launch.sh)

`FUN_HOOK=0` (run without the hook) · `FUN_EVLOG=1` (input events + pacing
diagnostics + shortcut probe) · `FUN_CFG_REFRESH=0` (keep state config) ·
`FUN_FF_SPEED=2..8` · `FUN_DRIVER`, `FUN_JOYDEV`, `FUN_WMCLASS`.

## Open questions for the Leaf maintainers

1. Blessed install location for third-party emulator packages, outside the
   update-wiped `.system` area?
2. What registers a second standalone emulator for a system in the picker?
3. Anything the compositor expects from a long-running fullscreen app that
   this port should be doing (lifecycle signals, suspend hooks)?
