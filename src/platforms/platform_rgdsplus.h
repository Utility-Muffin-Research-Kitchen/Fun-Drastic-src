// Fun Drastic target for the Anbernic RG DS Plus on ROCKNIX.
#ifndef PLATFORM_H
#define PLATFORM_H

#define PLATFORM_NAME       "rgdsplus"
#define FUN_DUAL_PANEL      1

#define FB_W                1024
#define FB_H                768
#define VFB_W               1024
#define VFB_H               768
#define ROT_DST_INIT        {0, 0, 1024, 768}
#define ROT_ANGLE           0.0

#define PP_SCALE_MAIN       4
#define PP_SCALE_FOCUS      3
#define PP_SCALE_PIP        1

// SDL joystick indices for InputPlumber's virtual DualSense. The D-pad is a hat.
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
#define JOY_L3              11
#define JOY_R3              12
#define JOY_PLUS            -1
#define JOY_MINUS           -1

#define AXIS_LX             0
#define AXIS_LY             1
#define AXIS_L2_TRIG        2
#define AXIS_R2_TRIG        5
#define CURSOR_PRESS_BTN    11

#endif
