// platform_brick.h — TrimUI Brick (and Smart Pro). NextUI CFW.
// SoC: Allwinner A133P, aarch64. Panel: 1024x768 landscape (no rotation).

#ifndef PLATFORM_H
#define PLATFORM_H

#define PLATFORM_NAME       "brick"

#define FB_W                1024
#define FB_H                768
#define VFB_W               1024
#define VFB_H               768
#define ROT_DST_INIT        {0, 0, 1024, 768}
#define ROT_ANGLE           0.0

// NDS native 256x192. 4x = 1024x768 = exact panel fill.
#define PP_SCALE_MAIN       4
#define PP_SCALE_FOCUS      3
#define PP_SCALE_PIP        1

// D-pad arrives as HAT events (JOY_UP..RIGHT = -1 selects the hook's hat mode).
#define JOY_UP              -1
#define JOY_DOWN            -1
#define JOY_LEFT            -1
#define JOY_RIGHT           -1

#define JOY_A               1
#define JOY_B               0
#define JOY_X               3
#define JOY_Y               2

#define JOY_L1              4
#define JOY_R1              5
#define JOY_L2              -1      // analog trigger, see AXIS_L2_TRIG
#define JOY_R2              -1      // analog trigger, see AXIS_R2_TRIG

#define JOY_SELECT          6
#define JOY_START           7
#define JOY_MENU            8
#define JOY_PLUS            14
#define JOY_MINUS           13
#define JOY_L3              9
#define JOY_R3              10

#define AXIS_LX             0
#define AXIS_LY             1
#define CURSOR_PRESS_BTN    JOY_R3  // stick click = touch tap (Brick has R3)

#define AXIS_L2_TRIG        2
#define AXIS_R2_TRIG        5

// Boot-resume 3D wait: the 64-bit DraStic's anti-tamper can stamp the
// just-loaded geometry FIFO if a boot-resume load lands too early, so defer
// the resume load until the 3D engine is live. 64-bit only (the armhf binary
// is immune; the code is #ifndef DRASTIC_ARM32).
#define NDS_RESUME_WAIT_3D 1

#endif // PLATFORM_H
