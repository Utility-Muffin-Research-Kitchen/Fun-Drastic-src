// Fun Drastic platform: mlp1 (RK3566, 960x720 panel, Wayland compositor).

#ifndef PLATFORM_H
#define PLATFORM_H

#define PLATFORM_NAME       "mlp1"

// Buttons arrive as keyboard events on this host; mirror them onto JOY_*.
// The Menu button is owned by the joystick (btn 10) - keyboard alias off.
#define FUN_KB_INPUT        1
#define FUN_KB_MENU2        (-2)

// The compositor grants input focus to fullscreen toplevels only.
#define FUN_FORCE_FULLSCREEN 1

// Emulator-side fixes (see funhook.c). No NDS_RESUME_WAIT_3D here: boot
// resume loads immediately on this host (field-clean), and the wait only
// added a visible hold.
#define NDS_PACING_VALVE    1
#define NDS_NO_VSYNC        1

// Panel-native canvas.
#define FB_W                960
#define FB_H                720
#define VFB_W               960
#define VFB_H               720
#define ROT_DST_INIT        {0, 0, 960, 720}
#define ROT_ANGLE           0.0

// Pixel-perfect scales (recomputed at runtime; these are fallbacks).
#define PP_SCALE_MAIN       3
#define PP_SCALE_FOCUS      2
#define PP_SCALE_PIP        1

// Buttons. The dpad arrives as HAT events: JOY_UP..RIGHT = -1 selects the
// hook's hat mode (virtual buttons 28-31).
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
#define JOY_L3              11
#define JOY_R3              -1

// Analog.
#define AXIS_LX             0
#define AXIS_LY             1
#define CURSOR_PRESS_BTN    11
#define AXIS_L2_TRIG        2
#define AXIS_R2_TRIG        5

#endif // PLATFORM_H
