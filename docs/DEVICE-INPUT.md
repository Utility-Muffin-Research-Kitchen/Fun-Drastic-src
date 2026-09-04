# Bringing up input on a new device

DraStic and this hook read the game pad through SDL. Different device images
expose the pad in one of three ways, and getting a new device working is mostly
a matter of finding out which — then setting the matching knob in the platform
header (`src/platforms/platform_<target>.h`).

## Step 1 — find out how the pad reaches SDL

Build the hook, run a game with `FUN_EVLOG=1`, and watch the log while you press
buttons (or run `evtest` on the device and pick each input node). One of three
things is true:

1. **SDL joystick events** — the common case. Each button is an
   `SDL_JOYBUTTON` with a number, the d-pad is a HAT, the stick is an axis. Set
   the `JOY_*`, `AXIS_LX/LY`, `CURSOR_PRESS_BTN` numbers in the header from what
   the log shows. `platform_leaf.h` is a worked example.

2. **SDL keyboard events** — the pad arrives as key presses. Set
   `FUN_KB_INPUT` and map the keys in `kb_to_btn()`.

3. **Nothing** — the image exposes no SDL joystick and no keyboard (the input
   probe logs only a stray power scancode). The pad is only on the raw kernel
   evdev nodes. This is the `h700` (h700) case — read on.

## Step 2 — the raw-evdev bridge (case 3)

When SDL sees nothing, set `NDS_EVDEV_PAD` in the platform header. The hook then
reads `/dev/input/event*` directly (see the bridge in `funhook.c`, guarded by
`#ifdef NDS_EVDEV_PAD`) and:

- publishes `app.joy_held` so the hook's **menu** navigates, and
- writes DraStic's **button_status** straight into its input struct — the same
  "the hook owns the field" idea `nds_touch_enforce()` uses for touch.

To adapt it to a new device:

- **Find the nodes.** `evtest` lists every `/dev/input/eventN` and what it
  carries. Put the pad + system nodes in `g_evp_node[]`.
- **Map the codes.** `evtest` prints the raw `EV_KEY` code for each button and
  the `EV_ABS` codes for the d-pad and stick. Fill in `evp_map()`. Codes differ
  between the pad node and the system nodes, so both are handled.
- **The `JOY_*` numbers** in the header are the logical button IDs the raw codes
  map to (and what the shipped `drastic.cfg` binds). `CURSOR_PRESS_BTN = JOY_L3`
  makes a stick-click a touch tap; `AXIS_LX/LY` name the stick axes.

## The touch/pen detail worth knowing

DraStic's pen-down flag is **bit 17 of button_status**. Its input path derives
`touch_status = (button_status >> 17) & 1` every frame, so writing the touch
status byte in the struct directly is a no-op — that bit overwrites it. The
bridge sets bit 17 when the hook's cursor is pressed; the x/y ride in through
`nds_touch_enforce()`. That is why the cursor "taps" land.

## Two gotchas the bridge already handles

- **Menu-exit bleed.** The button still held when you close the menu (the A that
  picked Resume) must not fall straight into the game. The bridge latches the
  held bits at menu close and hides them until you physically release.
- **Event starvation.** DraStic drains events with `while (SDL_PollEvent(&e))`
  and stops at the first empty return. `SDL_PollEvent`'s repoll loop feeds the
  next event on every consumed one so the queue never backs up (this is what
  keeps the stick cursor from lagging).

## Where each piece lives

- Header knobs: `src/platforms/platform_<target>.h`
- The evdev bridge + integration: `funhook.c`, all under `#ifdef NDS_EVDEV_PAD`
- Button bindings shipped to DraStic: `targets/<target>/config/drastic.cfg`
