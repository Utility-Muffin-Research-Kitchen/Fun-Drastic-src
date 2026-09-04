// Fun Drastic platform template — copy to platform_<yourdevice>.h and edit.
//
// This header is pulled in at compile time with -include and describes ONE
// device: its panel, how its buttons arrive, and a few emulator-side toggles.
// platform_leaf.h is the fully worked example (Miniloong Pocket 1 / Wayland);
// start from whichever existing header is closest to your hardware.
//
// The hook is a single source (funhook.c). A 64-bit device compiles it as-is;
// a 32-bit (armhf) device compiles it with -DDRASTIC_ARM32, which selects the
// armhf DraStic's offsets (a different, non-PIE binary - offsets are absolute).
// See docs/BUILDING.md.

#ifndef PLATFORM_H
#define PLATFORM_H

#define PLATFORM_NAME       "template"   // short id, shown in logs

// --- Input source -----------------------------------------------------------
// FUN_KB_INPUT: set to 1 if the OS delivers the gamepad as KEYBOARD events
// (then the JOY_* numbers below are mirrored from keys); 0 for a real joystick.
// FUN_KB_MENU2: a second key that acts as Menu, or -2 to disable (let the
// joystick own Menu — avoids a double Menu press on hosts that send both).
#define FUN_KB_INPUT        0
#define FUN_KB_MENU2        (-2)

// FUN_FORCE_FULLSCREEN: 1 where the compositor only gives input focus to a
// fullscreen surface (Wayland). 0 for framebuffer / KMSDRM devices.
#define FUN_FORCE_FULLSCREEN 0

// --- Emulator-side toggles (see funhook.c) ----------------------------------
#define NDS_PACING_VALVE    1   // hook paces frames (keep on)
#define NDS_NO_VSYNC        1   // strip vsync; pacing handles timing
// #define NDS_RESUME_WAIT_3D 1 // enable only if boot-resume loads glitch

// --- Panel ------------------------------------------------------------------
// FB_*  = physical framebuffer.  VFB_* = the canvas the hook renders to.
// They match unless the panel is rotated (then swap and set ROT_ANGLE).
#define FB_W                640
#define FB_H                480
#define VFB_W               640
#define VFB_H               480
#define ROT_DST_INIT        {0, 0, 640, 480}
#define ROT_ANGLE           0.0

// Pixel-perfect fallback scales (the hook recomputes these at runtime from
// the panel size; these are only used before the first compute).
#define PP_SCALE_MAIN       2
#define PP_SCALE_FOCUS      1
#define PP_SCALE_PIP        1

// --- Buttons ----------------------------------------------------------------
// SDL joystick button number for each control, or -1 if the device lacks it.
// If the d-pad arrives as HAT events, set JOY_UP..RIGHT = -1 to select the
// hook's hat mode (it republishes the hat as virtual buttons 28-31).
#define JOY_UP              -1
#define JOY_DOWN            -1
#define JOY_LEFT            -1
#define JOY_RIGHT           -1
#define JOY_A               1
#define JOY_B               0
#define JOY_X               2
#define JOY_Y               3
#define JOY_L1              4
#define JOY_R1              5
#define JOY_L2              6
#define JOY_R2              7
#define JOY_SELECT          8
#define JOY_START           9
#define JOY_MENU            10
#define JOY_PLUS            -1
#define JOY_MINUS           -1
#define JOY_L3              -1
#define JOY_R3              -1

// --- Analog -----------------------------------------------------------------
// Axis indices for the left stick (used for the touch cursor) and, if the
// triggers are analog, their axis numbers. CURSOR_PRESS_BTN taps the cursor.
#define AXIS_LX             0
#define AXIS_LY             1
#define CURSOR_PRESS_BTN    -1
#define AXIS_L2_TRIG        2
#define AXIS_R2_TRIG        5

#endif // PLATFORM_H
