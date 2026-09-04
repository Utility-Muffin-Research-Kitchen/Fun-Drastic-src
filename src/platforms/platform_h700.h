// platform_h700.h — Allwinner H700 family (Anbernic RG35XX Plus/H/2024,
// RG34XXSP, RG40XX, ...). Runs the 32-bit armhf DraStic, so this target builds
// funhook.c with -DDRASTIC_ARM32 (see the Makefile).
// Panel: 640x480 landscape (no rotation needed)

#ifndef PLATFORM_H
#define PLATFORM_H

#define PLATFORM_NAME       "h700"

// ── Physical SDL window = VFB (landscape, no rotation) ───────────────────────
#define FB_W                640
#define FB_H                480

#define VFB_W               640
#define VFB_H               480

// ── Display rotation ─────────────────────────────────────────────────────────
#define ROT_DST_INIT        {0, 0, 640, 480}
#define ROT_ANGLE           0.0

// ── Pixel-perfect scales ─────────────────────────────────────────────────────
// NDS native 256x192. 2x = 512x384 fits cleanly in 640x480.
#define PP_SCALE_MAIN       2       // 2x main  → 512x384 (SINGLE)
#define PP_SCALE_FOCUS      2       // 2x focus → 512x384 + 0.5x touch (128x96) below
#define PP_SCALE_PIP        1       // 1x PIP   → 256x192

// ── D-pad: hat-based (SDL sees EV_ABS as SDL_JOYHATMOTION) ───────────────────
#define JOY_UP              -1
#define JOY_DOWN            -1
#define JOY_LEFT            -1
#define JOY_RIGHT           -1

// ── Face buttons ─────────────────────────────────────────────────────────────
#define JOY_A               0
#define JOY_B               1
#define JOY_X               3
#define JOY_Y               2

// ── Shoulders ────────────────────────────────────────────────────────────────
#define JOY_L1              4
#define JOY_R1              5
#define JOY_L2              9       // real button
#define JOY_R2              10      // real button
#define JOY_L3              11      // left stick click (RAW_L3)
#define JOY_R3              12      // right stick click (RAW_R3)

// ── System buttons ───────────────────────────────────────────────────────────
#define JOY_SELECT          6
#define JOY_START           7
#define JOY_MENU            8
#define JOY_PLUS            18
#define JOY_MINUS           17

// ── Analog axes ──────────────────────────────────────────────────────────────
#define AXIS_LX             0
#define AXIS_LY             1
#define CURSOR_PRESS_BTN    JOY_L3  // stick click = touch tap

// ── Analog triggers — L2/R2 are real buttons, no axes needed ─────────────────
#define AXIS_L2_TRIG        -1
#define AXIS_R2_TRIG        -1

// ── Boot-resume 3D wait: deliberately NOT defined ──────────────────────────
// NDS_RESUME_WAIT_3D is an aarch64-only anti-tamper workaround (guarded
// #ifndef DRASTIC_ARM32 in the hook). The armhf drastic is immune, and
// defining it made every auto-resume and reset run to the ~15s cap. Off.

// ── Raw-evdev game-pad bridge ──────────────────────────────────────────────
// This image exposes NO SDL joystick and does NOT deliver the pad as SDL
// keyboard, so the SDL input path is dead. NDS_EVDEV_PAD makes the hook read
// the pad from the kernel evdev nodes directly (see the bridge in funhook.c).
// The evdev node list and raw-code map live in funhook.c's evp_map() - re-derive
// them with `evtest` for a different device.
#define NDS_EVDEV_PAD 1

#endif // PLATFORM_H
