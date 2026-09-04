/*
 * Fun Drastic - a themed, standalone frontend for the DraStic Nintendo DS
 * emulator, delivered as an LD_PRELOAD hook (libfundrastic.so). It wraps the
 * stock DraStic binary's SDL2 calls to draw its own menu, save states, cheats,
 * screen layouts, overlays and touch cursor - without modifying the emulator.
 *
 * One source builds every target: 64-bit devices compile it as is; 32-bit
 * (armhf) devices add -DDRASTIC_ARM32. Per-device settings come from the
 * platform_<target>.h included at compile time. Fixed addresses into the
 * DraStic binary are marked "disassembly-verified".
 *
 * Not affiliated with or endorsed by Exophase (DraStic) or Nintendo.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <SDL2/SDL.h>
#include <time.h>
#include <math.h>
#include <sys/time.h>   /* gettimeofday - pacing valve (drastic clock parity) */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#include "lib/stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "lib/stb_truetype.h"

#define SDCARD_PATH_ENV     "SDCARD_PATH"
#define SDCARD_DEFAULT      "/mnt/SDCARD"
#define SAVES_DIR           "Saves/NDS"
#define STATES_DIR          "Saves/NDS/states"
#define PREVIEWS_DIR        "Saves/NDS/previews"

#define DS_W    256     // NDS native screen width (never changes)
#define DS_H    192     // NDS native screen height

#define MAX_SLOTS   8
#define MAX_PATH    512
#define MAX_FILE    256
#define MENU_TAP_MS 250

static SDL_Rect g_rot_dst = ROT_DST_INIT;
#define ROT_DST g_rot_dst

static int g_vfb_w = VFB_W;
static int g_vfb_h = VFB_H;
#undef  VFB_W
#undef  VFB_H
#define VFB_W g_vfb_w
#define VFB_H g_vfb_h

static int g_pp_scale_main  = PP_SCALE_MAIN;
static int g_pp_scale_focus = PP_SCALE_FOCUS;
static int g_pp_scale_pip   = PP_SCALE_PIP;
#undef  PP_SCALE_MAIN
#undef  PP_SCALE_FOCUS
#undef  PP_SCALE_PIP
#define PP_SCALE_MAIN  g_pp_scale_main
#define PP_SCALE_FOCUS g_pp_scale_focus
#define PP_SCALE_PIP   g_pp_scale_pip

#define HAT_UP_BIT    28
#define HAT_DOWN_BIT  29
#define HAT_LEFT_BIT  30
#define HAT_RIGHT_BIT 31
#define TRIG_L2_BIT   20   // virtual bit for L2 analog trigger
#define TRIG_R2_BIT   21   // virtual bit for R2 analog trigger
#define TRIG_THRESHOLD 16000  // axis value to register as pressed

/* Default fake-mic trigger: L2 (free of DS use). The hook drives the mic off
   this button held; mic_trigger_held() accepts L2/R2 as a button or an axis. */
#if JOY_L2 >= 0
#define MIC_DEFAULT_BTN  JOY_L2
#elif defined(AXIS_L2_TRIG) && AXIS_L2_TRIG >= 0
#define MIC_DEFAULT_BTN  TRIG_L2_BIT
#else
#define MIC_DEFAULT_BTN  (-1)
#endif

#if JOY_UP == -1
#undef  JOY_UP
#undef  JOY_DOWN
#undef  JOY_LEFT
#undef  JOY_RIGHT
#define JOY_UP    HAT_UP_BIT
#define JOY_DOWN  HAT_DOWN_BIT
#define JOY_LEFT  HAT_LEFT_BIT
#define JOY_RIGHT HAT_RIGHT_BIT
#endif

#define VIEW_MAIN         0
#define VIEW_OPTIONS      1
#define VIEW_LAYOUT       2
#define VIEW_SHORTCUTS    3
#define VIEW_CONTROLS     4
#define VIEW_AUDIOVISUAL  5
#define VIEW_EMULATOR     6
#define VIEW_USER         7
#define VIEW_OVERLAY      8
#define VIEW_CHEATS       9

#define MI_CONTINUE     0
#define MI_SAVE         1
#define MI_LOAD         2
#define MI_OPTIONS      3
#define MI_RESET        4
#define MI_QUIT         5
#define MI_COUNT        6

#define OPT_AUDIOVISUAL   0
#define OPT_EMULATOR      1
#define OPT_CHEATS        2
#define OPT_USER          3
#define OPT_SHORTCUTS     4
#define OPT_CONTROLS      5
#define OPT_THEME         6
#define OPT_LANGUAGE      7
#define OPT_COUNT         8

#define AV_LAYOUT         0  // screen layout picker
#define AV_OVERLAY        1  // overlay pack picker
#define AV_SWAP_SCREENS   2  // swap top/bottom NDS screens
#define AV_QUALITY        3  // image quality (crisp/smooth)
#define AV_SCREEN_BLEND   4  // interframe_blend — requires reset
#define AV_EDGE_MARKING   5  // !disable_edge_marking — requires reset
#define AV_HIRES_3D       6  // hires_3d — requires reset
#define AV_CURSOR_SPEED   7  // touch-cursor speed multiplier (hook setting)
#define AV_COUNT          8

#define USR_PLAYER_NAME   0  // firmware.username
#define USR_LANGUAGE      1  // firmware.language
#define USR_FAV_COLOR     2  // firmware.favorite_color
#define USR_BIRTHDAY      3  // firmware.birthday month+day
#define USR_COUNT         4

#define EMU_AUTO_RESUME   0  // auto-resume on boot
#define EMU_CUSTOM_CLOCK  1  // rtc_mode: OFF/SYSTEM/CUSTOM + picker
#define EMU_FRAMESKIP     2  // frameskip_type: OFF/AUTO/MANUAL
#define EMU_FRAMESKIP_VAL 3  // frameskip_value 1-9 (MANUAL only)
#define EMU_SAFE_SKIP     4  // safe_frameskip
#define EMU_CPU_SPEED     5  // clock_speed
#define EMU_THREADED_3D   6  // threaded_3d
#define EMU_UNZIP_ROMS    7  // unzip_roms
#define EMU_FIX_2D        8  // fix_main_2d_screen
#define EMU_SLOT2         9  // slot2_device_type
#define EMU_COMPRESS     10  // compress_savestates
#define EMU_BACKUP_SAV   11  // backup_use_sav_format
#define EMU_ROM_HACK     12  // ignore_gamecard_limit
#define EMU_COUNT        13

#define LAYOUT_COUNT    5
#define LAYOUT_MENU_ITEMS 6
#define FF_TEMP_SLOT    9   // save slot used during FF speed change (transparent to user)
#define CHEAT_TEMP_SLOT 8   // save slot used during cheat hot-apply (transparent to user)

#define NAME_MAX_LEN    10

#define SC_SAVE_STATE     0
#define SC_LOAD_STATE     1
#define SC_FAST_FORWARD   2
#define SC_SWAP_SCREENS   3
#define SC_NEXT_LAYOUT    4
#define SC_PREV_LAYOUT    5
#define SC_CURSOR         6   /* touch-cursor toggle */
#define SC_COUNT          7

#define CTRL_COUNT   12               /* dpad/face/shoulder/select/start */
#define CTRL_MIC     CTRL_COUNT       /* extra row: fake-mic trigger */
#define CTRL_ROWS    (CTRL_COUNT + 1)

static int g_prev_x     = 312;
static int g_prev_y     =  68;
static int g_prev_w     = 296;
static int g_prev_h     = 222;
static int g_footer_y   = 428;
static int g_footer_h   =  52;
static int g_content_y  =  52;
static int g_menu_rh    =  62;
static int g_menu_vis   =   6;
static int MN_SML       =   1;
static int MN_NRM       =   2;
static int MN_BIG       =   3;

#define FOOTER_Y    g_footer_y
#define FOOTER_H    g_footer_h
#define CONTENT_Y   g_content_y
#define MENU_RH     g_menu_rh
#define PREV_X      g_prev_x
#define PREV_Y      g_prev_y
#define PREV_W      g_prev_w
#define PREV_H      g_prev_h

#define NDS_INPUT_OFFSET  0x80000
#define NDS_KEY_BIT_FAST  0x0200000

/* input_struct location + touch fields (disassembly-verified). The hook
   writes touch straight here and owns the cursor. */
#ifdef DRASTIC_ARM32
#define NDS_TOUCH_INPUT_OFFSET 0x852F4
#define NDS_IN_BTN       0x08
#define NDS_IN_TOUCH_X   0x0c
#define NDS_IN_TOUCH_Y   0x10
#define NDS_IN_TOUCH_ST  0x14
#define NDS_IN_TOUCH_PR  0x15
#else
#define NDS_TOUCH_INPUT_OFFSET 0x85550
#define NDS_IN_BTN       0x10
#define NDS_IN_TOUCH_X   0x14
#define NDS_IN_TOUCH_Y   0x18
#define NDS_IN_TOUCH_ST  0x1c
#define NDS_IN_TOUCH_PR  0x1d
#endif

/* Stick cursor. Deflection shows the overlay, idling hides it; speed is
   squared on the radial magnitude with per-direction range learning. */
#ifndef NDS_AXV
#define NDS_AXV(v) (v)
#endif
#define CURSOR_STICK_SPEED     280.0f
#define CURSOR_SPEED_COUNT     5
#define CURSOR_SPEED_DEFAULT   2      /* NORMAL = the tuned baseline (1.0x) */
#define CURSOR_SHOW_MS         2500
#define CURSOR_STICK_DETECT    12000
#define CURSOR_SILENCE_MS      2000
#define CURSOR_RANGE_FLOOR     12000.0f
#define CURSOR_DEADZONE_FRAC   0.28f
static uint32_t g_cur_live_x_ms = 0, g_cur_live_y_ms = 0;
static float g_cur_max_xp = CURSOR_RANGE_FLOOR, g_cur_max_xn = CURSOR_RANGE_FLOOR;
static float g_cur_max_yp = CURSOR_RANGE_FLOOR, g_cur_max_yn = CURSOR_RANGE_FLOOR;

/* DraStic binary offsets. drastic64 is PIE, so these are base-relative
   (app.base = load slide). The armhf drastic is non-PIE at 0x08000000, so
   find_exe_base() returns 0 and the ARM32 offsets are ABSOLUTE addresses.
   QUIT/LOAD_STATE/SAVE_STATE/VAR_SYSTEM are disassembly-verified; the 0x0
   ARM32 placeholders (in-place reset, ROM reload, is-saving probe) are made
   inert by the constant guards at each call site. */
#ifdef DRASTIC_ARM32
#define OFF_QUIT            0x08006444
#define OFF_RESET_SYSTEM    0x0
#define OFF_LOAD_NDS        0x0
#define OFF_LOAD_STATE      0x080951c0
#define OFF_SAVE_STATE      0x0809580c
#define OFF_VAR_SYSTEM      0x083f4000   /* direct struct address (non-PIE) */
#define OFF_IS_SAVING       0x0
#define OFF_MAIN_LONGJMP    0x0
#define OFF_AUDIO_CAPTURE_FLUSH 0x080aa894
#else
#define OFF_QUIT            0x0000e8d0
#define OFF_RESET_SYSTEM    0x0000fd50
#define OFF_LOAD_NDS        0x0006fd30
#define OFF_LOAD_STATE      0x000746f0
#define OFF_SAVE_STATE      0x00074da0
#define OFF_VAR_SYSTEM      0x15ff30
#define OFF_IS_SAVING       0x3ec27c
#define OFF_MAIN_LONGJMP    0x3b2a840
#endif

/* Fake microphone, driven directly (disassembly-verified). DraStic's own
   control-matching does not fire the mic on this 64-bit build, so the hook
   calls spu_fake_microphone_start/stop itself when the mic trigger is held.
   The base the emulator passes them is nds + 0x1587000, where nds is the
   pointer stored just past the input struct the touch code already writes. */
#define OFF_SPU_FAKE_MIC_START  0x6c8b0
#define OFF_SPU_FAKE_MIC_STOP   0x6c8d0
#define NDS_PTR_OFF        (NDS_TOUCH_INPUT_OFFSET + 8)  /* *(sys+this) = nds */
#define NDS_SPU_ARG_OFF    0x1587000   /* spu_fake_microphone_* base = nds + this */
#define NDS_SPU_STRUCT_OFF 0x15c7000   /* SPU struct = nds + this (arg + 0x40000) */
#define SPU_MIC_ACTIVE     0xd40       /* active flag, 0 or 1 */
#define SPU_MIC_WAV        0xd30       /* loaded WAV data ptr, 0 = none */

#ifdef DRASTIC_ARM32
/* The armhf DraStic needs audio_capture_flush neutralised with an in-place
   code patch (the 64-bit build does not). Overwrite the target's first 8
   bytes with `ldr pc,[pc,#-4]` + the absolute address of a no-op. */
static void arm32_noop(void) { }
static void arm32_patch_fn(uintptr_t target, uintptr_t cb) {
    size_t page = sysconf(_SC_PAGESIZE);
    void  *pg   = (void *)(target & ~(page - 1));
    if (mprotect(pg, page * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return;
    volatile uint8_t *m = (uint8_t *)target;
    m[0] = 0x04; m[1] = 0xf0; m[2] = 0x1f; m[3] = 0xe5;   /* ldr pc, [pc, #-4] */
    m[4] = cb & 0xff;         m[5] = (cb >> 8)  & 0xff;
    m[6] = (cb >> 16) & 0xff; m[7] = (cb >> 24) & 0xff;
    __clear_cache((char *)target, (char *)target + 8);
}
static void arm32_patch_audio(void) {
    arm32_patch_fn(OFF_AUDIO_CAPTURE_FLUSH, (uintptr_t)arm32_noop);
}
#endif

typedef int     (*drastic_main_t)(int, char **, char **);
typedef void    (*drastic_quit_t)(void *);
typedef void    (*drastic_reset_system_t)(void *);
typedef int32_t (*drastic_load_nds_t)(void *, const char *);
typedef int32_t (*drastic_load_state_t)(void *, const char *, uint16_t *, uint16_t *, uint32_t);
typedef int32_t (*drastic_save_state_t)(void *, const char *, char *, uint16_t *, uint16_t *);

typedef struct {
    SDL_Rect rect;
    uint8_t  alpha;
} screen_slot_t;

static struct {
    uintptr_t base;

    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *virtual_fb;
    SDL_Texture  *screens[2];

    char rom_path[MAX_PATH];
    char rom_name[MAX_FILE];
    char rom_display[MAX_FILE];

    SDL_Rect drastic_touch_rect;

    int  in_menu;
    int  quitting;
    volatile uint32_t last_render_ticks;
    volatile int  plat_busy;    /* >0 parks the watchdog (nesting counter) */
    uint32_t launch_time;
    int  view;
    int  menu_item;
    int  state_slot;            /* save target; also the quit/resume slot */
    int  load_slot;             /* load source - independent of save */
    int  ff_speed;
    int          ff_restore_pending;
    int          cheat_restore_pending;
    int          cheat_apply_pending;
    SDL_Texture *pip_deferred_tex;
    SDL_Rect     pip_deferred_rect;
    int          pip_deferred_alpha;
    int  layout;
    int  pip_corner;
    int  pixel_perfect;

    #define MAX_OVERLAY_PACKS 32
    #define OVERLAY_NAME_LEN  64
    char overlay_packs[MAX_OVERLAY_PACKS][OVERLAY_NAME_LEN];
    int  overlay_pack_count;
    char overlay_pack_name[OVERLAY_NAME_LEN];
    int  overlay_pack_idx;
    SDL_Texture *overlay_tex;
    int          overlay_needs_load;
    SDL_Texture *overlay_preview_tex;
    int          overlay_preview_idx;
    int  swap_screens;
    int  crisp;

    int  screen_blend;
    int  edge_marking;
    int  hires_3d;

    int  frameskip;
    int  frameskip_val;
    int  safe_skip;
    int  threaded_3d;
    int  clock_speed;
    int  fix_2d;
    int  slot2_device;
    int  auto_resume;
    int  rom_hack;
    int  compress_states;
    int  fw_language;
    int  fw_bday_month;
    int  fw_bday_day;
    int  fw_fav_color;
    char fw_username[11];

    int  rtc_mode;
    int  unzip_roms;
    int  backup_sav;
    int  rtc_year;
    int  rtc_month;
    int  rtc_day;
    int  rtc_hour;
    int  rtc_minute;

    int  overlay_scroll;
    int  av_scroll;
    int  emu_scroll;
    int  emu_bday_editing;
    int  emu_bday_cursor;
    int  emu_clock_editing;
    int  emu_clock_cursor;
    int  prepare_frames;

    char drastic_dir[MAX_PATH];

    void        *game_bg_pixels;
    SDL_Texture *game_bg_tex;

    SDL_Texture *preview_tex;
    int          preview_slot;

    int sc_btn[SC_COUNT];
    int sc_mod[SC_COUNT];

    int ctrl_map[CTRL_COUNT];
    int mic_btn;                /* fake-mic trigger (SDL button, -1 = off) */
    char lang_name[24];         /* UI language ("english" = built-in) */

    uint32_t joy_held;

    int   cursor_mode;
    int   cursor_speed;           /* 0..CURSOR_SPEED_COUNT-1, index into g_cursor_speed_mult */
    int   cursor_hdx;
    int   cursor_hdy;
    SDL_Texture *cursor_tex;
    float cursor_nx;
    float cursor_ny;
    int      cursor_stick_seen;   /* runtime: this device has a live stick */
    Sint16   cursor_ax;           /* latest raw LX/LY (consumed from drastic) */
    Sint16   cursor_ay;
    uint32_t cursor_move_ms;      /* last stick activity - overlay auto-show */
    int      cursor_touch_down;   /* pen is down at (nx,ny) */

    int sc_binding;
    int ctrl_binding;

    int slot_arrow_flash;
    int slot_arrow_timer;

    int  theme;
    int  cheat_scroll;
} app;

#define DRASTIC_FN(off)  ((void *)(app.base + (off)))
#define DRASTIC_VAR(off) (*(void **)(app.base + (off)))

static const char *sdcard_path(void);
static void *drastic_system(void);
static void drastic_save_state(int);
static void drastic_load_state(int);
static void drastic_cfg_set(const char *cfg_path, const char *key, const char *val);
/* cheat entries are a dynamically-grown array sized to the loaded game's
   cheat count — see cheat_parse_usrcheat. */
#define CHEAT_NAME_LEN   64
#define MAX_CHEAT_CODES  32   // max AR code pairs per cheat

typedef struct {
    char     name[CHEAT_NAME_LEN];
    uint8_t  enabled;
    uint32_t codes[MAX_CHEAT_CODES * 2];
    int      num_codes;
} CheatEntry;

static struct {
    CheatEntry *entries;   // dynamically grown to the game's cheat count
    int        cap;        // allocated capacity of `entries`
    int        count;
    char       game_code[8];
    int        loaded;
    int        dirty;
    CheatEntry master_entry;
    int        has_master;
} g_cheats;

static void cheat_read_game_code(void) {
    g_cheats.game_code[0] = '\0';
    if (!app.rom_path[0]) return;

    FILE *f = fopen(app.rom_path, "rb");
    if (!f) return;

    uint8_t hdr[0x20];
    if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)) {
        memcpy(g_cheats.game_code, hdr + 0x0C, 4);
        g_cheats.game_code[4] = '\0';
    }
    fclose(f);

}

static void cheat_sidecar_path(char *out, size_t n) {
    snprintf(out, n, "%s/%s/%s.chx",
             sdcard_path(), SAVES_DIR, g_cheats.game_code);
}

static void cheat_load_enabled(void) {
    char path[MAX_PATH];
    cheat_sidecar_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[CHEAT_NAME_LEN];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        for (int i = 0; i < g_cheats.count; i++) {
            if (strcmp(g_cheats.entries[i].name, line) == 0) {
                g_cheats.entries[i].enabled = 1;
                break;
            }
        }
    }
    fclose(f);
}

static void cheat_save_enabled(void) {
    if (!g_cheats.dirty) return;
    char path[MAX_PATH];
    cheat_sidecar_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_cheats.count; i++)
        if (g_cheats.entries[i].enabled)
            fprintf(f, "%s\n", g_cheats.entries[i].name);
    fclose(f);
    g_cheats.dirty = 0;
}

static void cheat_write_cht(void) {
    if (!g_cheats.game_code[0]) return;

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/cheats/%s.cht",
             app.drastic_dir[0] ? app.drastic_dir : ".", app.rom_name);

    int active = 0;
    for (int i = 0; i < g_cheats.count; i++)
        if (g_cheats.entries[i].enabled && g_cheats.entries[i].num_codes > 0)
            active++;

    if (active == 0) {

        unlink(path);
        return;
    }

    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s/cheats",
             app.drastic_dir[0] ? app.drastic_dir : ".");
    mkdir(dir, 0755);

    FILE *f = fopen(path, "w");
    if (!f) return;

    if (g_cheats.has_master) {
        CheatEntry *m = &g_cheats.master_entry;
        fprintf(f, "[%s]+\n", m->name);
        for (int c = 0; c < m->num_codes; c++)
            fprintf(f, "%08X %08X\n", m->codes[c*2], m->codes[c*2+1]);
    }

    for (int i = 0; i < g_cheats.count; i++) {
        CheatEntry *e = &g_cheats.entries[i];
        if (!e->enabled || !e->num_codes) continue;
        fprintf(f, "[%s]+\n", e->name);
        for (int c = 0; c < e->num_codes; c++)
            fprintf(f, "%08X %08X\n", e->codes[c*2], e->codes[c*2+1]);
    }
    fclose(f);
}

static void cheat_parse_usrcheat(void) {
    g_cheats.count  = 0;
    g_cheats.loaded = 0;

    /* free any prior game's list — the array is sized per-game, on demand */
    free(g_cheats.entries);
    g_cheats.entries = NULL;
    g_cheats.cap     = 0;

    cheat_read_game_code();

    if (!g_cheats.game_code[0]) {
        return;
    }

    char dat[MAX_PATH];
    snprintf(dat, sizeof(dat), "%s/config/usrcheat.dat",
             app.drastic_dir[0] ? app.drastic_dir : ".");
    FILE *f = fopen(dat, "rb");
    if (!f) {
        snprintf(dat, sizeof(dat), "%s/cheats/usrcheat.dat",
                 app.drastic_dir[0] ? app.drastic_dir : ".");
        f = fopen(dat, "rb");
    }
    if (!f) {
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    if (fsize <= 0 || fsize > 32*1024*1024) {
        fclose(f); return;
    }
    uint8_t *buf = malloc(fsize);
    if (!buf) { fclose(f); return; }
    if ((long)fread(buf, 1, fsize, f) != fsize) {
        free(buf); fclose(f);
        return;
    }
    fclose(f);

    uint8_t target[4];
    memcpy(target, g_cheats.game_code, 4);

    uint32_t cheat_data_off = 0;
    for (long i = 0x50; i <= fsize - 16; i += 16) {
        if (buf[i]   == target[0] && buf[i+1] == target[1] &&
            buf[i+2] == target[2] && buf[i+3] == target[3]) {
            memcpy(&cheat_data_off, buf + i + 8, 4);
            break;
        }
    }

    if (!cheat_data_off || cheat_data_off >= (uint32_t)fsize) {
        free(buf); return;
    }

    /* Bound the walk by the NEXT game's block, not by the count field (which is
       unreliable across R4 folder databases). Scan the 16-byte index (starts at
       0x50) for the smallest block offset greater than ours; that's our end. */
    long block_end = fsize;
    for (long i = 0x50; i + 16 <= fsize; i += 16) {
        uint32_t o = 0;
        memcpy(&o, buf + i + 8, 4);
        if (o > cheat_data_off && (long)o < block_end) block_end = (long)o;
    }

    long p = (long)cheat_data_off;

    long title_start = p;
    while (p < fsize && buf[p] != 0) p++;
    p++;
    long title_len = (p - title_start);
    p = title_start + (((title_len) + 3) & ~3);

    if (p + 4 > fsize) { free(buf); return; }
    /* skip the count field — we bound by block_end instead (see above) */
    p += 4;

    while (p + 4 <= block_end) {
        uint32_t body_dwords = 0;
        memcpy(&body_dwords, buf + p, 4); p += 4;

        if (body_dwords == 0) continue;

        /* Folder header: top nibble == 1 (both 0x10.. and 0x11.. occur, so test
           the nibble, not == 0x10). Layout: [word][name\0][note\0] padded to 4.
           Skip the header and let the children parse as normal entries — this is
           the folder-flatten fix that makes grouped R4 cheats list. */
        if ((body_dwords >> 28) == 1) {
            long q = p;
            while (q < block_end && buf[q] != 0) q++;   /* folder name */
            q++;                                        /* skip its NUL */
            while (q < block_end && buf[q] != 0) q++;   /* folder note */
            q++;                                        /* skip its NUL */
            p = (q + 3) & ~3;
            continue;
        }

        long body_size = (long)body_dwords * 4;
        if (p + body_size > block_end) break;
        uint8_t *body = buf + p;
        p += body_size;

        long name_end = 0;
        while (name_end < body_size && body[name_end] != 0) name_end++;
        long name_padded = ((name_end + 1 + 3) & ~3);

        if (name_padded + 4 > body_size) continue;

        int printable = 1;
        for (long k = 0; k < name_end; k++) {
            if (body[k] < 0x20 || body[k] >= 0x7f) { printable = 0; break; }
        }
        if (!printable || name_end == 0) continue;

        uint32_t nc_dwords = 0;
        memcpy(&nc_dwords, body + name_padded, 4);
        int num_pairs = (int)(nc_dwords / 2);
        if (num_pairs < 0 || num_pairs > MAX_CHEAT_CODES) num_pairs = MAX_CHEAT_CODES;

        CheatEntry e = {0};
        int ni = (int)(name_end < CHEAT_NAME_LEN - 1 ? name_end : CHEAT_NAME_LEN - 1);
        memcpy(e.name, body, ni);
        e.name[ni] = '\0';
        e.num_codes = 0;

        long code_off = name_padded + 4;
        for (int k = 0; k < num_pairs && code_off + 8 <= body_size; k++, code_off += 8) {
            if (e.num_codes < MAX_CHEAT_CODES) {
                memcpy(&e.codes[e.num_codes * 2 + 0], body + code_off,     4);
                memcpy(&e.codes[e.num_codes * 2 + 1], body + code_off + 4, 4);
                e.num_codes++;
            }
        }

        int name_lower_is_master = 0;
        {
            char nl[CHEAT_NAME_LEN];
            for (int k = 0; k < ni; k++)
                nl[k] = (e.name[k] >= 'A' && e.name[k] <= 'Z') ? e.name[k]+32 : e.name[k];
            nl[ni] = '\0';
            if (strstr(nl, "must be on") || strstr(nl, "mustbeon") ||
                strstr(nl, "master code") || strstr(nl, "enable code"))
                name_lower_is_master = 1;
        }

        if (name_lower_is_master) {
            if (!g_cheats.has_master) {
                g_cheats.master_entry = e;
                g_cheats.has_master = 1;
            }
            continue;
        }

        if (g_cheats.count >= g_cheats.cap) {
            int ncap = g_cheats.cap ? g_cheats.cap * 2 : 64;
            CheatEntry *ne = realloc(g_cheats.entries,
                                     (size_t)ncap * sizeof(CheatEntry));
            if (!ne) break;   /* out of memory: keep what we've got so far */
            g_cheats.entries = ne;
            g_cheats.cap     = ncap;
        }
        g_cheats.entries[g_cheats.count++] = e;
    }

    free(buf);

    if (g_cheats.count > 0) {
        g_cheats.loaded = 1;
        cheat_load_enabled();
    }

}

static void cheat_apply(void) {
    cheat_save_enabled();
    cheat_write_cht();

    if (app.cheat_restore_pending) return;

    void *sys = drastic_system();
    if (!sys) return;
    /* ARM32: no in-place ROM reload - the .cht is written; cheats take effect
       on the next game load. */
    if (!OFF_LOAD_NDS || !OFF_RESET_SYSTEM || !OFF_MAIN_LONGJMP) return;

    app.cheat_restore_pending = 1;

    drastic_save_state(CHEAT_TEMP_SLOT);
    ((drastic_load_nds_t)DRASTIC_FN(OFF_LOAD_NDS))((uint8_t *)sys + 800, app.rom_path);
    ((drastic_reset_system_t)DRASTIC_FN(OFF_RESET_SYSTEM))(sys);
    app.prepare_frames = 5;
    app.cursor_nx = 128.0f; app.cursor_ny = 92.0f;
    app.last_render_ticks = SDL_GetTicks();
    longjmp(*(jmp_buf *)((uint8_t *)sys + OFF_MAIN_LONGJMP), MN_SML);
}

static const char *sdcard_path(void) {
    const char *p = getenv(SDCARD_PATH_ENV);
    return (p && *p) ? p : SDCARD_DEFAULT;
}

static int           (*real_SDL_Init)(Uint32) = NULL;
static SDL_Window*   (*real_SDL_CreateWindow)(const char*,int,int,int,int,Uint32) = NULL;
static int           (*real_SDL_CreateWindowAndRenderer)(int,int,Uint32,SDL_Window**,SDL_Renderer**) = NULL;
static void          (*real_SDL_SetWindowSize)(SDL_Window*,int,int) = NULL;
static SDL_Renderer* (*real_SDL_CreateRenderer)(SDL_Window*,int,Uint32) = NULL;
static int           (*real_SDL_RenderClear)(SDL_Renderer*) = NULL;
static void          (*real_SDL_RenderPresent)(SDL_Renderer*) = NULL;
static void          (*real_SDL_DestroyRenderer)(SDL_Renderer*) = NULL;
static void          (*real_SDL_DestroyWindow)(SDL_Window*) = NULL;
static void          (*real_SDL_CloseAudio)(void) = NULL;
static void          (*real_SDL_PauseAudio)(int) = NULL;
static int           (*real_SDL_OpenAudio)(SDL_AudioSpec*, SDL_AudioSpec*) = NULL;
static void          (*real_SDL_Quit)(void) = NULL;
static SDL_Texture*  (*real_SDL_CreateTexture)(SDL_Renderer*,Uint32,int,int,int) = NULL;
static void          (*real_SDL_DestroyTexture)(SDL_Texture*) = NULL;
static int           (*real_SDL_RenderCopy)(SDL_Renderer*,SDL_Texture*,const SDL_Rect*,const SDL_Rect*) = NULL;
static int           (*real_SDL_RenderCopyEx)(SDL_Renderer*,SDL_Texture*,const SDL_Rect*,const SDL_Rect*,double,const SDL_Point*,SDL_RendererFlip) = NULL;
static int           (*real_SDL_SetRenderTarget)(SDL_Renderer*,SDL_Texture*) = NULL;
static int           (*real_SDL_RenderSetLogicalSize)(SDL_Renderer*,int,int) = NULL;
static int           (*real_SDL_PollEvent)(SDL_Event*) = NULL;
static void          (*real_SDL_Delay)(uint32_t) = NULL;
static uint32_t      (*real_SDL_GetTicks)(void)  = NULL;

static int  (*real__libc_start_main)(int(*)(int,char**,char**),int,char**,void(*)(void),void(*)(void),void(*)(void),void*) = NULL;
static int  (*real__snprintf_chk)(char*,size_t,int,size_t,const char*,...) = NULL;
static int  (*real__sprintf_chk)(char*,int,size_t,const char*,...) = NULL;
static void (*real_exit)(int) = NULL;
static void (*real__exit)(int) = NULL;
static int  (*real_system)(const char*) = NULL;

typedef int (*sdl_set_scale_mode_t)(SDL_Texture *, int);
static sdl_set_scale_mode_t fn_SetTextureScaleMode = NULL;

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;

static uint32_t PAD_curr = 0;
static uint32_t PAD_prev = 0;

/* FUN_KB_INPUT: platforms that deliver buttons as keyboard events.
   Mapped keys are mirrored onto the JOY_* button space so the menu, nav
   and shortcuts work unchanged. */
#ifdef FUN_KB_INPUT
#ifndef FUN_KB_UP
#define FUN_KB_UP     SDLK_UP
#define FUN_KB_DOWN   SDLK_DOWN
#define FUN_KB_LEFT   SDLK_LEFT
#define FUN_KB_RIGHT  SDLK_RIGHT
#define FUN_KB_A      SDLK_SPACE
#define FUN_KB_B      SDLK_LCTRL
#define FUN_KB_X      SDLK_z
#define FUN_KB_Y      SDLK_x
#define FUN_KB_L1     SDLK_LSHIFT
#define FUN_KB_R1     SDLK_c
#define FUN_KB_START  SDLK_RETURN
#define FUN_KB_SELECT SDLK_RSHIFT
#define FUN_KB_MENU   SDLK_m
#endif
/* some hosts deliver the Menu button as HOME - accept both */
#ifndef FUN_KB_MENU2
#define FUN_KB_MENU2  SDLK_HOME
#endif
static int kb_to_btn(int sym) {
    if (sym == FUN_KB_MENU)  return JOY_MENU;
    if (sym == FUN_KB_MENU2) return JOY_MENU;
    switch (sym) {
    case FUN_KB_UP:     return JOY_UP;
    case FUN_KB_DOWN:   return JOY_DOWN;
    case FUN_KB_LEFT:   return JOY_LEFT;
    case FUN_KB_RIGHT:  return JOY_RIGHT;
    case FUN_KB_A:      return JOY_A;
    case FUN_KB_B:      return JOY_B;
    case FUN_KB_X:      return JOY_X;
    case FUN_KB_Y:      return JOY_Y;
    case FUN_KB_L1:     return JOY_L1;
    case FUN_KB_R1:     return JOY_R1;
    case FUN_KB_START:  return JOY_START;
    case FUN_KB_SELECT: return JOY_SELECT;
    default:            return -1;
    }
}
#endif

/* FUN_EVLOG=1 turns on diagnostics: input events, pacing-valve resyncs,
   and the shortcut combo probe */
static int fun_evlog(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("FUN_EVLOG"); v = (e && *e == '1') ? 1 : 0; }
    return v;
}
static void evlog_event(const SDL_Event *e, const char *src) {
    if (!fun_evlog()) return;
    switch (e->type) {
    case SDL_KEYDOWN: case SDL_KEYUP:
        fprintf(stderr, "[fun-ev %s] %s sym=%d scan=%d rep=%d\n", src,
                e->type == SDL_KEYDOWN ? "KEYDOWN" : "KEYUP",
                (int)e->key.keysym.sym, (int)e->key.keysym.scancode,
                (int)e->key.repeat);
        break;
    case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP:
        fprintf(stderr, "[fun-ev %s] %s btn=%d\n", src,
                e->type == SDL_JOYBUTTONDOWN ? "JOYDOWN" : "JOYUP",
                (int)e->jbutton.button);
        break;
    case SDL_JOYHATMOTION:
        fprintf(stderr, "[fun-ev %s] HAT hat=%d val=%d\n", src,
                (int)e->jhat.hat, (int)e->jhat.value);
        break;
    case SDL_WINDOWEVENT:
        /* subtype ids: 10 ENTER 11 LEAVE 12 FOCUS_GAINED 13 FOCUS_LOST
           5 RESIZED 6 SIZE_CHANGED 8 MINIMIZED 14 CLOSE 15 TAKE_FOCUS */
        fprintf(stderr, "[fun-ev %s] WINDOW ev=%d\n", src, (int)e->window.event);
        break;
    case SDL_JOYDEVICEADDED:
    case SDL_JOYDEVICEREMOVED:
        fprintf(stderr, "[fun-ev %s] JOYDEV %s which=%d\n", src,
                e->type == SDL_JOYDEVICEADDED ? "ADD" : "DEL", (int)e->jdevice.which);
        break;
    case SDL_JOYAXISMOTION:
    case SDL_MOUSEMOTION:
        break; /* too chatty */
    default:
        fprintf(stderr, "[fun-ev %s] type=0x%x\n", src, (unsigned)e->type);
        break;
    }
}

#ifdef NDS_EVDEV_PAD
/* ── Raw-evdev game-pad bridge ──────────────────────────────────────────────
 * Some device images expose the game pad through NEITHER an SDL joystick NOR
 * SDL keyboard events - the SDL input path DraStic (and this hook) rely on is
 * simply dead. Set NDS_EVDEV_PAD in the platform header for such a device and
 * the hook reads the kernel evdev nodes directly instead, driving both:
 *   - its own menu, through app.joy_held (JOY_* / HAT_* bit space), and
 *   - DraStic's button_status, written straight into its input_struct - the
 *     same "the hook owns the field" contract nds_touch_enforce() uses for touch.
 *
 * Bring-up on a new device: use `evtest` to find which /dev/input/eventN carry
 * the pad, then map the raw EV_KEY codes to the JOY_* logical buttons in
 * evp_map() below. The d-pad and analog stick usually arrive as EV_ABS. */
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>

/* DraStic's button_status uses CONTROL_INDEX bit order (UP,DOWN,LEFT,RIGHT,
 * A,B,X,Y,L,R,START,SELECT) - NOT the NDS hardware KEYINPUT order. */
#define NB_UP     0x0001u
#define NB_DOWN   0x0002u
#define NB_LEFT   0x0004u
#define NB_RIGHT  0x0008u
#define NB_A      0x0010u
#define NB_B      0x0020u
#define NB_X      0x0040u
#define NB_Y      0x0080u
#define NB_L      0x0100u
#define NB_R      0x0200u
#define NB_START  0x0400u
#define NB_SELECT 0x0800u

/* Nodes/codes below are for the H700 image this target was brought up on -
 * re-derive them with evtest for another device. The pad node and the two
 * system nodes report different code sets; both are covered. */
enum { EVP_PAD = 0, EVP_SYS0, EVP_SYS1, EVP_N };
static int          g_evp_fd[EVP_N]  = { -1, -1, -1 };
static const char  *g_evp_node[EVP_N] =
    { "/dev/input/event3", "/dev/input/event0", "/dev/input/event1" };
static uint32_t     g_evp_joy = 0;   /* JOY_ / HAT_ bit space (menu + shortcuts) */
static uint32_t     g_evp_nds = 0;   /* button_status bit space (the game) */
static int          g_evp_menu_active = 0;   /* menu owned input this frame */
static int          g_evp_lsx = 0, g_evp_lsy = 0;   /* left stick, SDL +-32767 scale */
static uint32_t     g_evp_synth_suppress = 0;   /* buttons held across menu close */

/* Raw EV_KEY code -> (JOY_* bit, button_status bit) for this device. */
static void evp_map(int node, int code, int *joybit, uint32_t *nds) {
    *joybit = -1; *nds = 0;
    if (node == EVP_PAD) {
        switch (code) {
        case 305: *joybit = JOY_A;      *nds = NB_A;      break;
        case 304: *joybit = JOY_B;      *nds = NB_B;      break;
        case 308: *joybit = JOY_X;      *nds = NB_X;      break;
        case 307: *joybit = JOY_Y;      *nds = NB_Y;      break;
        case 310: *joybit = JOY_L1;     *nds = NB_L;      break;
        case 311: *joybit = JOY_R1;     *nds = NB_R;      break;
        case 314: *joybit = JOY_SELECT; *nds = NB_SELECT; break;
        case 315: *joybit = JOY_START;  *nds = NB_START;  break;
        case 316: *joybit = JOY_MENU;   *nds = 0;         break;
        }
    } else {
        switch (code) {
        case 103: *joybit = HAT_UP_BIT;    *nds = NB_UP;    break;
        case 108: *joybit = HAT_DOWN_BIT;  *nds = NB_DOWN;  break;
        case 105: *joybit = HAT_LEFT_BIT;  *nds = NB_LEFT;  break;
        case 106: *joybit = HAT_RIGHT_BIT; *nds = NB_RIGHT; break;
        case 304: *joybit = JOY_A;      *nds = NB_A;      break;
        case 305: *joybit = JOY_B;      *nds = NB_B;      break;
        case 307: *joybit = JOY_X;      *nds = NB_X;      break;
        case 306: *joybit = JOY_Y;      *nds = NB_Y;      break;
        case 308: *joybit = JOY_L1;     *nds = NB_L;      break;
        case 309: *joybit = JOY_R1;     *nds = NB_R;      break;
        case 310: *joybit = JOY_SELECT; *nds = NB_SELECT; break;
        case 311: *joybit = JOY_START;  *nds = NB_START;  break;
        case 312: *joybit = JOY_MENU;   *nds = 0;         break;
        case 314: *joybit = JOY_L2;     *nds = 0;         break;
        case 313: *joybit = JOY_L3;     *nds = 0;         break;
        case 315: *joybit = JOY_R2;     *nds = 0;         break;
        case 316: *joybit = JOY_R3;     *nds = 0;         break;
        }
    }
}

static void evp_set(int joybit, uint32_t nds, int down) {
    if (joybit >= 0) {
        if (down) g_evp_joy |=  (1u << joybit);
        else      g_evp_joy &= ~(1u << joybit);
    }
    if (nds) {
        if (down) g_evp_nds |=  nds;
        else      g_evp_nds &= ~nds;
    }
}

/* Drain pending evdev events into g_evp_joy / g_evp_nds (buttons + d-pad) and
 * g_evp_lsx/lsy (analog stick). */
static void nds_evpad_poll(void) {
    static int opened = 0;
    if (!opened) {
        opened = 1;
        for (int i = 0; i < EVP_N; i++)
            g_evp_fd[i] = open(g_evp_node[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fun_evlog())
            fprintf(stderr, "[fun] evpad: fds pad=%d sys0=%d sys1=%d\n",
                    g_evp_fd[0], g_evp_fd[1], g_evp_fd[2]);
    }
    struct input_event ev[32];
    for (int i = 0; i < EVP_N; i++) {
        if (g_evp_fd[i] < 0) continue;
        ssize_t n;
        while ((n = read(g_evp_fd[i], ev, sizeof(ev))) > 0) {
            int cnt = (int)(n / (ssize_t)sizeof(ev[0]));
            for (int k = 0; k < cnt; k++) {
                if (ev[k].type == EV_KEY) {
                    int jb; uint32_t nb;
                    evp_map(i, ev[k].code, &jb, &nb);
                    evp_set(jb, nb, ev[k].value != 0);
                } else if (ev[k].type == EV_ABS && i != EVP_PAD) {
                    int v = ev[k].value;
                    if (ev[k].code == 16) {
                        evp_set(HAT_LEFT_BIT,  NB_LEFT,  v < 0);
                        evp_set(HAT_RIGHT_BIT, NB_RIGHT, v > 0);
                    } else if (ev[k].code == 17) {
                        evp_set(HAT_UP_BIT,   NB_UP,   v < 0);
                        evp_set(HAT_DOWN_BIT, NB_DOWN, v > 0);
                    } else if (ev[k].code == 2) {   /* left-stick X */
                        g_evp_lsx = (v * 32767) / 4096;
                    } else if (ev[k].code == 3) {   /* left-stick Y */
                        g_evp_lsy = (v * 32767) / 4096;
                    }
                }
            }
        }
    }
    if (app.in_menu) g_evp_menu_active = 1;
}

/* Per-frame game feed: poll, publish app.joy_held for the menu, and write
 * DraStic's button_status straight into its input_struct. */
static void nds_evpad_update(void) {
    static uint32_t suppress = 0;   /* game bits held at menu close, hidden till released */
    nds_evpad_poll();
    app.joy_held = g_evp_joy;
    /* On menu close, whatever button is still held (e.g. the A that picked
     * Resume) must not bleed into the game. Latch the still-held bits on the
     * first game poll after menu exit and hide them until physically released. */
    if (g_evp_menu_active && !app.in_menu) {
        suppress = g_evp_nds;
        g_evp_synth_suppress = g_evp_joy;
        g_evp_menu_active = 0;
    }
    suppress &= g_evp_nds;              /* a physical release re-arms */
    g_evp_synth_suppress &= g_evp_joy;
    if (!app.in_menu && !app.quitting) {
        uint32_t out = g_evp_nds & ~suppress;
        /* While MENU is held it is a shortcut modifier (the NDS has no MENU
         * button), and in cursor mode the whole pad drives the pen - the game
         * must see neither. */
        if (app.cursor_mode || (g_evp_joy & (1u << JOY_MENU)))
            out = 0;
        /* Bit 17 of button_status is DraStic's pen-down flag: platform_get_input
         * seeds its input from button_status and derives touch_status =
         * (status >> 17) & 1, then places the pen at touch_x/touch_y. Writing
         * touch_status in the struct directly is a no-op (this bit overwrites it
         * every frame) - THIS is the field that makes a tap land. touch_x/y ride
         * in through nds_touch_enforce(). */
        if (app.cursor_touch_down) out |= 0x00020000u;
        void *sys = drastic_system();
        if (sys) {
            volatile uint32_t *btn =
                (volatile uint32_t *)((uint8_t *)sys + NDS_TOUCH_INPUT_OFFSET + NDS_IN_BTN);
            *btn = out;
        }
    }
}

/* Synthesize one SDL_JOYBUTTON edge from the pad so the hook's SDL-event-based
 * menu / shortcut detection fires on an image that emits no real SDL events.
 * One edge per call, drained across SDL_PollEvent's repoll loop. */
static int nds_evpad_next_event(SDL_Event *e) {
    static uint32_t prev = 0;
    uint32_t cur  = (g_evp_joy & 0xf0001fffu) & ~g_evp_synth_suppress; /* buttons 0-12 + hat 28-31 */
    uint32_t diff = cur ^ prev;
    if (!diff) return 0;
    int bit  = __builtin_ctz(diff);
    int down = (cur >> bit) & 1u;
    prev ^= (1u << bit);
    SDL_memset(e, 0, sizeof(*e));
    e->type           = down ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
    e->jbutton.which  = 0x5A5A;   /* sentinel: hook-synthesized */
    e->jbutton.button = (Uint8)bit;
    e->jbutton.state  = down ? SDL_PRESSED : SDL_RELEASED;
    return 1;
}
#endif /* NDS_EVDEV_PAD */

static void PAD_poll(void) {
#ifdef NDS_EVDEV_PAD
    /* no SDL joystick on this image: read the pad directly */
    PAD_prev = PAD_curr;
    nds_evpad_poll();
    PAD_curr = g_evp_joy;
    app.joy_held = PAD_curr;
    return;
#endif
    SDL_Event e;
    PAD_prev = PAD_curr;
    while (real_SDL_PollEvent(&e)) {
        evlog_event(&e, "pad");
        if (e.type == SDL_JOYBUTTONDOWN) PAD_curr |=  (1u << e.jbutton.button);
        if (e.type == SDL_JOYBUTTONUP)   PAD_curr &= ~(1u << e.jbutton.button);
#ifdef FUN_KB_INPUT
        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
            int kb = kb_to_btn((int)e.key.keysym.sym);
            if (kb >= 0) {
                if (e.type == SDL_KEYDOWN) PAD_curr |=  (1u << kb);
                else                       PAD_curr &= ~(1u << kb);
            }
        }
#endif

        if (e.type == SDL_JOYHATMOTION && e.jhat.hat == 0) {
            PAD_curr &= ~((1u<<HAT_UP_BIT)|(1u<<HAT_DOWN_BIT)|(1u<<HAT_LEFT_BIT)|(1u<<HAT_RIGHT_BIT));
            if (e.jhat.value & SDL_HAT_UP)    PAD_curr |= (1u<<HAT_UP_BIT);
            if (e.jhat.value & SDL_HAT_DOWN)  PAD_curr |= (1u<<HAT_DOWN_BIT);
            if (e.jhat.value & SDL_HAT_LEFT)  PAD_curr |= (1u<<HAT_LEFT_BIT);
            if (e.jhat.value & SDL_HAT_RIGHT) PAD_curr |= (1u<<HAT_RIGHT_BIT);
        }

#if defined(AXIS_L2_TRIG) && AXIS_L2_TRIG >= 0
        if (e.type == SDL_JOYAXISMOTION && e.jaxis.axis == AXIS_L2_TRIG) {
            if (e.jaxis.value >  TRIG_THRESHOLD) PAD_curr |=  (1u << TRIG_L2_BIT);
            else                                  PAD_curr &= ~(1u << TRIG_L2_BIT);
        }
#endif
#if defined(AXIS_R2_TRIG) && AXIS_R2_TRIG >= 0
        if (e.type == SDL_JOYAXISMOTION && e.jaxis.axis == AXIS_R2_TRIG) {
            if (e.jaxis.value >  TRIG_THRESHOLD) PAD_curr |=  (1u << TRIG_R2_BIT);
            else                                  PAD_curr &= ~(1u << TRIG_R2_BIT);
        }
#endif

    }

    app.joy_held = PAD_curr;
}

#define PAD_justPressed(b)  ((PAD_curr & (1u<<(b))) &&  !(PAD_prev & (1u<<(b))))
#define PAD_isPressed(b)    ((PAD_curr & (1u<<(b))) != 0)

static int _pick_exe(struct dl_phdr_info *i, size_t s, void *out) {
    (void)s;
    if (!i->dlpi_name || !i->dlpi_name[0]) {
        *(uintptr_t *)out = (uintptr_t)i->dlpi_addr;
        return 1;
    }
    return 0;
}
static uintptr_t find_exe_base(void) {
    uintptr_t base = 0;
    dl_iterate_phdr(_pick_exe, &base);
    return base;
}

static void apply_scale_mode(SDL_Texture *t) {
    if (fn_SetTextureScaleMode && t)
        fn_SetTextureScaleMode(t, app.crisp ? 0 : 1);
}

static const char *btn_name(int b) {
    if (b == TRIG_L2_BIT) return "L2";
    if (b == TRIG_R2_BIT) return "R2";
#if defined(JOY_L2) && JOY_L2 >= 0
    if (b == JOY_L2) return "L2";
#endif
#if defined(JOY_R2) && JOY_R2 >= 0
    if (b == JOY_R2) return "R2";
#endif
#if defined(JOY_L3) && JOY_L3 >= 0
    if (b == JOY_L3) return "L3";
#endif
#if defined(JOY_R3) && JOY_R3 >= 0
    if (b == JOY_R3) return "R3";
#endif
#if defined(JOY_PLUS) && JOY_PLUS >= 0
    if (b == JOY_PLUS)  return "+";
#endif
#if defined(JOY_MINUS) && JOY_MINUS >= 0
    if (b == JOY_MINUS) return "-";
#endif
    switch (b) {
    case JOY_A:      return "A";
    case JOY_B:      return "B";
    case JOY_X:      return "X";
    case JOY_Y:      return "Y";
    case JOY_L1:     return "L1";
    case JOY_R1:     return "R1";
    case JOY_SELECT: return "SEL";
    case JOY_START:  return "STA";
    case JOY_UP:     return "UP";
    case JOY_DOWN:   return "DN";
    case JOY_LEFT:   return "LFT";
    case JOY_RIGHT:  return "RGT";
    case JOY_MENU:   return "MNU";
    default:         return "?";
    }
}

static const char *sc_combo_name(int btn, int mod, char *buf) {
    if (btn < 0) { buf[0] = '-'; buf[1] = '\0'; return buf; }
    if (mod == 1) snprintf(buf, 32, "MNU+%s", btn_name(btn));
    else          snprintf(buf, 32, "%s",      btn_name(btn));
    return buf;
}

/* Layouts ride the triggers (Menu+R2/L2) and swap-screens sits on a bare
   R2 tap - the DS never used L2/R2, so they are free. Platforms without
   triggers fall back to the older dpad/Y combos. */
#define SC_HAS_TRIG (JOY_L2 >= 0 && JOY_R2 >= 0)
static const int sc_btn_defaults[SC_COUNT] = {
    JOY_R1, JOY_L1, JOY_A,
    SC_HAS_TRIG ? JOY_R2 : JOY_Y,       /* swap screens */
    SC_HAS_TRIG ? JOY_R2 : JOY_RIGHT,   /* next layout  */
    SC_HAS_TRIG ? JOY_L2 : JOY_LEFT,    /* prev layout  */
    JOY_B,
};
static const int sc_mod_defaults[SC_COUNT] = {
    1, 1, 1,
    SC_HAS_TRIG ? 0 : 1,                /* swap: bare R2 tap */
    1, 1, 1,
};

static const int ctrl_defaults[CTRL_COUNT] = {
    JOY_UP, JOY_DOWN, JOY_LEFT, JOY_RIGHT,
    JOY_A,  JOY_B,    JOY_X,    JOY_Y,
    JOY_L1, JOY_R1,   JOY_SELECT, JOY_START,
};

/* Identity remap target - intentionally the same order as ctrl_defaults. */
static const int ctrl_canonical[CTRL_COUNT] = {
    JOY_UP, JOY_DOWN, JOY_LEFT, JOY_RIGHT,
    JOY_A,  JOY_B,    JOY_X,    JOY_Y,
    JOY_L1, JOY_R1,   JOY_SELECT, JOY_START,
};

typedef struct {
    uint8_t BG[4];
    uint8_t HDR[4];
    uint8_t ITEM[4];
    uint8_t SEL[4];
    uint8_t ACC[4];
    uint8_t TXT[4];
    uint8_t DIM[4];
    uint8_t SEP[4];
} Theme;

static const Theme g_themes[] = {

    { .BG   = {  18,  18,  20, 255 },
      .HDR  = {  26,  26,  28, 255 },
      .ITEM = {  36,  36,  38, 200 },
      .SEL  = { 140,  22,  22, 230 },
      .ACC  = { 225,  50,  50, 255 },
      .TXT  = { 230, 230, 230, 255 },
      .DIM  = { 120, 120, 120, 255 },
      .SEP  = {  48,  48,  50, 255 } },

    { .BG   = {  28,  24,  18, 255 },
      .HDR  = {  40,  34,  24, 255 },
      .ITEM = {  52,  44,  30, 200 },
      .SEL  = { 110,  72,  18, 220 },
      .ACC  = { 185, 120,  40, 255 },
      .TXT  = { 235, 219, 178, 255 },
      .DIM  = { 146, 121, 100, 255 },
      .SEP  = {  62,  50,  34, 255 } },

    { .BG   = {  28,  20,  24, 255 },
      .HDR  = {  38,  28,  34, 255 },
      .ITEM = {  52,  38,  46, 200 },
      .SEL  = { 175,  70,  95, 220 },
      .ACC  = { 255, 172, 160, 255 },
      .TXT  = { 245, 225, 220, 255 },
      .DIM  = { 160, 120, 130, 255 },
      .SEP  = {  60,  42,  50, 255 } },

    { .BG   = {  22,  20,  32, 255 },
      .HDR  = {  30,  28,  44, 255 },
      .ITEM = {  42,  38,  58, 200 },
      .SEL  = {  98,  58, 150, 230 },
      .ACC  = { 210, 140, 255, 255 },
      .TXT  = { 248, 248, 242, 255 },
      .DIM  = { 110, 105, 125, 255 },
      .SEP  = {  48,  44,  66, 255 } },

    { .BG   = {  18,  19,  17, 255 },
      .HDR  = {  27,  28,  25, 255 },
      .ITEM = {  38,  40,  35, 200 },
      .SEL  = {  34, 110,  48, 220 },
      .ACC  = {  80, 210,  90, 255 },
      .TXT  = { 235, 238, 230, 255 },
      .DIM  = { 118, 122, 112, 255 },
      .SEP  = {  44,  46,  40, 255 } },
};
#define THEME_COUNT ((int)(sizeof(g_themes) / sizeof(g_themes[0])))
static const char *g_theme_names[] = { "MARIO", "KOOPA", "PEACH", "WARIO", "YOSHI" };

/* Optional custom theme loaded from themes/custom.cfg. A CFW or user gives
   it their own colors (and a name) so the menu matches their system. When
   the file is present it appears as the last theme in the picker. */
static Theme g_custom;
static int   g_has_custom;
static char  g_custom_name[24];
static char  g_custom_font[64];   /* optional TTF/OTF in fonts/ */

static int theme_total(void)         { return THEME_COUNT + (g_has_custom ? 1 : 0); }
static const char *theme_name(int i) {
    if (i >= THEME_COUNT) return g_custom_name[0] ? g_custom_name : "CUSTOM";
    return g_theme_names[i];
}

static const Theme *g_theme = &g_themes[0];

#define C_BG   g_theme->BG[0],  g_theme->BG[1],  g_theme->BG[2],  g_theme->BG[3]
#define C_HDR  g_theme->HDR[0], g_theme->HDR[1], g_theme->HDR[2], g_theme->HDR[3]
#define C_ITEM g_theme->ITEM[0],g_theme->ITEM[1], g_theme->ITEM[2],g_theme->ITEM[3]
#define C_SEL  g_theme->SEL[0], g_theme->SEL[1],  g_theme->SEL[2], g_theme->SEL[3]
#define C_ACC  g_theme->ACC[0], g_theme->ACC[1],  g_theme->ACC[2], g_theme->ACC[3]
#define C_SEP  g_theme->SEP[0], g_theme->SEP[1],  g_theme->SEP[2], g_theme->SEP[3]

#define COL(nm) (SDL_Color){ g_theme->nm[0], g_theme->nm[1], g_theme->nm[2], g_theme->nm[3] }

static void theme_apply(int i) {
    g_theme = (i >= THEME_COUNT && g_has_custom) ? &g_custom : &g_themes[i % THEME_COUNT];
}

/* Parse a hex colour "#RRGGBB" or "#RRGGBBAA" (the # is optional) into an
   RGBA slot. Leaves the slot untouched on a malformed value. */
static void parse_hex_color(const char *v, uint8_t *c) {
    if (*v == '#') v++;
    int n = 0; while (v[n] && v[n] != ' ' && v[n] != '\t') n++;
    if (n != 6 && n != 8) return;
    for (int i = 0; i < n; i++) {
        char d = v[i];
        if (!((d >= '0' && d <= '9') || (d >= 'a' && d <= 'f') || (d >= 'A' && d <= 'F')))
            return;
    }
    unsigned long x = strtoul(v, NULL, 16);
    if (n == 6) { c[0] = (x >> 16) & 0xFF; c[1] = (x >> 8) & 0xFF; c[2] = x & 0xFF; c[3] = 0xFF; }
    else        { c[0] = (x >> 24) & 0xFF; c[1] = (x >> 16) & 0xFF; c[2] = (x >> 8) & 0xFF; c[3] = x & 0xFF; }
}

/* themes/custom.cfg: "name=", optional "font=", and one "key=#RRGGBB[AA]"
   per colour. Any colour left out keeps the default, so a partial file
   still renders. */
static void theme_load_custom(const char *dir) {
    g_has_custom = 0; g_custom_name[0] = '\0'; g_custom_font[0] = '\0';
    if (!dir || !dir[0]) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/themes/custom.cfg", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    g_custom = g_themes[0];
    struct { const char *key; uint8_t *c; } map[] = {
        {"bg", g_custom.BG}, {"header", g_custom.HDR}, {"item", g_custom.ITEM},
        {"selection", g_custom.SEL}, {"accent", g_custom.ACC}, {"text", g_custom.TXT},
        {"dim", g_custom.DIM}, {"separator", g_custom.SEP},
    };
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        char *nl = strpbrk(v, "\r\n"); if (nl) *nl = '\0';
        if (strcmp(k, "name") == 0) {
            snprintf(g_custom_name, sizeof(g_custom_name), "%s", v);
            continue;
        }
        if (strcmp(k, "font") == 0) {
            snprintf(g_custom_font, sizeof(g_custom_font), "%s", v);
            continue;
        }
        for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++) {
            if (strcmp(k, map[i].key)) continue;
            parse_hex_color(v, map[i].c);
            break;
        }
    }
    fclose(f);
    g_has_custom = 1;
}

static void config_init_defaults(void) {
    for (int i = 0; i < SC_COUNT;   i++) { app.sc_btn[i]   = sc_btn_defaults[i]; app.sc_mod[i] = sc_mod_defaults[i]; }
    for (int i = 0; i < CTRL_COUNT; i++)   app.ctrl_map[i] = ctrl_defaults[i];
    app.mic_btn      = MIC_DEFAULT_BTN;   /* L2 (analog trigger, free of DS use) */
    snprintf(app.lang_name, sizeof(app.lang_name), "english");
    app.sc_binding   = -1;
    app.ctrl_binding = -1;
    app.slot_arrow_flash = 0;
    app.slot_arrow_timer = 0;
    app.theme            = 0;
    g_theme              = &g_themes[0];

    app.crisp        = 1;
    app.ff_speed      = 0;
    app.pip_corner    = 0;
    app.pixel_perfect = 0;
    app.overlay_pack_count = 0;
    app.overlay_pack_idx   = 0;
    app.overlay_pack_name[0] = '\0';
    app.overlay_tex        = NULL;
    app.auto_resume   = 0;
    app.prepare_frames      = 0;

    app.frameskip     = 0;
    app.frameskip_val = 0;
    app.safe_skip     = 0;
    app.threaded_3d   = 0;
    app.clock_speed   = 0;
    app.fix_2d        = 0;
    app.slot2_device  = 0;
    app.rom_hack      = 0;
    app.compress_states = 0;
    app.screen_blend  = 0;
    app.edge_marking  = 0;
    app.hires_3d      = 0;
    app.fw_language   = 0;
    app.fw_bday_month = 0;
    app.fw_bday_day   = 0;
    app.fw_fav_color  = 0;
    app.fw_username[0] = '\0';
    app.rtc_mode      = 0;
    app.unzip_roms    = 0;
    app.backup_sav    = 0;
    app.rtc_year      = 0;
    app.rtc_month     = 0;
    app.rtc_day       = 0;
    app.rtc_hour      = 0;
    app.rtc_minute    = 0;

    app.overlay_scroll         = 0;
    app.av_scroll              = 0;
    app.emu_scroll             = 0;
    app.emu_bday_editing   = 0;
    app.emu_bday_cursor    = 0;
    app.cursor_nx     = 128.0f;
    app.cursor_ax = 0; app.cursor_ay = 0;
    app.cursor_stick_seen = 0;
    app.cursor_move_ms = 0;
    app.cursor_touch_down = 0;
    app.cursor_ny     = 92.0f;

}

/* Flush to disk before closing - config/state writes must survive a
   power cut the moment they return. */
static void fclose_durable(FILE *f) {
    fflush(f); fsync(fileno(f)); fclose(f);
}
static void state_path(char *out, size_t sz, int slot) {
    snprintf(out, sz, "%s/%s/%s.st%d", sdcard_path(), STATES_DIR, app.rom_name, slot);
}
static void skip_resume_path(char *out, size_t sz) {
    snprintf(out, sz, "%s/%s/.skip_resume", sdcard_path(), SAVES_DIR);
}

static void config_path(char *out, size_t sz, const char *name) {
    snprintf(out, sz, "%s/%s", app.drastic_dir, name);
}
static void sc_config_path(char *out, size_t sz)   { config_path(out, sz, "user_shortcuts.cfg"); }
static void ctrl_config_path(char *out, size_t sz) { config_path(out, sz, "user_controls.cfg"); }
static void emu_config_path(char *out, size_t sz)  { config_path(out, sz, "user_emu.cfg"); }

static void sc_config_save(void) {
    char path[MAX_PATH]; sc_config_path(path, sizeof(path));
    FILE *f = fopen(path, "w"); if (!f) return;
    fprintf(f, "v2\n");
    for (int i = 0; i < SC_COUNT; i++)
        fprintf(f, "%d %d\n", app.sc_btn[i], app.sc_mod[i]);
    fclose_durable(f);
}
/* Hat-mode migration: older configs stored the dpad as buttons 12-15;
   in hat mode the dpad is virtual buttons 28-31. Remap on load. */
static int sc_migrate_btn(int b) {
#if JOY_UP == HAT_UP_BIT
    if (b >= 12 && b <= 15) return b + (HAT_UP_BIT - 12);
#endif
    return b;
}

static void sc_config_load(void) {
    char path[MAX_PATH]; sc_config_path(path, sizeof(path));
    FILE *f = fopen(path, "r"); if (!f) return;
    /* unversioned files predate the current button numbering - discard
       them (defaults stay) rather than load bindings that can't fire */
    char tag[8] = {0};
    if (fscanf(f, "%7s\n", tag) != 1 || strcmp(tag, "v2") != 0) {
        fclose(f);
        sc_config_save();
        return;
    }
    for (int i = 0; i < SC_COUNT; i++) {
        int b = -1, m = 1;
        if (fscanf(f, "%d %d\n", &b, &m) == 2) { app.sc_btn[i] = sc_migrate_btn(b); app.sc_mod[i] = m; }
    }
    fclose(f);
}

static void ctrl_config_save(void) {
    char path[MAX_PATH]; ctrl_config_path(path, sizeof(path));
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int i = 0; i < CTRL_COUNT; i++) fprintf(f, "%d\n", app.ctrl_map[i]);
    fclose_durable(f);
}
static void ctrl_config_load(void) {
    char path[MAX_PATH]; ctrl_config_path(path, sizeof(path));
    FILE *f = fopen(path, "r"); if (!f) return;
    for (int i = 0; i < CTRL_COUNT; i++) {
        int b = ctrl_defaults[i];
        if (fscanf(f, "%d\n", &b) == 1) app.ctrl_map[i] = sc_migrate_btn(b);
    }
    fclose(f);
}

static void overlay_base_dir(char *out, int sz) {
    snprintf(out, sz, "%s/Overlays/%dx%d", app.drastic_dir, VFB_W, VFB_H);
}

static void overlay_scan(void) {
    app.overlay_pack_count = 0;
    char base[MAX_PATH];
    overlay_base_dir(base, sizeof(base));
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && app.overlay_pack_count < MAX_OVERLAY_PACKS) {
        if (e->d_name[0] == '.') continue;

        char full[MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", base, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(app.overlay_packs[app.overlay_pack_count],
                     OVERLAY_NAME_LEN, "%s", e->d_name);
            app.overlay_pack_count++;
        }
    }
    closedir(d);

    for (int i = 0; i < app.overlay_pack_count - 1; i++)
        for (int j = i + 1; j < app.overlay_pack_count; j++)
            if (strcmp(app.overlay_packs[i], app.overlay_packs[j]) > 0) {
                char tmp[OVERLAY_NAME_LEN];
                memcpy(tmp, app.overlay_packs[i], OVERLAY_NAME_LEN);
                memcpy(app.overlay_packs[i], app.overlay_packs[j], OVERLAY_NAME_LEN);
                memcpy(app.overlay_packs[j], tmp, OVERLAY_NAME_LEN);
            }
}

static void overlay_filename(char *out, int sz) {
    static const char *layout_keys[LAYOUT_COUNT] = {
        "pip", "single", "focus", "stacked", "sidebyside"
    };
    snprintf(out, sz, "%s_%s.png",
             app.pixel_perfect ? "pp" : "aspect",
             layout_keys[app.layout]);
}

static SDL_Texture *load_png_tex(SDL_Renderer *r, const char *path) {
    int w, h, n;
    unsigned char *px = stbi_load(path, &w, &h, &n, 4);
    if (!px) return NULL;
    SDL_Surface *rgba = SDL_CreateRGBSurfaceFrom(px, w, h, 32, w * 4,
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (!rgba) { stbi_image_free(px); return NULL; }
    SDL_Surface *surf = SDL_ConvertSurfaceFormat(rgba, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(rgba);
    stbi_image_free(px);
    if (!surf) return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_FreeSurface(surf);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

static SDL_Texture *overlay_load_tex(SDL_Renderer *r) {
    if (!app.overlay_pack_name[0]) return NULL;
    char base[MAX_PATH], fname[64], path[MAX_PATH];
    overlay_base_dir(base, sizeof(base));
    overlay_filename(fname, sizeof(fname));
    snprintf(path, sizeof(path), "%s/%s/%s", base, app.overlay_pack_name, fname);
    SDL_Texture *tex = load_png_tex(r, path);
    if (!tex) {

        app.overlay_pack_name[0] = '\0';
        app.overlay_pack_idx = 0;
    }
    return tex;
}

static void overlay_preview_clear(void) {
    if (app.overlay_preview_tex) {
        SDL_DestroyTexture(app.overlay_preview_tex);
        app.overlay_preview_tex = NULL;
        app.overlay_preview_idx = -1;
    }
}

static void overlay_reload(void) {
    if (app.overlay_tex) {
        SDL_DestroyTexture(app.overlay_tex);
        app.overlay_tex = NULL;
    }
    app.overlay_needs_load = app.overlay_pack_name[0] ? 1 : 0;
}

static void update_overlay(SDL_Renderer *r) {
    if (!app.overlay_needs_load) return;
    app.overlay_needs_load = 0;
    if (app.overlay_tex) { SDL_DestroyTexture(app.overlay_tex); app.overlay_tex = NULL; }
    /* PNG decode off a slow card can cross the watchdog's hang bar and
       trigger a relaunch mid-session - park it for the load */
    app.plat_busy++;
    app.overlay_tex = overlay_load_tex(r);
    app.last_render_ticks = SDL_GetTicks();
    app.plat_busy--;
}

static SDL_Texture *overlay_get_preview_tex(SDL_Renderer *r, int menu_item) {
    if (menu_item == 0) {
        if (app.overlay_preview_tex) {
            SDL_DestroyTexture(app.overlay_preview_tex);
            app.overlay_preview_tex = NULL;
            app.overlay_preview_idx = -1;
        }
        return NULL;
    }
    if (menu_item == app.overlay_preview_idx && app.overlay_preview_tex)
        return app.overlay_preview_tex;

    if (app.overlay_preview_tex) { SDL_DestroyTexture(app.overlay_preview_tex); app.overlay_preview_tex = NULL; }
    app.overlay_preview_idx = menu_item;

    int pack_i = menu_item - 1;
    if (pack_i < 0 || pack_i >= app.overlay_pack_count) return NULL;

    char base[MAX_PATH], fname[64], path[MAX_PATH];
    overlay_base_dir(base, sizeof(base));
    overlay_filename(fname, sizeof(fname));
    snprintf(path, sizeof(path), "%s/%s/%s", base, app.overlay_packs[pack_i], fname);

    app.overlay_preview_tex = load_png_tex(r, path);
    return app.overlay_preview_tex;
}

static void emu_config_save(void) {
    char path[MAX_PATH]; emu_config_path(path, sizeof(path));
    FILE *f = fopen(path, "w"); if (!f) return;
    fprintf(f, "theme %d\n",          app.theme);
    fprintf(f, "layout %d\n",         app.layout);
    fprintf(f, "overlay_pack %s\n",   app.overlay_pack_name[0] ? app.overlay_pack_name : "none");
    fprintf(f, "auto_resume %d\n",    app.auto_resume);
    fprintf(f, "pixel_perfect %d\n",  app.pixel_perfect);
    fprintf(f, "swap_screens %d\n",   app.swap_screens);
    fprintf(f, "pip_corner %d\n",     app.pip_corner);
    fprintf(f, "cursor_mode %d\n",    app.cursor_mode);
    fprintf(f, "cursor_speed %d\n",   app.cursor_speed);
    fprintf(f, "crisp %d\n",          app.crisp);
    fprintf(f, "save_slot %d\n",      app.state_slot);
    fprintf(f, "load_slot %d\n",      app.load_slot);
    fprintf(f, "mic_btn %d\n",        app.mic_btn);
    fprintf(f, "lang %s\n",           app.lang_name);
    fclose_durable(f);
}

/* rtc fields must stay renderable: month indexes month_names[m-1], so an
   unset 0 would read out of bounds and print neighboring string data */
static void rtc_clamp(void) {
    if (app.rtc_year   < 2000 || app.rtc_year  > 2099) app.rtc_year   = 2026;
    if (app.rtc_month  < 1    || app.rtc_month > 12)   app.rtc_month  = 1;
    if (app.rtc_day    < 1    || app.rtc_day   > 31)   app.rtc_day    = 1;
    if (app.rtc_hour   < 0    || app.rtc_hour  > 23)   app.rtc_hour   = 0;
    if (app.rtc_minute < 0    || app.rtc_minute > 59)  app.rtc_minute = 0;
}

static void drastic_cfg_load(void) {
    if (!app.drastic_dir[0]) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/config/drastic.cfg", app.drastic_dir);
    FILE *f = fopen(path, "r"); if (!f) return;
    char key[64], strval[64];
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63s = %63s", key, strval) != 2) continue;
        int val = atoi(strval);
        if (!strcmp(key, "threaded_3d"))          app.threaded_3d   = val;
        if (!strcmp(key, "clock_speed"))           app.clock_speed   = val;
        if (!strcmp(key, "hires_3d"))              app.hires_3d      = val;
        if (!strcmp(key, "interframe_blend"))      app.screen_blend  = val;
        if (!strcmp(key, "disable_edge_marking"))  app.edge_marking  = !val;
        if (!strcmp(key, "compress_savestates"))   app.compress_states = val;
        if (!strcmp(key, "ignore_gamecard_limit")) app.rom_hack      = val;
        if (!strcmp(key, "fix_main_2d_screen"))    app.fix_2d        = val;
        if (!strcmp(key, "slot2_device_type"))     app.slot2_device  = val;
        if (!strcmp(key, "safe_frameskip"))        app.safe_skip     = val;
        if (!strcmp(key, "frameskip_type"))        app.frameskip     = val;
        if (!strcmp(key, "frameskip_value"))       app.frameskip_val = val;
        if (!strcmp(key, "rtc_system_time"))       { app.rtc_mode = val ? 1 : 0; }
        if (!strcmp(key, "use_rtc_custom_time"))   { if (val) app.rtc_mode = 2; }
        if (!strcmp(key, "rtc_custom_time")) {
            long ts = atol(strval);
            if (ts > 0) {
                time_t tt = (time_t)ts;
                struct tm tmv;
                if (localtime_r(&tt, &tmv)) {
                    app.rtc_year   = tmv.tm_year + 1900;
                    app.rtc_month  = tmv.tm_mon + 1;
                    app.rtc_day    = tmv.tm_mday;
                    app.rtc_hour   = tmv.tm_hour;
                    app.rtc_minute = tmv.tm_min;
                }
            }
        }
        if (!strcmp(key, "fast_forward"))          app.ff_speed      = val;
        if (!strcmp(key, "unzip_roms"))            app.unzip_roms    = val;
        if (!strcmp(key, "backup_use_sav_format")) app.backup_sav    = val;
        if (!strcmp(key, "firmware.language"))     app.fw_language   = val;
        if (!strcmp(key, "firmware.favorite_color")) app.fw_fav_color = val;
        if (!strcmp(key, "firmware.birthday_month")) app.fw_bday_month = val;
        if (!strcmp(key, "firmware.birthday_day"))   app.fw_bday_day   = val;
        if (!strcmp(key, "firmware.username")) {
            strncpy(app.fw_username, strval, sizeof(app.fw_username) - 1);
            app.fw_username[sizeof(app.fw_username) - 1] = '\0';
        }
    }
    fclose(f);
    rtc_clamp();
}

static void emu_config_load(void) {
    app.cursor_speed = CURSOR_SPEED_DEFAULT;   /* default when key/file absent */
    char path[MAX_PATH]; emu_config_path(path, sizeof(path));
    FILE *f = fopen(path, "r"); if (!f) return;
    char key[32], strval[32]; int val;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%31s %31s", key, strval) != 2) continue;
        val = atoi(strval);
        if (!strcmp(key, "theme"))         app.theme         = val;
        if (!strcmp(key, "layout"))        app.layout        = val;
        if (!strcmp(key, "overlay_pack")) {
            if (strcmp(strval, "none") == 0)
                app.overlay_pack_name[0] = '\0';
            else
                snprintf(app.overlay_pack_name, OVERLAY_NAME_LEN, "%s", strval);
        }
        if (!strcmp(key, "auto_resume"))   app.auto_resume   = val;
        if (!strcmp(key, "pixel_perfect")) app.pixel_perfect = val;
        if (!strcmp(key, "swap_screens"))  app.swap_screens  = val;
        if (!strcmp(key, "pip_corner"))    app.pip_corner    = (val >= 0 && val < 4) ? val : 0;
        if (!strcmp(key, "cursor_mode"))   app.cursor_mode   = val ? 1 : 0;
        if (!strcmp(key, "cursor_speed"))  app.cursor_speed  = val;
        if (!strcmp(key, "crisp"))         app.crisp         = val;
        if (!strcmp(key, "save_slot"))     app.state_slot    = (val >= 0 && val < MAX_SLOTS) ? val : 0;
        if (!strcmp(key, "load_slot"))     app.load_slot     = (val >= 0 && val < MAX_SLOTS) ? val : 0;
        if (!strcmp(key, "mic_btn"))       app.mic_btn       = (val >= -1 && val < 32) ? val : -1;
        if (!strcmp(key, "lang"))          snprintf(app.lang_name, sizeof(app.lang_name), "%s", strval);
    }
    fclose(f);
    if (app.layout < 0 || app.layout >= LAYOUT_COUNT) app.layout = 0;
    if (app.cursor_speed < 0 || app.cursor_speed >= CURSOR_SPEED_COUNT)
        app.cursor_speed = CURSOR_SPEED_DEFAULT;
    /* Allow the one custom-theme index through: the custom theme loads
       after this, so the final range check happens there. */
    if (app.theme  < 0 || app.theme  > THEME_COUNT)  app.theme  = 0;
    theme_apply(app.theme);
}

static void drastic_cfg_set(const char *cfg_path, const char *key, const char *val) {

    FILE *f = fopen(cfg_path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 256); if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, sz, f); buf[rd] = '\0'; fclose(f);

    char newline[256];
    snprintf(newline, sizeof(newline), "%s = %s", key, val);

    char *pos = buf;
    char *out = malloc(sz + 256); if (!out) { free(buf); return; }
    char *op = out;
    while (*pos) {
        char *eol = strchr(pos, '\n');
        if (!eol) eol = pos + strlen(pos);
        size_t llen = eol - pos;

        size_t klen = strlen(key);
        int match = (llen > klen && strncmp(pos, key, klen) == 0 &&
                     (pos[klen] == ' ' || pos[klen] == '='));
        if (match) {
            size_t nl = strlen(newline);
            memcpy(op, newline, nl); op += nl;
        } else {
            memcpy(op, pos, llen); op += llen;
        }
        if (*eol == '\n') { *op++ = '\n'; pos = eol + 1; }
        else              { pos = eol; }
    }
    *op = '\0';

    f = fopen(cfg_path, "w");
    if (f) { fputs(out, f); fclose_durable(f); }
    free(buf); free(out);
}

static void drastic_cfg_save_all(void) {
    if (!app.drastic_dir[0]) return;
    char cfg[MAX_PATH];
    snprintf(cfg, sizeof(cfg), "%s/config/drastic.cfg", app.drastic_dir);

    char tmp[64];

    snprintf(tmp, sizeof(tmp), "%d", app.threaded_3d);       drastic_cfg_set(cfg, "threaded_3d",          tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.clock_speed);       drastic_cfg_set(cfg, "clock_speed",          tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.hires_3d);          drastic_cfg_set(cfg, "hires_3d",             tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.screen_blend);      drastic_cfg_set(cfg, "interframe_blend",     tmp);
    snprintf(tmp, sizeof(tmp), "%d", !app.edge_marking);     drastic_cfg_set(cfg, "disable_edge_marking", tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.compress_states);   drastic_cfg_set(cfg, "compress_savestates",  tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.rom_hack);          drastic_cfg_set(cfg, "ignore_gamecard_limit",tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.fw_language);       drastic_cfg_set(cfg, "firmware.language",    tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.fw_bday_month);     drastic_cfg_set(cfg, "firmware.birthday_month", tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.fw_bday_day);       drastic_cfg_set(cfg, "firmware.birthday_day",   tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.fw_fav_color);      drastic_cfg_set(cfg, "firmware.favorite_color", tmp);
    drastic_cfg_set(cfg, "firmware.username", app.fw_username);

    snprintf(tmp, sizeof(tmp), "%d", app.fix_2d);            drastic_cfg_set(cfg, "fix_main_2d_screen",      tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.slot2_device);      drastic_cfg_set(cfg, "slot2_device_type",       tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.safe_skip);         drastic_cfg_set(cfg, "safe_frameskip",          tmp);

    {
        int fs_type  = (app.frameskip == 0) ? 0 : (app.frameskip == 1) ? 1 : 2;
        int fs_value = (app.frameskip >= 2) ? app.frameskip_val : 1;
        snprintf(tmp, sizeof(tmp), "%d", fs_type);  drastic_cfg_set(cfg, "frameskip_type",  tmp);
        snprintf(tmp, sizeof(tmp), "%d", fs_value); drastic_cfg_set(cfg, "frameskip_value", tmp);
    }

    drastic_cfg_set(cfg, "rtc_system_time",     app.rtc_mode == 1 ? "1" : "0");
    drastic_cfg_set(cfg, "use_rtc_custom_time", app.rtc_mode == 2 ? "1" : "0");
    if (app.rtc_mode == 2) {
        struct tm t = {0};
        t.tm_year = app.rtc_year - 1900;
        t.tm_mon  = app.rtc_month - 1;
        t.tm_mday = app.rtc_day;
        t.tm_hour = app.rtc_hour;
        t.tm_min  = app.rtc_minute;
        t.tm_isdst = -1;
        time_t ts = mktime(&t);
        snprintf(tmp, sizeof(tmp), "%ld", (long)ts);
        drastic_cfg_set(cfg, "rtc_custom_time", tmp);
    }

    snprintf(tmp, sizeof(tmp), "%d", app.ff_speed ? 1 : 0);
    drastic_cfg_set(cfg, "fast_forward", tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.unzip_roms);
    drastic_cfg_set(cfg, "unzip_roms", tmp);
    snprintf(tmp, sizeof(tmp), "%d", app.backup_sav);
    drastic_cfg_set(cfg, "backup_use_sav_format", tmp);
}

static int remap_button(int physical) {
    for (int i = 0; i < CTRL_COUNT; i++)
        if (app.ctrl_map[i] == physical) return ctrl_canonical[i];
    return physical;
}

static void vfb_clear(void) {
    if (!app.renderer || !app.virtual_fb) return;
    real_SDL_SetRenderTarget(app.renderer, app.virtual_fb);
    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
    real_SDL_RenderClear(app.renderer);
}

/* present one black frame to the screen (quit blanking, pre-load holds) */
static void present_black(SDL_Renderer *ren) {
    real_SDL_SetRenderTarget(ren, NULL);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    real_SDL_RenderClear(ren);
    real_SDL_RenderPresent(ren);
}

#ifdef DRASTIC_ARM32
/* non-PIE: OFF_VAR_SYSTEM is the struct's absolute address */
static void *drastic_system(void) { return (void *)(uintptr_t)OFF_VAR_SYSTEM; }
#else
/* PIE: OFF_VAR_SYSTEM holds a pointer to the struct */
static void *drastic_system(void) { return DRASTIC_VAR(OFF_VAR_SYSTEM); }
#endif

/* Touch write into the emulator input_struct. Called from both the
 * PollEvent and present hooks so no internal stage can overwrite it.
 * One trailing clear when the pen lifts. */
static void nds_touch_enforce(void) {
    static int touch_written = 0;
    if (!app.cursor_touch_down && !touch_written) return;
    void *sys = drastic_system();
    if (!sys) return;
    uint8_t *in = (uint8_t *)sys + NDS_TOUCH_INPUT_OFFSET;
    if (app.cursor_touch_down) {
        *(uint32_t *)(in + NDS_IN_TOUCH_X) = (uint32_t)app.cursor_nx;
        *(uint32_t *)(in + NDS_IN_TOUCH_Y) = (uint32_t)app.cursor_ny;
        *(uint8_t  *)(in + NDS_IN_TOUCH_ST) = 1;
        *(uint8_t  *)(in + NDS_IN_TOUCH_PR) = 1;
        touch_written = 1;
    } else {
        *(uint8_t  *)(in + NDS_IN_TOUCH_ST) = 0;
        *(uint8_t  *)(in + NDS_IN_TOUCH_PR) = 0;
        touch_written = 0;
    }
}

static void drastic_await_save(void) {
    if (!OFF_IS_SAVING) return;   /* ARM32: no busy flag - file wait covers it */
    if (!app.base) return;
    volatile uint32_t *busy = (volatile uint32_t *)(app.base + OFF_IS_SAVING);
    int timeout = 3000;
    while (*busy && timeout-- > 0) real_SDL_Delay(1);
}

#ifdef DRASTIC_ARM32
/* ARM32 has no is-saving flag, so wait for the .st<slot> file to appear and
   stop growing (drastic writes a temp then renames) before tearing down. */
static void nds_await_state_written(int slot) {
    char path[MAX_PATH]; struct stat st;
    state_path(path, sizeof(path), slot);
    off_t last = -1; int stable = 0;
    for (int i = 0; i < 500; i++) {
        if (stat(path, &st) == 0 && st.st_size > 0) {
            if (st.st_size == last) { if (++stable >= 4) return; }
            else { last = st.st_size; stable = 0; }
        }
        real_SDL_Delay(10);
    }
}
#endif

static void drastic_save_state(int slot) {
    app.plat_busy++;
    drastic_await_save();
    void *sys = drastic_system();
    if (!sys) { app.plat_busy--; return; }
    char dir[MAX_PATH], name[MAX_FILE];
    snprintf(dir,  sizeof(dir),  "%s/%s/", sdcard_path(), STATES_DIR);
    snprintf(name, sizeof(name), "%s.st%d", app.rom_name, slot);
    app.last_render_ticks = SDL_GetTicks();
    ((drastic_save_state_t)DRASTIC_FN(OFF_SAVE_STATE))(sys, dir, name, NULL, NULL);
    app.last_render_ticks = SDL_GetTicks();
    app.plat_busy--;
}


/* State-load FIFO guard + frame-sync offsets (disassembly-verified).
   A state load while the 3D engine is cold can leave the geometry FIFO
   quartet poisoned; validate it and, when garbage, apply the emulator's
   own empty-FIFO reset. Runs post-load plus a ~600-frame poll-watch. */
#define NDS_SYNC_BASELINE_OFF 0x3b2a978u  /* sys+: uint64, us*3 */
#define NDS_SYNC_TARGET_OFF   0x3b2a980u  /* sys+: uint64, us*3 */
#ifndef DRASTIC_ARM32
static volatile int g_nds_fifo_watch = 0;
static int nds_fifo_sanitize(const char *where) {
    uint8_t *s2 = (uint8_t *)drastic_system();
    if (!s2) return 0;
    uintptr_t lo     = (uintptr_t)s2;
    uintptr_t hi     = lo + 0x4000000;
    uintptr_t base_l = lo + 0x79b00;   /* geometry list buffer */
    uintptr_t base_a = lo + 0x81b00;   /* geometry aux buffer  */
    volatile uintptr_t *q = (volatile uintptr_t *)(s2 + 0x8000 + 6760);
    /* all-zero = drastic's own harmless post-load state - leave it so the
       self-heal path runs; anything else out of range = poison */
    if ((q[0] | q[1] | q[2] | q[3]) == 0) return 0;
    if (q[0] < lo || q[0] >= hi || q[1] < lo || q[1] >= hi ||
        q[2] < lo || q[2] >= hi || q[3] < lo || q[3] >= hi) {
        fprintf(stderr, "[fun] fifo-sanitize(%s): garbage - reset to empty\n", where);
        q[0] = base_l; q[1] = base_a; q[2] = base_l; q[3] = base_a;
        return 1;
    }
    return 0;
}
#endif

static void drastic_load_state(int slot) {
    app.prepare_frames = 0;
    /* big states + the save-compressor await can cross the watchdog's
       hang bar - park it for the whole operation */
    app.plat_busy++;
    drastic_await_save();
    void *sys = drastic_system();
    if (!sys) { app.plat_busy--; return; }
    char path[MAX_PATH];
    state_path(path, sizeof(path), slot);
    if (access(path, F_OK) != 0)
        fprintf(stderr, "[fun] load_state: slot %d file missing (%s)\n",
                slot, path);
    app.last_render_ticks = SDL_GetTicks();
    ((drastic_load_state_t)DRASTIC_FN(OFF_LOAD_STATE))(sys, path, NULL, NULL, 0);
    app.last_render_ticks = SDL_GetTicks();
    app.plat_busy--;
#ifndef DRASTIC_ARM32
    nds_fifo_sanitize("post-load");
    g_nds_fifo_watch = 600;   /* keep watching ~10s of frames */
#endif
}

void ff_apply(void) {

    if (app.drastic_dir[0]) {
        char cfg[MAX_PATH], tmp[4];
        snprintf(cfg, sizeof(cfg), "%s/config/drastic.cfg", app.drastic_dir);
        snprintf(tmp, sizeof(tmp), "%d", app.ff_speed ? 1 : 0);
        drastic_cfg_set(cfg, "fast_forward", tmp);
    }
}

/* The hook drives the mic directly (mic_inject), so mic_apply UNBINDS
   DraStic's own mic control (65535). That stops its per-scanline update_input
   from clearing the active flag out from under the hook. The launcher
   refreshes drastic.cfg each boot, so this is re-applied at game launch. */
static void mic_apply(void) {
    if (!app.drastic_dir[0]) return;
    char cfg[MAX_PATH];
    snprintf(cfg, sizeof(cfg), "%s/config/drastic.cfg", app.drastic_dir);
    drastic_cfg_set(cfg, "controls_b[CONTROL_INDEX_FAKE_MICROPHONE]", "65535");
    drastic_cfg_set(cfg, "controls_a[CONTROL_INDEX_FAKE_MICROPHONE]", "65535");
}

/* Direct fake-mic drive. Calls DraStic's own spu_fake_microphone_start/stop
   with the base its update_input uses: nds + 0x1587000, where nds sits just
   past the input struct the touch code writes (proven base). Sanity-gated -
   a wrong base/WAV aborts instead of corrupting memory. */
static void mic_inject(int on) {
    static int cur = -1;
    static int warned = 0;
    if (!app.base) return;
    void *sys = drastic_system();
    if (!sys) return;
    void *nds = *(void **)((uint8_t *)sys + NDS_PTR_OFF);
    if (!nds || ((uintptr_t)nds & 7)) return;               /* null/misaligned */
    uint8_t *spu = (uint8_t *)nds + NDS_SPU_STRUCT_OFF;
    uint8_t  flag = spu[SPU_MIC_ACTIVE];
    if (flag > 1) {                       /* flag is a bool - garbage = wrong base */
        if (!warned) { fprintf(stderr, "[fun] mic: base sanity fail (flag=%u) - direct drive off\n", flag); warned = 1; }
        return;
    }
    if (!*(void **)(spu + SPU_MIC_WAV)) return;   /* WAV not loaded yet - wait quietly */
    void *arg = (uint8_t *)nds + NDS_SPU_ARG_OFF;
    if (on) {
        if (cur != 1) ((void (*)(void *))(app.base + OFF_SPU_FAKE_MIC_START))(arg);
        else if (flag == 0) spu[SPU_MIC_ACTIVE] = 1;   /* re-assert if DraStic cleared it */
    } else if (cur != 0) {
        ((void (*)(void *))(app.base + OFF_SPU_FAKE_MIC_STOP))(arg);
    }
    cur = on;
}

/* L2/R2 analog-trigger held state, tracked from axis events in the game
   stream (they may arrive as a button or an axis on this hardware). */
static int g_l2_axis_held = 0, g_r2_axis_held = 0;
static int mic_trigger_held(void) {
    int b = app.mic_btn;
    if (b < 0) return 0;
#if JOY_L2 >= 0
    if (b == JOY_L2 || b == TRIG_L2_BIT)
        return g_l2_axis_held || (app.joy_held & (1u << JOY_L2)) != 0;
#endif
#if JOY_R2 >= 0
    if (b == JOY_R2 || b == TRIG_R2_BIT)
        return g_r2_axis_held || (app.joy_held & (1u << JOY_R2)) != 0;
#endif
    return (app.joy_held & (1u << b)) != 0;
}

static void ff_reset_apply(void) {
    if (app.ff_restore_pending) return;
    /* ARM32: no in-place reset - apply the fast-forward cfg only. */
    if (!OFF_LOAD_NDS || !OFF_RESET_SYSTEM || !OFF_MAIN_LONGJMP) { ff_apply(); return; }
    void *sys = drastic_system();
    if (!sys) return;

    /* The reset wipes the running game, so fire it only once the scratch
       state is on disk (async save: await + stat); on failure the cfg
       still flips and the speed applies at the next natural reset. Parks
       the watchdog - the compressor await stalls presents and reads as a hang. */
    app.plat_busy++;
    time_t t0 = time(NULL);
    drastic_save_state(FF_TEMP_SLOT);
    drastic_await_save();
    app.last_render_ticks = SDL_GetTicks();
    {
        char vpath[MAX_PATH];
        struct stat vst;
        state_path(vpath, sizeof(vpath), FF_TEMP_SLOT);
        if (stat(vpath, &vst) != 0 || vst.st_size < 1024 ||
            vst.st_mtime < t0 - 5) {
            fprintf(stderr, "[fun] ff: scratch state missing/stale - "
                    "cfg-only apply (no reset)\n");
            ff_apply();
            app.plat_busy--;
            return;
        }
    }
    app.ff_restore_pending = 1;
    ff_apply();
    ((drastic_load_nds_t)DRASTIC_FN(OFF_LOAD_NDS))((uint8_t *)sys + 800, app.rom_path);
    ((drastic_reset_system_t)DRASTIC_FN(OFF_RESET_SYSTEM))(sys);
    app.prepare_frames = 5;
    app.cursor_nx = 128.0f; app.cursor_ny = 92.0f;
    app.last_render_ticks = SDL_GetTicks();
    app.plat_busy--;
    longjmp(*(jmp_buf *)((uint8_t *)sys + OFF_MAIN_LONGJMP), MN_SML);
}

static void drastic_reset(void) {
    app.ff_speed = 0;
    ff_apply();
    void *sys = drastic_system();
    if (!sys) return;
    if (!OFF_LOAD_NDS || !OFF_RESET_SYSTEM || !OFF_MAIN_LONGJMP) return;  /* ARM32: no in-place reset */
    ((drastic_load_nds_t)DRASTIC_FN(OFF_LOAD_NDS))((uint8_t *)sys + 800, app.rom_path);
    ((drastic_reset_system_t)DRASTIC_FN(OFF_RESET_SYSTEM))(sys);
    app.cursor_nx = 128.0f; app.cursor_ny = 92.0f;
    app.last_render_ticks = SDL_GetTicks();
    longjmp(*(jmp_buf *)((uint8_t *)sys + OFF_MAIN_LONGJMP), MN_SML);
}

static void drastic_quit(void) {
    drastic_await_save();
    real_SDL_Delay(100);

    void *sys = drastic_system();
    if (!sys) { real_exit(0); return; }
    ((drastic_quit_t)DRASTIC_FN(OFF_QUIT))(sys);
    real_exit(0);
}

#define NDS_W   256
#define NDS_H   192

#define PP_PIP_W   (NDS_W * PP_SCALE_PIP)    // 256
#define PP_PIP_H   (NDS_H * PP_SCALE_PIP)    // 192

/* Aspect-mode PIP: 30% of canvas width. PP mode stays integer. */
#define PIP_W   (VFB_W * 3 / 10)
#define PIP_H   (PIP_W * 3 / 4)
#define PIP_PAD  10

static screen_slot_t layout_slots[LAYOUT_COUNT][2];

#define PP_GAP  92

static screen_slot_t layout_slots_pp[LAYOUT_COUNT][2];

static void compute_layouts(void) {
    float sx = (float)VFB_W / 640.0f;
    float sy = (float)VFB_H / 480.0f;
#define SC(v,s) ((int)((v)*(s)+0.5f))

    if (VFB_H >= 600) { MN_SML = 2; MN_NRM = 3; MN_BIG = 4; }
    else              { MN_SML = 1; MN_NRM = 2; MN_BIG = 3; }

    g_content_y   = SC(52,  sy);
    g_footer_h    = SC(52,  sy);
    g_footer_y    = VFB_H - g_footer_h;
    g_menu_rh     = SC(62,  sy);
    g_menu_vis    = (g_footer_y - g_content_y) / g_menu_rh;
    if (g_menu_vis < 3) g_menu_vis = 3;

    g_prev_w      = SC(296, sx);
    g_prev_h      = g_prev_w * VFB_H / VFB_W;
    g_prev_x      = VFB_W - g_prev_w - SC(16, sx);
    g_prev_y      = g_content_y + SC(8, sy);

    #define FIT43W(bw,bh) (((bw)*3 <= (bh)*4) ? (bw) : ((bh)*4/3))

    {
        int mw = (VFB_W*3 <= VFB_H*4) ? VFB_W : VFB_H*4/3;
        int mh = mw*3/4;
        int mx = (VFB_W-mw)/2, my = (VFB_H-mh)/2;
        layout_slots[0][0] = (screen_slot_t){{ mx, my, mw, mh }, 255};
        layout_slots[0][1] = (screen_slot_t){{ VFB_W-PIP_W-PIP_PAD, PIP_PAD, PIP_W, PIP_H }, 160};
    }

    {
        int sw = FIT43W(VFB_W, VFB_H);
        int sh = sw * 3 / 4;
        int sx2 = (VFB_W - sw) / 2;
        int sy2 = (VFB_H - sh) / 2;
        layout_slots[1][0] = (screen_slot_t){{ sx2, sy2, sw, sh }, 255};
        layout_slots[1][1] = (screen_slot_t){{ 0, 0, 0, 0 }, 0};
    }

    {

        int mw = VFB_W;
        int mh = mw * 3 / 4;
        if (mh > VFB_H * 3 / 4) { mh = VFB_H * 3 / 4; mw = mh * 4 / 3; }
        int mx = (VFB_W - mw) / 2;

        int rem_h = VFB_H - mh;
        int tw = FIT43W(VFB_W, rem_h);
        int th = tw * 3 / 4;
        int tx = (VFB_W - tw) / 2;
        int ty = mh + (rem_h - th) / 2;
        layout_slots[2][0] = (screen_slot_t){{ mx, 0, mw, mh }, 255};
        layout_slots[2][1] = (screen_slot_t){{ tx, ty, tw, th }, 255};
    }

    {

        int sh = VFB_H / 2;
        int sw = FIT43W(VFB_W, sh);

        if (sw < VFB_W * 3 / 4) { sw = VFB_W; sh = sw * 3 / 4; }

        sw = FIT43W(VFB_W, VFB_H / 2);
        sh = sw * 3 / 4;
        int sx2 = (VFB_W - sw) / 2;
        int total_h = sh * 2;
        int top_y = (VFB_H - total_h) / 2;
        layout_slots[3][0] = (screen_slot_t){{ sx2, top_y,      sw, sh }, 255};
        layout_slots[3][1] = (screen_slot_t){{ sx2, top_y + sh, sw, sh }, 255};
    }

    {

        int sw = VFB_W / 2;
        int sh = sw * 3 / 4;
        if (sh > VFB_H) { sh = VFB_H; sw = sh * 4 / 3; }
        int total_w = sw * 2;
        int left_x  = (VFB_W - total_w) / 2;
        int cy2     = (VFB_H - sh) / 2;
        layout_slots[4][0] = (screen_slot_t){{ left_x + sw, cy2, sw, sh }, 255};
        layout_slots[4][1] = (screen_slot_t){{ left_x,      cy2, sw, sh }, 255};
    }

    #undef FIT43W
#undef SC

    g_pp_scale_main = 1;
    for (int s = 4; s >= 1; s--) {
        if (NDS_W * s <= VFB_W && NDS_H * s <= VFB_H) { g_pp_scale_main = s; break; }
    }

    g_pp_scale_focus = 1;
    for (int s = 4; s >= 1; s--) {
        if (NDS_W * s <= VFB_W && NDS_H * s < VFB_H) { g_pp_scale_focus = s; break; }
    }
    g_pp_scale_pip = 1;

    int pmw = NDS_W * g_pp_scale_main,  pmh = NDS_H * g_pp_scale_main;
    int ppw = NDS_W * g_pp_scale_pip,   pph = NDS_H * g_pp_scale_pip;
    int pfw = NDS_W * g_pp_scale_focus, pfh = NDS_H * g_pp_scale_focus;
    int pftw = (VFB_H - pfh) * NDS_W / NDS_H;
    int pfth = VFB_H - pfh;

    layout_slots_pp[0][0] = (screen_slot_t){{ 0, 0, VFB_W, VFB_H }, 255};
    layout_slots_pp[0][1] = (screen_slot_t){{ 0, 0, ppw, pph }, 160};

    layout_slots_pp[1][0] = (screen_slot_t){{ (VFB_W-pmw)/2, (VFB_H-pmh)/2, pmw, pmh }, 255};
    layout_slots_pp[1][1] = (screen_slot_t){{ 0, 0, 0, 0 }, 0};

    layout_slots_pp[2][0] = (screen_slot_t){{ (VFB_W-pfw)/2, 0, pfw, pfh }, 255};
    layout_slots_pp[2][1] = (screen_slot_t){{ (VFB_W-pftw)/2, pfh, pftw, pfth }, 255};

    layout_slots_pp[3][0] = (screen_slot_t){{ (VFB_W-NDS_W)/2, (VFB_H-(NDS_H*2+PP_GAP))/2,               NDS_W, NDS_H }, 255};
    layout_slots_pp[3][1] = (screen_slot_t){{ (VFB_W-NDS_W)/2, (VFB_H-(NDS_H*2+PP_GAP))/2+NDS_H+PP_GAP,  NDS_W, NDS_H }, 255};

    layout_slots_pp[4][0] = (screen_slot_t){{ (VFB_W-(NDS_W*2+PP_GAP))/2+NDS_W+PP_GAP, (VFB_H-NDS_H)/2, NDS_W, NDS_H }, 255};
    layout_slots_pp[4][1] = (screen_slot_t){{ (VFB_W-(NDS_W*2+PP_GAP))/2,               (VFB_H-NDS_H)/2, NDS_W, NDS_H }, 255};
}

static const char *layout_names[LAYOUT_COUNT] = {
    "PIP CORNER", "SINGLE", "FOCUS", "STACKED", "SIDE BY SIDE",
};

static void get_pip_slots(int swap, int corner, int pp, screen_slot_t out[2]) {

    int mw, mh;
    if (pp) {

        mw = NDS_W * g_pp_scale_main;
        mh = NDS_H * g_pp_scale_main;
    } else {

        mw = (VFB_W * 3 <= VFB_H * 4) ? VFB_W : VFB_H * 4 / 3;
        mh = mw * 3 / 4;
    }
    int mx = (VFB_W - mw) / 2;
    int my = (VFB_H - mh) / 2;

    int pw = pp ? PP_PIP_W : PIP_W;
    int ph = pp ? PP_PIP_H : PIP_H;
    if (pw > mw / 3) { pw = mw / 3; ph = pw * 3 / 4; }
    if (pw < 64)     { pw = 64;     ph = 48; }

    int px, py;
    switch (corner) {
        default:
        case 0: px = mx + mw - pw - PIP_PAD; py = my + PIP_PAD;           break;
        case 1: px = mx + mw - pw - PIP_PAD; py = my + mh - ph - PIP_PAD; break;
        case 2: px = mx + PIP_PAD;            py = my + mh - ph - PIP_PAD; break;
        case 3: px = mx + PIP_PAD;            py = my + PIP_PAD;           break;
    }
    if (!swap) {
        out[0] = (screen_slot_t){{ mx, my, mw, mh }, 255};
        out[1] = (screen_slot_t){{ px, py, pw, ph }, 160};
    } else {
        out[0] = (screen_slot_t){{ px, py, pw, ph }, 160};
        out[1] = (screen_slot_t){{ mx, my, mw, mh }, 255};
    }
}

static SDL_Rect cursor_touch_rect(void) {
    screen_slot_t pip[2];
    if (app.layout == 0) {
        get_pip_slots(app.swap_screens, app.pip_corner, app.pixel_perfect, pip);
        return pip[1].rect;
    }
    int ts = app.swap_screens ? 0 : 1;
    const screen_slot_t (*slots)[2] = app.pixel_perfect ? layout_slots_pp : layout_slots;
    return slots[app.layout][ts].rect;
}

static void cursor_tex_load(SDL_Renderer *r) {
    if (app.cursor_tex) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/res/cursor/1.png", app.drastic_dir);
    app.cursor_tex = load_png_tex(r, path);
}



static void rom_name_from_path(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(app.rom_name, MAX_FILE, "%s", base);
    char *dot = strrchr(app.rom_name, '.');
    if (dot) *dot = '\0';
    if (strlen(app.rom_name) > 28) app.rom_name[28] = '\0';

    char *src = app.rom_name;
    char *dst = app.rom_display;
    int depth_paren = 0, depth_bracket = 0;
    while (*src) {
        if      (*src == '(') { depth_paren++;   src++; }
        else if (*src == ')') { depth_paren--;   src++; }
        else if (*src == '[') { depth_bracket++; src++; }
        else if (*src == ']') { depth_bracket--; src++; }
        else if (depth_paren == 0 && depth_bracket == 0)
            *dst++ = *src++;
        else
            src++;
    }
    *dst = '\0';

    while (dst > app.rom_display && (*(dst-1) == ' ' || *(dst-1) == '_'))
        *--dst = '\0';
}

/* Translations. A language file is KEY=VALUE lines, one per string, where
   the key is the English text. tr() returns the loaded value or the key
   itself, so English needs no file and any untranslated string falls back
   to English. "english" is built in and always first. */
#define LANG_MAX     640
#define LANG_BUF     32768
#define LANG_NAME_LEN 24
static struct {
    char  *key[LANG_MAX];
    char  *val[LANG_MAX];
    int    count;
    char   buf[LANG_BUF];
    int    buf_len;
    char   names[16][LANG_NAME_LEN];   /* selectable languages, [0]=english */
    int    n_names;
    int    sel;
} g_lang;

static const char *tr(const char *s) {
    if (!s || g_lang.sel <= 0) return s;
    for (int i = 0; i < g_lang.count; i++)
        if (strcmp(g_lang.key[i], s) == 0) return g_lang.val[i];
    return s;
}

static void lang_clear(void) { g_lang.count = 0; g_lang.buf_len = 0; }

static void lang_load(const char *name) {
    lang_clear();
    if (!name || !name[0] || strcmp(name, "english") == 0) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/language/%s.txt", app.drastic_dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f) && g_lang.count < LANG_MAX) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        char *nl = strpbrk(v, "\r\n"); if (nl) *nl = '\0';
        if (!*k || !*v) continue;
        int kl = (int)strlen(k) + 1, vl = (int)strlen(v) + 1;
        if (g_lang.buf_len + kl + vl > LANG_BUF) break;
        g_lang.key[g_lang.count] = g_lang.buf + g_lang.buf_len;
        memcpy(g_lang.buf + g_lang.buf_len, k, kl); g_lang.buf_len += kl;
        g_lang.val[g_lang.count] = g_lang.buf + g_lang.buf_len;
        memcpy(g_lang.buf + g_lang.buf_len, v, vl); g_lang.buf_len += vl;
        g_lang.count++;
    }
    fclose(f);
}

static void font_select(void);   /* defined with the font module below */

static void lang_set_current(void) {
    snprintf(app.lang_name, sizeof(app.lang_name), "%s",
             g_lang.names[g_lang.sel]);
    lang_load(app.lang_name);
    font_select();
    emu_config_save();
}

static void lang_scan(void) {
    g_lang.n_names = 0;
    snprintf(g_lang.names[g_lang.n_names++], LANG_NAME_LEN, "english");
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s/language", app.drastic_dir);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && g_lang.n_names < 16) {
            const char *dot = strrchr(e->d_name, '.');
            if (!dot || strcmp(dot, ".txt") != 0) continue;
            int n = (int)(dot - e->d_name);
            if (n <= 0 || n >= LANG_NAME_LEN) continue;
            if (strncmp(e->d_name, "template", 8) == 0) continue;
            if (strncmp(e->d_name, "english", 7) == 0) continue;
            snprintf(g_lang.names[g_lang.n_names], LANG_NAME_LEN, "%.*s", n, e->d_name);
            g_lang.n_names++;
        }
        closedir(d);
    }
}

/* TrueType text. The 5x7 bitmap stays the default; a TTF loads only when
   the UI needs glyphs it lacks (a translation, or a custom theme font).
   Glyphs rasterise on demand into cached textures. */
typedef struct {
    unsigned char *data;
    stbtt_fontinfo info;
    int loaded;
} TTFont;
static TTFont g_ttf_tr;      /* translation font (non-English) */
static TTFont g_ttf_custom;  /* custom-theme font (optional) */
static TTFont *g_font;       /* TTF for non-bitmap glyphs, or NULL */
static int     g_font_all;   /* 1 = TTF draws every glyph (custom font) */

#define GLYPH_CACHE 512
typedef struct {
    TTFont  *font;
    int      cp, px;
    SDL_Texture *tex;
    int      w, h, adv, xoff, yoff;
    uint32_t used;
} Glyph;
static Glyph    g_glyphs[GLYPH_CACHE];
static uint32_t g_glyph_clock;

static int ttf_open(TTFont *f, const char *path) {
    f->loaded = 0; f->data = NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return 0; }
    f->data = malloc(sz);
    if (!f->data) { fclose(fp); return 0; }
    if (fread(f->data, 1, sz, fp) != (size_t)sz) { free(f->data); f->data = NULL; fclose(fp); return 0; }
    fclose(fp);
    if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        free(f->data); f->data = NULL; return 0;
    }
    f->loaded = 1;
    return 1;
}

/* stbtt scale that makes the font's cap height match the bitmap's (7 rows
   per menu scale step), so a TTF glyph sits at the same size as bitmap
   text on the same line. */
static float ttf_scale(TTFont *f, int scale) {
    int x0, y0, x1, y1;
    if (stbtt_GetCodepointBox(&f->info, 'H', &x0, &y0, &x1, &y1) && y1 > y0)
        return (float)(7 * scale) / (float)(y1 - y0);
    return stbtt_ScaleForPixelHeight(&f->info, 9.0f * scale);
}

/* UTF-8: decode one codepoint, advance *s past it */
static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int cp; int n;
    if (p[0] < 0x80)        { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; n = 4; }
    else { *s += 1; return 0xFFFD; }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += n;
    return cp;
}

static Glyph *glyph_get(SDL_Renderer *r, TTFont *f, int cp, int scale) {
    Glyph *lru = &g_glyphs[0];
    for (int i = 0; i < GLYPH_CACHE; i++) {
        Glyph *g = &g_glyphs[i];
        if (g->font == f && g->cp == cp && g->px == scale) { g->used = ++g_glyph_clock; return g; }
        if (g->used < lru->used) lru = g;
    }
    if (lru->tex) { real_SDL_DestroyTexture(lru->tex); lru->tex = NULL; }
    float sc = ttf_scale(f, scale);
    int w = 0, h = 0, xo = 0, yo = 0;
    unsigned char *bmp = stbtt_GetCodepointBitmap(&f->info, 0, sc, cp, &w, &h, &xo, &yo);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
    lru->font = f; lru->cp = cp; lru->px = scale;
    lru->w = w; lru->h = h; lru->xoff = xo; lru->yoff = yo;
    lru->adv = (int)(adv * sc + 0.5f);
    lru->tex = NULL;
    lru->used = ++g_glyph_clock;
    if (bmp && w > 0 && h > 0) {
        uint32_t *rgba = malloc((size_t)w * h * 4);
        if (rgba) {
            for (int i = 0; i < w * h; i++)
                rgba[i] = 0x00FFFFFFu | ((uint32_t)bmp[i] << 24);  /* white, alpha=coverage */
            SDL_Texture *t = real_SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888,
                                                    SDL_TEXTUREACCESS_STATIC, w, h);
            if (t) {
                SDL_UpdateTexture(t, NULL, rgba, w * 4);
                SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
                lru->tex = t;
            }
            free(rgba);
        }
    }
    if (bmp) stbtt_FreeBitmap(bmp, NULL);
    return lru;
}

static void font_free(TTFont *f) {
    if (f->data) { free(f->data); f->data = NULL; }
    f->loaded = 0;
}

/* The TTF used for glyphs the bitmap can't draw, plus whether it should
   draw every glyph (a custom theme font) or only the non-bitmap ones (a
   translation, so the bitmap English look and arrows are kept). */
static void font_select(void) {
    if (g_lang.sel > 0 && g_ttf_tr.loaded) {
        g_font = &g_ttf_tr; g_font_all = 0;   /* localise: bitmap + TTF fill-in */
    } else if (app.theme >= THEME_COUNT && g_has_custom && g_ttf_custom.loaded) {
        g_font = &g_ttf_custom; g_font_all = 1;  /* custom theme wants its font */
    } else {
        g_font = NULL; g_font_all = 0;         /* built-in bitmap look */
    }
}

static void glyph_cache_clear(void) {
    for (int i = 0; i < GLYPH_CACHE; i++) {
        if (g_glyphs[i].tex) { real_SDL_DestroyTexture(g_glyphs[i].tex); g_glyphs[i].tex = NULL; }
        g_glyphs[i].font = NULL; g_glyphs[i].used = 0;
    }
}

static void font_load_all(const char *dir, const char *custom_font) {
    char path[MAX_PATH];
    glyph_cache_clear();
    font_free(&g_ttf_tr); font_free(&g_ttf_custom);
    snprintf(path, sizeof(path), "%s/fonts/Translate.otf", dir);
    ttf_open(&g_ttf_tr, path);
    if (custom_font && custom_font[0]) {
        snprintf(path, sizeof(path), "%s/fonts/%s", dir, custom_font);
        ttf_open(&g_ttf_custom, path);
    }
    font_select();
}

static void ensure_dirs(void) {
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s/%s", sdcard_path(), SAVES_DIR);    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/%s", sdcard_path(), STATES_DIR);   mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/%s", sdcard_path(), PREVIEWS_DIR); mkdir(dir, 0755);
}

static void capture_game_bg(void) {
    if (!app.renderer || !app.virtual_fb) return;

    if (!app.game_bg_pixels)
        app.game_bg_pixels = malloc(VFB_W * VFB_H * 4);
    if (!app.game_bg_pixels) return;

    real_SDL_SetRenderTarget(app.renderer, app.virtual_fb);
    SDL_RenderReadPixels(app.renderer, NULL,
                         SDL_PIXELFORMAT_RGBA8888,
                         app.game_bg_pixels, VFB_W * 4);

    if (!app.game_bg_tex) {
        app.game_bg_tex = real_SDL_CreateTexture(app.renderer,
            SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, VFB_W, VFB_H);
    }
    if (app.game_bg_tex) {
        void *tp; int pitch;
        if (SDL_LockTexture(app.game_bg_tex, NULL, &tp, &pitch) == 0) {
            memcpy(tp, app.game_bg_pixels, VFB_W * VFB_H * 4);
            SDL_UnlockTexture(app.game_bg_tex);
        }
    }
}

static void preview_path(char *out, size_t sz, int slot) {
    snprintf(out, sz, "%s/%s/%s.%d.raw",
             sdcard_path(), PREVIEWS_DIR, app.rom_name, slot);
}

static void preview_save(int slot) {
    char path[MAX_PATH];
    FILE *f;
    if (!app.game_bg_pixels) return;
    preview_path(path, sizeof(path), slot);
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(app.game_bg_pixels, VFB_W * VFB_H * 4, 1, f);
    fclose(f);
}

static int preview_exists(int slot) {
    char path[MAX_PATH];
    struct stat st;
    preview_path(path, sizeof(path), slot);
    return stat(path, &st) == 0;
}

static void preview_free(void) {
    if (app.preview_tex) {
        real_SDL_DestroyTexture(app.preview_tex);
        app.preview_tex = NULL;
    }
    app.preview_slot = -1;
}

static void preview_load(int slot) {
    char path[MAX_PATH];
    FILE *f;
    void *pixels;
    if (app.preview_slot == slot && app.preview_tex) return;
    preview_free();
    app.preview_slot = slot;
    preview_path(path, sizeof(path), slot);
    f = fopen(path, "rb");
    if (!f) return;
    pixels = malloc(VFB_W * VFB_H * 4);
    if (!pixels) { fclose(f); return; }
    if (fread(pixels, VFB_W * VFB_H * 4, 1, f) == 1 && app.renderer) {
        app.preview_tex = real_SDL_CreateTexture(app.renderer,
            SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, VFB_W, VFB_H);
        if (app.preview_tex) {
            void *tp; int pitch;
            if (SDL_LockTexture(app.preview_tex, NULL, &tp, &pitch) == 0) {
                memcpy(tp, pixels, VFB_W * VFB_H * 4);
                SDL_UnlockTexture(app.preview_tex);
            } else {
                real_SDL_DestroyTexture(app.preview_tex);
                app.preview_tex = NULL;
            }
        }
    }
    free(pixels);
    fclose(f);
}

static const uint8_t font5x7[][5] = {

    {0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},
    {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},
    {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},
    {0x14,0x08,0x3E,0x08,0x14},
    {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},
    {0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},
    {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},
    {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},
    {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},
    {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43},
    {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},
    {0x00,0x41,0x41,0x7F,0x00},
    {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},
    {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7E,0x09,0x01,0x02},
    {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},
    {0x00,0x44,0x7D,0x40,0x00},
    {0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},
    {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},
    {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C},
    {0x7C,0x08,0x04,0x04,0x08},
    {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},
    {0x3C,0x40,0x40,0x20,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},
    {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},
};

static void draw_char(SDL_Renderer *r, char c, int x, int y, int scale, SDL_Color col) {
    if (c < ' ' || c > 'z') return;
    const uint8_t *glyph = font5x7[(unsigned char)c - ' '];
    SDL_SetRenderDrawColor(r, col.r, col.g, col.b, col.a);
    for (int ci = 0; ci < 5; ci++)
        for (int row = 0; row < 7; row++)
            if (glyph[ci] & (1 << row)) {
                SDL_Rect px = {x + ci * scale, y + row * scale, scale, scale};
                SDL_RenderFillRect(r, &px);
            }
}

/* ASCII in this range is drawn by the bitmap font (Fun Drastic's own look,
   including the arrows); everything else - accents, CJK - comes from the
   active TTF. A custom theme font (g_font_all) draws its own letters but
   still keeps Fun Drastic's bitmap arrows - the selector '<' '>' stay ours. */
#define BITMAP_HAS(cp) ((cp) >= 0x20 && (cp) <= 0x7A)
#define IS_ARROW(cp)   ((cp) == '<' || (cp) == '>')
#define USE_BITMAP(cp) (BITMAP_HAS(cp) && (!g_font_all || IS_ARROW(cp)))

static void draw_string(SDL_Renderer *r, const char *s, int x, int y,
                        int scale, SDL_Color col) {
    if (!(g_font && g_font->loaded)) {
        for (; *s; s++, x += 6 * scale)
            draw_char(r, *s, x, y, scale, col);
        return;
    }
    int baseline = y + 7 * scale;
    while (*s) {
        int cp = utf8_next(&s);
        if (USE_BITMAP(cp)) {
            draw_char(r, (char)cp, x, y, scale, col);
            x += 6 * scale;
        } else {
            Glyph *g = glyph_get(r, g_font, cp, scale);
            if (g->tex) {
                SDL_SetTextureColorMod(g->tex, col.r, col.g, col.b);
                SDL_SetTextureAlphaMod(g->tex, col.a);
                SDL_Rect dst = { x + g->xoff, baseline + g->yoff, g->w, g->h };
                real_SDL_RenderCopy(r, g->tex, NULL, &dst);
            }
            x += g->adv;
        }
    }
}

static int str_w(const char *s, int scale) {
    if (!(g_font && g_font->loaded)) return (int)strlen(s) * 6 * scale;
    int w = 0;
    while (*s) {
        int cp = utf8_next(&s);
        if (USE_BITMAP(cp)) {
            w += 6 * scale;
        } else {
            float sc = ttf_scale(g_font, scale);
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&g_font->info, cp, &adv, &lsb);
            w += (int)(adv * sc + 0.5f);
        }
    }
    return w;
}
static int str_h(int scale)                { return 7 * scale; }

static void fill_rect(SDL_Renderer *r, int x, int y, int w, int h,
                      int R, int G, int B, int A) {
    SDL_SetRenderDrawColor(r, R, G, B, A);
    SDL_Rect rc = {x, y, w, h};
    SDL_RenderFillRect(r, &rc);
}

static void draw_centered(SDL_Renderer *r, const char *s,
                          int bx, int by, int bw, int bh,
                          int scale, SDL_Color col) {
    int tw = str_w(s, scale), th = str_h(scale);
    draw_string(r, s, bx + (bw - tw) / 2, by + (bh - th) / 2, scale, col);
}

static void menu_bg(SDL_Renderer *r) {
    fill_rect(r, 0, 0, VFB_W, VFB_H, C_BG);
}

static void draw_list_row(SDL_Renderer *r, int rx, int iy, int rw, int rh,
                          int sel, int show_sep) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    if (sel) {
        fill_rect(r, rx, iy + 2, rw, rh - 4, C_SEL);
        fill_rect(r, rx, iy + 2,  3, rh - 4, C_ACC);
    } else {
        fill_rect(r, rx, iy + 2, rw, rh - 4, C_ITEM);
        if (show_sep) fill_rect(r, rx + 12, iy, rw - 24, 1, C_SEP);
    }
}

static void draw_scrollbar(SDL_Renderer *r, int x, int y, int h,
                           int total, int vis, int scroll) {
    if (total <= vis) return;
    int thumb_h = h * vis / total;
    if (thumb_h < 6) thumb_h = 6;
    int thumb_y = y + scroll * (h - thumb_h) / (total - vis);
    fill_rect(r, x, y, 3, h, 40, 40, 60, 255);
    fill_rect(r, x, thumb_y, 3, thumb_h, C_ACC);
}

/* Row height fitting `count` rows between content top and footer. */
static int fit_row_h(int count) {
    int rh    = MENU_RH;
    int avail = FOOTER_Y - CONTENT_Y;
    if (count > 0 && count * rh > avail) rh = avail / count;
    return rh;
}

static void draw_header(SDL_Renderer *r) {
    fill_rect(r, 0, 0, VFB_W, g_content_y, C_HDR);
    fill_rect(r, 0, g_content_y - 2, VFB_W, 2, C_ACC);

    const char *title = app.rom_display[0] ? app.rom_display : (app.rom_name[0] ? app.rom_name : "FUN DRASTIC");
    int hty = (g_content_y - str_h(MN_BIG)) / 2;
    draw_string(r, title, 16, hty, MN_BIG, COL(ACC));
}

static void draw_footer(SDL_Renderer *r, const char *left, const char *right) {
    fill_rect(r, 0, FOOTER_Y, VFB_W, FOOTER_H, C_HDR);
    fill_rect(r, 0, FOOTER_Y, VFB_W, 2, C_SEP);
    int ty = FOOTER_Y + (FOOTER_H - str_h(MN_NRM)) / 2;
    if (left)  draw_string(r, left,  16, ty, MN_NRM, COL(DIM));
    if (right) {
        int rw = str_w(right, MN_NRM);
        draw_string(r, right, VFB_W - rw - 16, ty, MN_NRM, COL(DIM));
    }
}

static void present_vfb(void) {
    if (app.quitting) {
        present_black(app.renderer);
        return;
    }
    real_SDL_SetRenderTarget(app.renderer, NULL);
    real_SDL_RenderClear(app.renderer);
    real_SDL_RenderCopyEx(app.renderer, app.virtual_fb, NULL, &ROT_DST,
                          ROT_ANGLE, NULL, SDL_FLIP_NONE);
    real_SDL_RenderPresent(app.renderer);
    real_SDL_SetRenderTarget(app.renderer, app.virtual_fb);
}

#define CHEAT_ROWS_VISIBLE 6

static void menu_render_cheats(SDL_Renderer *r) {

    menu_bg(r);
    draw_header(r);
    draw_footer(r, tr("B: BACK"), NULL);

    const int IX = 16, IW = VFB_W - 32, IH = MENU_RH, IY = CONTENT_Y, IPAD = 16;

    if (!g_cheats.loaded || g_cheats.count == 0) {
        const char *msg;
        if (!app.rom_path[0])            msg = "No game loaded.";
        else if (!g_cheats.game_code[0]) msg = "Could not read ROM header.";
        else                             msg = "No cheats found for this game.";
        int ty = IY + (IH - str_h(MN_BIG)) / 2;
        draw_string(r, tr(msg), IX + IPAD, ty, MN_BIG, COL(DIM));
        return;
    }

    int max_scroll = g_cheats.count - CHEAT_ROWS_VISIBLE;
    if (app.cheat_scroll > max_scroll) app.cheat_scroll = max_scroll;
    if (app.cheat_scroll < 0)          app.cheat_scroll = 0;

    int visible = g_cheats.count < CHEAT_ROWS_VISIBLE ? g_cheats.count : CHEAT_ROWS_VISIBLE;
    for (int i = 0; i < visible; i++) {
        int idx = app.cheat_scroll + i;
        if (idx >= g_cheats.count) break;
        CheatEntry *e = &g_cheats.entries[idx];

        int iy  = IY + i * IH;
        int sel = (idx == app.menu_item);

        draw_list_row(r, IX, iy, IW, IH, sel, i < visible - 1);

        int ty = iy + (IH - str_h(MN_BIG)) / 2;

        SDL_Color tc = sel ? COL(TXT) : (e->enabled ? COL(TXT) : COL(DIM));
        draw_string(r, e->name, IX + IPAD, ty, MN_BIG, tc);

        char vbuf[16];
        snprintf(vbuf, sizeof(vbuf), "< %s >", e->enabled ? tr("ON") : tr("OFF"));
        SDL_Color vc = e->enabled ? COL(ACC) : COL(DIM);
        int vw = str_w(vbuf, MN_BIG);
        draw_string(r, vbuf, IX + IW - vw - IPAD, ty, MN_BIG, vc);
    }

    if (g_cheats.count > CHEAT_ROWS_VISIBLE) {
        int bar_x   = IX + IW + 4;
        int bar_h   = visible * IH;
        float frac  = (float)app.cheat_scroll / (g_cheats.count - CHEAT_ROWS_VISIBLE);
        int thumb_y = IY + (int)(frac * (bar_h - 20));
        fill_rect(r, bar_x, IY, 3, bar_h, 40, 40, 60, 255);
        fill_rect(r, bar_x, thumb_y, 3, 20, C_ACC);
    }
}

static void menu_input_cheats(int btn) {
    switch (btn) {
    case JOY_UP:
        if (g_cheats.count > 0) {
            app.menu_item = (app.menu_item - 1 + g_cheats.count) % g_cheats.count;

            if (app.menu_item < app.cheat_scroll)
                app.cheat_scroll = app.menu_item;
            if (app.menu_item >= app.cheat_scroll + CHEAT_ROWS_VISIBLE)
                app.cheat_scroll = app.menu_item - CHEAT_ROWS_VISIBLE + 1;
        }
        break;
    case JOY_DOWN:
        if (g_cheats.count > 0) {
            app.menu_item = (app.menu_item + 1) % g_cheats.count;

            if (app.menu_item >= app.cheat_scroll + CHEAT_ROWS_VISIBLE)
                app.cheat_scroll = app.menu_item - CHEAT_ROWS_VISIBLE + 1;
            if (app.menu_item < app.cheat_scroll)
                app.cheat_scroll = app.menu_item;
        }
        break;
    case JOY_A:
    case JOY_LEFT:
    case JOY_RIGHT:
        if (g_cheats.loaded && app.menu_item < g_cheats.count) {
            g_cheats.entries[app.menu_item].enabled ^= 1;
            g_cheats.dirty = 1;
        }
        break;
    case JOY_B:
        if (g_cheats.dirty) app.cheat_apply_pending = 1;
        app.view      = VIEW_OPTIONS;
        app.menu_item = OPT_CHEATS;
        break;
    }
}
static void menu_render_main(SDL_Renderer *r) {

static const char *main_labels[MI_COUNT] = {
    "CONTINUE", "SAVE STATE", "LOAD STATE", "OPTIONS", "RESET", "QUIT",
};

    if (app.game_bg_tex) {
        SDL_SetTextureColorMod(app.game_bg_tex, 65, 65, 78);
        real_SDL_RenderCopy(r, app.game_bg_tex, NULL, NULL);
        SDL_SetTextureColorMod(app.game_bg_tex, 255, 255, 255);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        fill_rect(r, 0, 0, VFB_W, VFB_H, 0, 0, 10, 110);
    } else {
        menu_bg(r);
    }

    draw_header(r);
    draw_footer(r, tr("B: CLOSE"), tr("A: SELECT"));

    const int IX = 16, IW = g_prev_x - IX - 14, IH = fit_row_h(MI_COUNT), IY = CONTENT_Y, IPAD = 16;

    for (int i = 0; i < MI_COUNT; i++) {
        int iy = IY + i * IH;
        int sel = (i == app.menu_item);

        draw_list_row(r, IX, iy, IW, IH, sel, 0);

        SDL_Color tc = sel ? COL(TXT) : COL(DIM);
        int ty = iy + (IH - str_h(MN_BIG)) / 2;
        const char *lbl = main_labels[i];
        char quit_lbl[16];
        if (i == MI_QUIT) {
            snprintf(quit_lbl, sizeof(quit_lbl), "%s",
                     app.auto_resume ? "SAVE & QUIT" : "QUIT");
            lbl = quit_lbl;
        }
        draw_string(r, tr(lbl), IX + IPAD, ty, MN_BIG, tc);

        if (i == MI_SAVE || i == MI_LOAD) {
            char slotbuf[4];
            int rs = (i == MI_SAVE) ? app.state_slot : app.load_slot;
            snprintf(slotbuf, sizeof(slotbuf), "%d", rs + 1);
            int sw = str_w(slotbuf, MN_BIG);
            SDL_Color sc = sel ? COL(TXT) : COL(ACC);
            draw_string(r, slotbuf, IX + IW - sw - IPAD, ty, MN_BIG, sc);
        }

        if (i == MI_OPTIONS) {
            int ax = IX + IW - str_w(">", MN_BIG) - IPAD;
            draw_string(r, ">", ax, ty, MN_BIG, sel ? COL(TXT) : COL(DIM));
        }
    }

    if (app.menu_item == MI_SAVE || app.menu_item == MI_LOAD) {
        int rs = (app.menu_item == MI_SAVE) ? app.state_slot : app.load_slot;
        preview_load(rs);

        fill_rect(r, PREV_X - 3, PREV_Y - 3, PREV_W + 6, PREV_H + 6, C_SEP);
        fill_rect(r, PREV_X, PREV_Y, PREV_W, PREV_H, 0, 0, 0, 255);

        if (app.preview_tex) {
            SDL_Rect dst = {PREV_X, PREV_Y, PREV_W, PREV_H};
            real_SDL_RenderCopy(r, app.preview_tex, NULL, &dst);
        } else {
            const char *msg = preview_exists(rs) ? "NO PREVIEW" : "EMPTY SLOT";
            draw_centered(r, tr(msg), PREV_X, PREV_Y, PREV_W, PREV_H, MN_NRM, COL(DIM));
        }

        {
            char num[4];
            snprintf(num, sizeof(num), "%d", rs + 1);
            const char *lt = "< ", *rt = " >";
            int lt_w = str_w(lt, MN_NRM), num_w = str_w(num, MN_NRM), rt_w = str_w(rt, MN_NRM);
            int total = lt_w + num_w + rt_w;
            int bx = PREV_X + (PREV_W - total) / 2;
            int by = PREV_Y + PREV_H + 14;

            if (app.slot_arrow_timer > 0) app.slot_arrow_timer--;
            else                           app.slot_arrow_flash = 0;

            /* On-background text: DIM/ACC only (TXT is for the selection bar,
               and is dark on light-pill themes like Leaf - invisible here). */
            SDL_Color lt_col = (app.slot_arrow_flash == -1) ? COL(DIM) : COL(ACC);
            SDL_Color rt_col = (app.slot_arrow_flash ==  1) ? COL(DIM) : COL(ACC);

            draw_string(r, lt,  bx,              by, MN_NRM, lt_col);
            draw_string(r, num, bx + lt_w,       by, MN_NRM, COL(DIM));
            draw_string(r, rt,  bx + lt_w + num_w, by, MN_NRM, rt_col);
        }
    }
}

static void menu_render_options(SDL_Renderer *r) {
    menu_bg(r);
    draw_header(r);
    draw_footer(r, tr("B: BACK"),
        (app.menu_item == OPT_THEME) ? "A: CHANGE" : "A: SELECT");

    const int RX = 18, RW = VFB_W - 36, RH = MENU_RH, RY = CONTENT_Y, RPAD = 16;
    const int VISIBLE = g_menu_vis;

    const char *labels[OPT_COUNT] = {
        "AUDIOVISUAL", "EMULATOR", "CHEATS", "USER", "SHORTCUTS", "CONTROLS", "THEME",
        "MENU LANGUAGE",
    };
    const char *vals[OPT_COUNT] = { ">", ">", ">", ">", ">", ">", NULL, NULL };
    char theme_val[32];
    snprintf(theme_val, sizeof(theme_val), "< %s >", theme_name(app.theme));
    vals[OPT_THEME] = theme_val;
    char lang_val[32];
    snprintf(lang_val, sizeof(lang_val), "< %s >",
             g_lang.n_names ? g_lang.names[g_lang.sel] : "english");
    vals[OPT_LANGUAGE] = lang_val;

    static int opt_scroll = 0;
    if (app.menu_item < opt_scroll) opt_scroll = app.menu_item;
    if (app.menu_item >= opt_scroll + VISIBLE) opt_scroll = app.menu_item - VISIBLE + 1;
    if (opt_scroll < 0) opt_scroll = 0;
    if (opt_scroll > OPT_COUNT - VISIBLE) opt_scroll = OPT_COUNT - VISIBLE;

    for (int i = 0; i < VISIBLE && (opt_scroll + i) < OPT_COUNT; i++) {
        int idx = opt_scroll + i;
        int iy = RY + i * RH;
        int sel = (idx == app.menu_item);

        draw_list_row(r, RX, iy, RW, RH, sel, i > 0);

        SDL_Color lc = sel ? COL(TXT) : COL(DIM);
        int ty = iy + (RH - str_h(MN_BIG)) / 2;
        draw_string(r, tr(labels[idx]), RX + RPAD, ty, MN_BIG, lc);

        SDL_Color vc = sel ? COL(TXT) : COL(ACC);
        int vw = str_w(vals[idx], MN_BIG);
        draw_string(r, tr(vals[idx]), RX + RW - vw - RPAD, ty, MN_BIG, vc);
        if (idx != OPT_THEME && idx != OPT_LANGUAGE) {
            int chevx = RX + RW - vw - str_w(">", MN_BIG) - RPAD - 8;
            draw_string(r, ">", chevx, ty, MN_BIG, sel ? COL(TXT) : COL(DIM));
        }
    }
    if (OPT_COUNT > VISIBLE)
        draw_scrollbar(r, RX + RW + 6, CONTENT_Y, VISIBLE * MENU_RH,
                       OPT_COUNT, VISIBLE, opt_scroll);
}

static SDL_Rect vfb_to_layprev(SDL_Rect vr) {
    if (vr.w == 0 || vr.h == 0) return (SDL_Rect){0,0,0,0};
    return (SDL_Rect){
        PREV_X + vr.x * PREV_W / VFB_W,
        PREV_Y + vr.y * PREV_H / VFB_H,
        vr.w * PREV_W / VFB_W,
        vr.h * PREV_H / VFB_H,
    };
}

static void menu_render_layout(SDL_Renderer *r) {
    menu_bg(r);
    draw_header(r);
    static const char *pip_corner_names[] = { "TOP RIGHT", "BOT RIGHT", "BOT LEFT", "TOP LEFT" };
    const char *layout_hint = "A: APPLY";
    draw_footer(r, tr("B: BACK"), layout_hint);

    fill_rect(r, PREV_X - 3, PREV_Y - 3, PREV_W + 6, PREV_H + 6, C_SEP);
    fill_rect(r, PREV_X, PREV_Y, PREV_W, PREV_H, 0, 0, 0, 255);

    screen_slot_t pip_preview[2];
    const screen_slot_t *sl;
    int preview_idx = (app.menu_item < LAYOUT_COUNT) ? app.menu_item : app.layout;
    if (preview_idx == 0) {
        get_pip_slots(app.swap_screens, app.pip_corner, app.pixel_perfect, pip_preview);
        sl = pip_preview;
    } else {
        const screen_slot_t (*slots)[2] = app.pixel_perfect ? layout_slots_pp : layout_slots;
        sl = slots[preview_idx];
    }
    SDL_Rect d0 = vfb_to_layprev(sl[0].rect);
    SDL_Rect d1 = vfb_to_layprev(sl[1].rect);

    int pip_idx  = (sl[0].rect.w < sl[1].rect.w) ? 0 : 1;
    int fill_idx = 1 - pip_idx;
    SDL_Rect *dr[2] = { &d0, &d1 };
    SDL_Texture *tx[2] = { app.screens[0], app.screens[1] };
    uint8_t al[2] = { 255, sl[1].alpha };

    SDL_Color ph[2] = { {45,110,210,255}, {48,155,90,255} };

    for (int pass = 0; pass < 2; pass++) {
        int i = (pass == 0) ? fill_idx : pip_idx;
        if (dr[i]->w <= 0 || dr[i]->h <= 0) continue;
        if (tx[i]) {
            SDL_SetTextureAlphaMod(tx[i], al[i]);
            SDL_SetTextureBlendMode(tx[i], al[i] < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
            real_SDL_RenderCopy(r, tx[i], NULL, dr[i]);
            SDL_SetTextureAlphaMod(tx[i], 255);
            SDL_SetTextureBlendMode(tx[i], SDL_BLENDMODE_NONE);
        } else {
            SDL_SetRenderDrawBlendMode(r, al[i] < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(r, ph[i].r, ph[i].g, ph[i].b, al[i]);
            SDL_RenderFillRect(r, dr[i]);
        }
    }

    int preview_layout = (app.menu_item < LAYOUT_COUNT) ? app.menu_item : app.layout;
    int lw = str_w(layout_names[preview_layout], MN_NRM);
    int label_y = PREV_Y + PREV_H + 12;
    draw_string(r, tr(layout_names[preview_layout]),
                PREV_X + (PREV_W - lw) / 2,
                label_y, MN_NRM, COL(ACC));

    if (app.menu_item == 0) {
        char pip_buf[32];
        snprintf(pip_buf, sizeof(pip_buf), "< %s >", tr(pip_corner_names[app.pip_corner]));
        int pw = str_w(pip_buf, MN_NRM);
        draw_string(r, pip_buf,
                    PREV_X + (PREV_W - pw) / 2,
                    label_y + str_h(MN_NRM) + 10, MN_NRM, COL(DIM));
    }

    const int LX = 16, LW = g_prev_x - LX - 14, LH = fit_row_h(LAYOUT_MENU_ITEMS), LY = g_content_y, LPAD = 14;

    for (int i = 0; i < LAYOUT_MENU_ITEMS; i++) {
        int iy  = LY + i * LH;
        int sel = (i == app.menu_item);

        draw_list_row(r, LX, iy, LW, LH, sel, i > 0);

        int ty = iy + (LH - str_h(MN_BIG)) / 2;

        if (i == LAYOUT_COUNT) {
            SDL_Color tc = sel ? COL(TXT) : COL(DIM);
            draw_string(r, tr("SCALING"), LX + LPAD, ty, MN_BIG, tc);
            char vbuf[16];
            snprintf(vbuf, sizeof(vbuf), "< %s >", app.pixel_perfect ? tr("NATIVE") : tr("ASPECT"));
            SDL_Color vc = sel ? COL(TXT) : COL(ACC);
            int vw = str_w(vbuf, MN_BIG);
            draw_string(r, vbuf, LX + LW - vw - LPAD, ty, MN_BIG, vc);
        } else {
            SDL_Color tc = sel ? COL(TXT) : COL(DIM);
            draw_string(r, tr(layout_names[i]), LX + LPAD, ty, MN_BIG, tc);
            if (i == app.layout)
                draw_string(r, tr("*"), LX + LW - str_w("*", MN_BIG) - LPAD, ty, MN_BIG, sel ? COL(TXT) : COL(ACC));
        }
    }
}

static void menu_close(void);

static void menu_input_main(int btn) {
    switch (btn) {
    case JOY_UP:
        app.menu_item = (app.menu_item - 1 + MI_COUNT) % MI_COUNT;
        preview_free();
        break;
    case JOY_DOWN:
        app.menu_item = (app.menu_item + 1) % MI_COUNT;
        preview_free();
        break;

    case JOY_LEFT:
    case JOY_RIGHT:
        if (app.menu_item == MI_SAVE || app.menu_item == MI_LOAD) {
            int *rs = (app.menu_item == MI_SAVE) ? &app.state_slot : &app.load_slot;
            int  d  = (btn == JOY_RIGHT) ? 1 : (MAX_SLOTS - 1);
            *rs = (*rs + d) % MAX_SLOTS;
            preview_free();
            app.slot_arrow_flash = (btn == JOY_RIGHT) ? 1 : -1;
            app.slot_arrow_timer =  4;
        }
        break;
    case JOY_A:
    case JOY_START:
        switch (app.menu_item) {
        case MI_CONTINUE:
            menu_close();
            break;
        case MI_SAVE:
            menu_close();
            drastic_save_state(app.state_slot);
            preview_save(app.state_slot);
            break;
        case MI_LOAD:
            menu_close();
            app.ff_speed = 0;
            emu_config_save();
            ff_apply();
            drastic_load_state(app.load_slot);
            break;
        case MI_OPTIONS:
            app.view = VIEW_OPTIONS;
            app.menu_item = 0;
            break;
        case MI_RESET:
            menu_close();

            {
                char spath[MAX_PATH];
                skip_resume_path(spath, sizeof(spath));
                FILE *sf = fopen(spath, "w");
                if (sf) { fprintf(sf, "1\n"); fclose(sf); }
            }
            drastic_reset();
            break;
        case MI_QUIT:
            app.quitting = 1;

            if (real_SDL_PauseAudio) real_SDL_PauseAudio(1);
            if (app.auto_resume) {
                drastic_save_state(app.state_slot);
                drastic_await_save();
#ifdef DRASTIC_ARM32
                nds_await_state_written(app.state_slot);
#endif
                preview_save(app.state_slot);
            }
            emu_config_save();
            drastic_quit();
            break;
        }
        break;
    case JOY_B:
        menu_close();
        break;
    default: break;
    }
}

static void menu_input_options(int btn) {
    switch (btn) {
    case JOY_UP:
        app.menu_item = (app.menu_item - 1 + OPT_COUNT) % OPT_COUNT;
        break;
    case JOY_DOWN:
        app.menu_item = (app.menu_item + 1) % OPT_COUNT;
        break;
    case JOY_LEFT:
        if (app.menu_item == OPT_THEME) {
            app.theme = (app.theme - 1 + theme_total()) % theme_total();
            theme_apply(app.theme);
            font_select();
            emu_config_save();
        } else if (app.menu_item == OPT_LANGUAGE && g_lang.n_names > 0) {
            g_lang.sel = (g_lang.sel - 1 + g_lang.n_names) % g_lang.n_names;
            lang_set_current();
        }
        break;
    case JOY_RIGHT:
        if (app.menu_item == OPT_THEME) {
            app.theme = (app.theme + 1) % theme_total();
            theme_apply(app.theme);
            font_select();
            emu_config_save();
        } else if (app.menu_item == OPT_LANGUAGE && g_lang.n_names > 0) {
            g_lang.sel = (g_lang.sel + 1) % g_lang.n_names;
            lang_set_current();
        }
        break;
    case JOY_A:
        switch (app.menu_item) {
        case OPT_AUDIOVISUAL:
            app.view = VIEW_AUDIOVISUAL;
            app.menu_item = 0;
            break;
        case OPT_EMULATOR:
            app.view = VIEW_EMULATOR;
            app.menu_item = 0;
            break;
        case OPT_CHEATS:
            if (!g_cheats.loaded) cheat_parse_usrcheat();
            app.view         = VIEW_CHEATS;
            app.menu_item    = 0;
            app.cheat_scroll = 0;
            break;
        case OPT_USER:
            app.view = VIEW_USER;
            app.menu_item = 0;
            break;
        case OPT_SHORTCUTS:
            app.view = VIEW_SHORTCUTS;
            app.menu_item = 0;
            app.sc_binding = -1;
            break;
        case OPT_CONTROLS:
            app.view = VIEW_CONTROLS;
            app.menu_item = 0;
            app.ctrl_binding = -1;
            break;
        case OPT_THEME:

            app.theme = (app.theme + 1) % theme_total();
            theme_apply(app.theme);
            font_select();
            emu_config_save();
            break;
        }
        break;
    case JOY_B:
        app.view = VIEW_MAIN;
        app.menu_item = MI_OPTIONS;
        break;
    default: break;
    }
}

static void menu_input_layout(int btn) {
    switch (btn) {
    case JOY_UP:
        app.menu_item = (app.menu_item - 1 + LAYOUT_MENU_ITEMS) % LAYOUT_MENU_ITEMS;
        break;
    case JOY_DOWN:
        app.menu_item = (app.menu_item + 1) % LAYOUT_MENU_ITEMS;
        break;
    case JOY_LEFT:
    case JOY_RIGHT: {
        int dir = (btn == JOY_RIGHT) ? 1 : -1;
        if (app.menu_item == 0) {
            app.pip_corner = (app.pip_corner + dir + 4) % 4;
            emu_config_save();
        } else if (app.menu_item == LAYOUT_COUNT) {
            app.pixel_perfect ^= 1;
            overlay_reload();
            app.pip_deferred_tex = NULL;
            vfb_clear();
            emu_config_save();
        }
        break;
    }
    case JOY_A:
        if (app.menu_item == LAYOUT_COUNT) {
            app.pixel_perfect ^= 1;
            overlay_reload();
            app.pip_deferred_tex = NULL;
            vfb_clear();
            emu_config_save();
        } else {
            app.layout = app.menu_item;
            overlay_reload();
            emu_config_save();
            app.view = VIEW_AUDIOVISUAL;
            app.menu_item = AV_LAYOUT;
        }
        break;
    case JOY_B:
        app.view = VIEW_AUDIOVISUAL;
        app.menu_item = AV_LAYOUT;
        break;
    default: break;
    }
}

static const char *sc_labels[SC_COUNT] = {
    "SAVE STATE", "LOAD STATE", "FAST FORWARD",
    "SWAP SCREENS", "NEXT LAYOUT", "PREV LAYOUT",
    "TOUCH CURSOR",
};

static void menu_render_shortcuts(SDL_Renderer *r) {
    menu_bg(r);
    draw_header(r);

    if (app.sc_binding >= 0)
        draw_footer(r, tr("HOLD MENU FOR COMBO"), tr("PRESS ANY BUTTON"));
    else
        draw_footer(r, tr("B: BACK"), tr("X: CLEAR  A: BIND"));

    const int RX = 18, RW = VFB_W - 36, RH = fit_row_h(SC_COUNT), RY = CONTENT_Y, RPAD = 16;
    char combo[32];

    for (int i = 0; i < SC_COUNT; i++) {
        int iy      = RY + i * RH;
        int sel     = (i == app.menu_item);
        int binding = (app.sc_binding == i);

        draw_list_row(r, RX, iy, RW, RH, sel, i > 0);

        SDL_Color lc = sel ? COL(TXT) : COL(DIM);
        int ty = iy + (RH - str_h(MN_BIG)) / 2;
        draw_string(r, tr(sc_labels[i]), RX + RPAD, ty, MN_BIG, lc);

        const char *val = binding ? "..." : sc_combo_name(app.sc_btn[i], app.sc_mod[i], combo);
        SDL_Color   vc  = binding ? COL(TXT) : (sel ? COL(TXT) : (app.sc_btn[i] >= 0 ? COL(ACC) : COL(DIM)));
        int vw = str_w(val, MN_BIG);
        draw_string(r, val, RX + RW - vw - RPAD, ty, MN_BIG, vc);
    }
}

/* Modal button capture: render `render` each frame until a button other than
   MENU is pressed, then return it. *mod, if given, records whether MENU was
   held at the moment of the press (used by shortcuts as a chord modifier). */
static int capture_button(void (*render)(SDL_Renderer *), int *mod) {
    int bound = 0, btn = -1;
    while (!bound) {
        PAD_poll();
        for (int b = 0; b < 32; b++) {
            if (!PAD_justPressed(b)) continue;
            if (b == JOY_MENU) continue;
            if (mod) *mod = PAD_isPressed(JOY_MENU) ? 1 : 0;
            btn = b; bound = 1; break;
        }
        render(app.renderer);
        present_vfb();
        if (!bound) real_SDL_Delay(16);
    }
    return btn;
}

static void bind_shortcut(int sc_idx) {
    app.sc_binding = sc_idx;
    int mod = 0;
    app.sc_btn[sc_idx] = capture_button(menu_render_shortcuts, &mod);
    app.sc_mod[sc_idx] = mod;
    app.sc_binding = -1;
    sc_config_save();
}

static void menu_input_shortcuts(int btn) {
    switch (btn) {
    case JOY_UP:
        app.menu_item = (app.menu_item - 1 + SC_COUNT) % SC_COUNT;
        break;
    case JOY_DOWN:
        app.menu_item = (app.menu_item + 1) % SC_COUNT;
        break;
    case JOY_A:
        bind_shortcut(app.menu_item);
        break;
    case JOY_X:
        app.sc_btn[app.menu_item] = -1;
        app.sc_mod[app.menu_item] = 1;
        sc_config_save();
        break;
    case JOY_B:
        app.view     = VIEW_OPTIONS;
        app.menu_item = OPT_SHORTCUTS;
        break;
    default: break;
    }
}

static const char *ctrl_labels[CTRL_COUNT] = {
    "UP", "DOWN", "LEFT", "RIGHT",
    "A BUTTON", "B BUTTON", "X BUTTON", "Y BUTTON",
    "L BUTTON", "R BUTTON", "SELECT", "START",
};

static void menu_render_controls(SDL_Renderer *r) {
    menu_bg(r);
    draw_header(r);

    if (app.ctrl_binding >= 0)
        draw_footer(r, NULL, tr("PRESS ANY BUTTON TO BIND"));
    else
        draw_footer(r, tr("B: BACK"), tr("X: RESET  A: REBIND"));

    const int RX = 18, RW = VFB_W - 36, RH = MENU_RH, RY = CONTENT_Y, RPAD = 16;
    const int VISIBLE = g_menu_vis;
    int scroll = app.menu_item - VISIBLE + 1;
    if (scroll < 0) scroll = 0;

    for (int vi = 0; vi < VISIBLE && (vi + scroll) < CTRL_ROWS; vi++) {
        int i       = vi + scroll;
        int iy      = RY + vi * RH;
        int sel     = (i == app.menu_item);
        int binding = (app.ctrl_binding == i);
        int is_mic  = (i == CTRL_MIC);

        draw_list_row(r, RX, iy, RW, RH, sel, vi > 0);

        SDL_Color lc = sel ? COL(TXT) : COL(DIM);
        int ty = iy + (RH - str_h(MN_BIG)) / 2;
        draw_string(r, tr(is_mic ? "MICROPHONE" : ctrl_labels[i]), RX + RPAD, ty, MN_BIG, lc);

        int cur = is_mic ? app.mic_btn : app.ctrl_map[i];
        int def = is_mic ? MIC_DEFAULT_BTN : ctrl_defaults[i];
        const char *val = binding ? "..." : (cur < 0 ? "OFF" : btn_name(cur));
        SDL_Color   vc  = binding ? COL(TXT) : (sel ? COL(TXT) : (cur != def ? COL(ACC) : COL(DIM)));
        int vw = str_w(val, MN_BIG);
        draw_string(r, val, RX + RW - vw - RPAD, ty, MN_BIG, vc);
    }

    draw_scrollbar(r, RX + RW + 6, CONTENT_Y, g_menu_vis * MENU_RH,
                   CTRL_ROWS, g_menu_vis, scroll);
}

static void bind_control(int ci_idx) {
    app.ctrl_binding = ci_idx;
    app.ctrl_map[ci_idx] = capture_button(menu_render_controls, NULL);
    app.ctrl_binding = -1;
    ctrl_config_save();
}

/* Bind the fake-mic trigger; the pressed button is saved to the emu config. */
static void bind_mic(void) {
    app.ctrl_binding = CTRL_MIC;
    app.mic_btn = capture_button(menu_render_controls, NULL);
    app.ctrl_binding = -1;
    emu_config_save();
    mic_apply();
}

static void menu_input_controls(int btn) {
    switch (btn) {
    case JOY_UP:
        app.menu_item = (app.menu_item - 1 + CTRL_ROWS) % CTRL_ROWS;
        break;
    case JOY_DOWN:
        app.menu_item = (app.menu_item + 1) % CTRL_ROWS;
        break;
    case JOY_A:
        if (app.menu_item == CTRL_MIC) bind_mic();
        else                           bind_control(app.menu_item);
        break;
    case JOY_X:
        if (app.menu_item == CTRL_MIC) {
            app.mic_btn = MIC_DEFAULT_BTN;
            emu_config_save();
            mic_apply();
        } else {
            app.ctrl_map[app.menu_item] = ctrl_defaults[app.menu_item];
            ctrl_config_save();
        }
        break;
    case JOY_B:
        app.view      = VIEW_OPTIONS;
        app.menu_item = OPT_CONTROLS;
        break;
    default: break;
    }
}

static void menu_render_overlay(SDL_Renderer *r) {
    menu_bg(r);
    draw_header(r);

    int total = app.overlay_pack_count + 1;

    if (app.overlay_pack_count == 0)
        draw_footer(r, tr("B: BACK"), tr("NO OVERLAYS FOUND"));
    else
        draw_footer(r, tr("B: BACK"), tr("A: SELECT"));

    fill_rect(r, PREV_X - 3, PREV_Y - 3, PREV_W + 6, PREV_H + 6, C_SEP);
    fill_rect(r, PREV_X, PREV_Y, PREV_W, PREV_H, 0, 0, 0, 255);

    const screen_slot_t (*slots)[2] = app.pixel_perfect ? layout_slots_pp : layout_slots;
    screen_slot_t pip_slots[2];
    const screen_slot_t *sl;
    if (app.layout == 0) {
        get_pip_slots(app.swap_screens, app.pip_corner, app.pixel_perfect, pip_slots);
        sl = pip_slots;
    } else {
        sl = slots[app.layout];
    }
    SDL_Rect d0 = vfb_to_layprev(sl[0].rect);
    SDL_Rect d1 = vfb_to_layprev(sl[1].rect);
    SDL_Color ph[2] = { {45,110,210,255}, {48,155,90,255} };
    for (int i = 0; i < 2; i++) {
        SDL_Rect *dr = (i == 0) ? &d0 : &d1;
        if (dr->w <= 0 || dr->h <= 0) continue;
        if (app.screens[i]) {
            real_SDL_RenderCopy(r, app.screens[i], NULL, dr);
        } else {
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(r, ph[i].r, ph[i].g, ph[i].b, 255);
            SDL_RenderFillRect(r, dr);
        }
    }

    SDL_Texture *prev_tex = overlay_get_preview_tex(r, app.menu_item);
    if (prev_tex) {
        SDL_Rect prev_full = { PREV_X, PREV_Y, PREV_W, PREV_H };
        real_SDL_RenderCopy(r, prev_tex, NULL, &prev_full);
    }

    const char *pname = (app.menu_item == 0) ? "NONE"
        : (app.menu_item - 1 < app.overlay_pack_count
           ? app.overlay_packs[app.menu_item - 1] : "NONE");
    int nw = str_w(pname, MN_NRM);
    draw_string(r, pname, PREV_X + (PREV_W - nw) / 2,
                PREV_Y + PREV_H + 8, MN_NRM, COL(ACC));

    if (app.menu_item == app.overlay_pack_idx) {
        int aw = str_w("ACTIVE", MN_NRM);
        draw_string(r, tr("ACTIVE"), PREV_X + (PREV_W - aw) / 2,
                    PREV_Y + PREV_H + 8 + str_h(MN_NRM) + 4, MN_NRM, COL(ACC));
    }

    const int LX = 16, LW = g_prev_x - LX - 14, LH = g_menu_rh, LY = g_content_y, LPAD = 14;
    int vis = g_menu_vis;

    if (app.menu_item < app.overlay_scroll)
        app.overlay_scroll = app.menu_item;
    if (app.menu_item >= app.overlay_scroll + vis)
        app.overlay_scroll = app.menu_item - vis + 1;

    for (int i = 0; i < vis; i++) {
        int idx = app.overlay_scroll + i;
        if (idx >= total) break;
        int iy = LY + i * LH;
        int sel = (idx == app.menu_item);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        if (sel) {
            fill_rect(r, LX, iy + 2, LW, LH - 4, C_SEL);
            fill_rect(r, LX, iy + 2,  3, LH - 4, C_ACC);
        } else {
            fill_rect(r, LX, iy + 2, LW, LH - 4, C_ITEM);
        }
        const char *lbl = (idx == 0) ? "NONE" : app.overlay_packs[idx - 1];
        SDL_Color tc = sel ? COL(TXT) : COL(DIM);
        int ty = iy + (LH - str_h(MN_BIG)) / 2;
        draw_string(r, tr(lbl), LX + LPAD, ty, MN_BIG, tc);

        if (idx == app.overlay_pack_idx)
            draw_string(r, tr("*"), LX + LW - str_w("*", MN_BIG) - LPAD, ty, MN_BIG, sel ? COL(TXT) : COL(ACC));
    }
}

static void menu_input_overlay(int btn) {
    int total = app.overlay_pack_count + 1;
    switch (btn) {
    case JOY_UP:
        app.menu_item = (app.menu_item - 1 + total) % total;
        break;
    case JOY_DOWN:
        app.menu_item = (app.menu_item + 1) % total;
        break;
    case JOY_A:
        app.overlay_pack_idx = app.menu_item;
        if (app.menu_item == 0) {
            app.overlay_pack_name[0] = '\0';
        } else {
            int pack_i = app.menu_item - 1;
            snprintf(app.overlay_pack_name, OVERLAY_NAME_LEN, "%s", app.overlay_packs[pack_i]);
        }
        overlay_reload();
        emu_config_save();
        overlay_preview_clear();
        app.view = VIEW_AUDIOVISUAL;
        app.menu_item = AV_OVERLAY;
        break;
    case JOY_B:
        overlay_preview_clear();
        app.view = VIEW_AUDIOVISUAL;
        app.menu_item = AV_OVERLAY;
        break;
    default: break;
    }
}

static const char *clock_names[]     = { "100%", "150%", "200%", "300%" };
/* Touch-cursor speed: NORMAL (index 2) is the tuned 1.0x baseline. */
static const float g_cursor_speed_mult[CURSOR_SPEED_COUNT] =
    { 0.55f, 0.75f, 1.0f, 1.4f, 1.9f };
static const char *cursor_speed_names[CURSOR_SPEED_COUNT] =
    { "SLOWEST", "SLOW", "NORMAL", "FAST", "FASTEST" };
static const char *language_names[]  = {
    "JAPANESE", "ENGLISH", "FRENCH", "GERMAN",
    "ITALIAN", "SPANISH", "CHINESE", "KOREAN"
};
static const char *month_names[] = {
    "JAN","FEB","MAR","APR","MAY","JUN",
    "JUL","AUG","SEP","OCT","NOV","DEC"
};

static void menu_render_audiovisual(SDL_Renderer *r) {
    menu_bg(r);
    draw_header(r);

    int av_needs_reset = (app.menu_item == AV_SCREEN_BLEND ||
                          app.menu_item == AV_EDGE_MARKING  ||
                          app.menu_item == AV_HIRES_3D);
    int av_is_sub  = (app.menu_item == AV_LAYOUT || app.menu_item == AV_OVERLAY);
    int av_no_packs = (app.menu_item == AV_OVERLAY && app.overlay_pack_count == 0);
    if (av_needs_reset)
        draw_footer(r, tr("B: BACK  REQUIRES RESET"), NULL);
    else if (av_is_sub)
        draw_footer(r, tr("B: BACK"), tr("A: ENTER"));
    else if (av_no_packs)
        draw_footer(r, tr("B: BACK"), tr("NO OVERLAYS FOUND"));
    else
        draw_footer(r, tr("B: BACK"), NULL);

    int ov_i = app.overlay_pack_idx - 1;
    const char *overlay_val = (app.overlay_pack_idx == 0 ||
                                app.overlay_pack_count == 0 ||
                                ov_i < 0 || ov_i >= app.overlay_pack_count)
        ? "NONE" : app.overlay_packs[ov_i];
    const char *labels[AV_COUNT] = {
        "SCREEN LAYOUT", "OVERLAY",
        "SWAP SCREENS", "IMAGE QUALITY",
        "SCREEN BLEND", "EDGE MARKING", "HI-RES 3D",
        "CURSOR SPEED",
    };
    const char *vals[AV_COUNT] = {
        layout_names[app.layout], overlay_val,
        app.swap_screens ? "ON"    : "OFF",
        app.crisp        ? "CRISP" : "SMOOTH",
        app.screen_blend ? "ON"    : "OFF",
        app.edge_marking ? "ON"    : "OFF",
        app.hires_3d     ? "ON"    : "OFF",
        cursor_speed_names[app.cursor_speed],
    };
    int is_on[AV_COUNT] = {
        0, (app.overlay_pack_idx > 0),
        app.swap_screens, app.crisp,
        app.screen_blend, app.edge_marking, app.hires_3d,
        (app.cursor_speed != CURSOR_SPEED_DEFAULT),
    };

    const int RX = 18, RW = VFB_W - 36, RH = MENU_RH, RY = CONTENT_Y, RPAD = 16;
    int av_vis = g_menu_vis;
    if (app.menu_item < app.av_scroll) app.av_scroll = app.menu_item;
    if (app.menu_item >= app.av_scroll + av_vis) app.av_scroll = app.menu_item - av_vis + 1;

    for (int vi = 0; vi < av_vis; vi++) {
        int i = app.av_scroll + vi;
        if (i >= AV_COUNT) break;
        int iy = RY + vi * RH;
        int sel = (i == app.menu_item);

        draw_list_row(r, RX, iy, RW, RH, sel, i > 0);

        SDL_Color lc = sel ? COL(TXT) : COL(DIM);
        int ty = iy + (RH - str_h(MN_BIG)) / 2;
        draw_string(r, tr(labels[i]), RX + RPAD, ty, MN_BIG, lc);

        SDL_Color vc = sel ? COL(TXT) : (is_on[i] ? COL(ACC) : COL(DIM));
        int is_sub = (i == AV_LAYOUT || i == AV_OVERLAY);
        char avbuf[32];
        const char *vstr;
        if (is_sub) {
            vstr = vals[i];
        } else {
            snprintf(avbuf, sizeof(avbuf), "< %s >", tr(vals[i]));
            vstr = avbuf;
        }
        int vw = str_w(vstr, MN_BIG);
        draw_string(r, vstr, RX + RW - vw - RPAD, ty, MN_BIG, vc);
        if (is_sub) {
            int chevx = RX + RW - vw - str_w(">", MN_BIG) - RPAD - 8;
            draw_string(r, ">", chevx, ty, MN_BIG, sel ? COL(TXT) : COL(DIM));
        }
    }

    draw_scrollbar(r, RX + RW + 6, CONTENT_Y, av_vis * MENU_RH,
                   AV_COUNT, av_vis, app.av_scroll);
}

static void menu_input_audiovisual(int btn) {
    switch (btn) {
    case JOY_UP:
        app.menu_item = (app.menu_item - 1 + AV_COUNT) % AV_COUNT;
        break;
    case JOY_DOWN:
        app.menu_item = (app.menu_item + 1) % AV_COUNT;
        break;
    case JOY_A:
        if (app.menu_item == AV_LAYOUT) {
            app.view = VIEW_LAYOUT;
            app.menu_item = app.layout;
        } else if (app.menu_item == AV_OVERLAY) {
            app.view = VIEW_OVERLAY;
            app.menu_item = app.overlay_pack_idx;
            app.overlay_scroll = 0;
        }
        break;
    case JOY_LEFT:
    case JOY_RIGHT: {
        int dir = (btn == JOY_RIGHT) ? 1 : -1;
        switch (app.menu_item) {
        case AV_LAYOUT:
            app.layout = (app.layout + dir + LAYOUT_COUNT) % LAYOUT_COUNT;
            overlay_reload();
            emu_config_save(); break;
        case AV_SWAP_SCREENS:
            app.swap_screens ^= 1;
            emu_config_save(); break;
        case AV_QUALITY:
            app.crisp ^= 1; break;
        case AV_SCREEN_BLEND:
            app.screen_blend ^= 1;
            drastic_cfg_save_all(); break;
        case AV_EDGE_MARKING:
            app.edge_marking ^= 1;
            drastic_cfg_save_all(); break;
        case AV_HIRES_3D:
            app.hires_3d ^= 1;
            drastic_cfg_save_all(); break;
        case AV_CURSOR_SPEED:
            app.cursor_speed = (app.cursor_speed + dir + CURSOR_SPEED_COUNT) % CURSOR_SPEED_COUNT;
            emu_config_save(); break;
        case AV_OVERLAY:
            break;   /* pack is chosen with A; arrows do nothing here */
        }
        break;
    }
    case JOY_B:
        app.view = VIEW_OPTIONS;
        app.menu_item = OPT_AUDIOVISUAL;
        break;
    default: break;
    }
}

static const char *slot2_names[] = { "NONE", "RUMBLE PACK", "GBA CART", "SRAM CART" };
static const char *frameskip_type_names[] = { "OFF", "AUTO", "MANUAL" };

static const char *fav_color_names[] = {
    "GRAY", "BROWN", "RED", "PINK", "ORANGE", "YELLOW",
    "LIME", "GREEN", "DARK GREEN", "TEAL", "BLUE", "DARK BLUE",
    "INDIGO", "PURPLE", "DARK PURPLE", "MAGENTA"
};

static void emu_clamp_scroll(void);

static void modal_scrim(SDL_Renderer *r) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    fill_rect(r, 0, 0, VFB_W, VFB_H, 0, 0, 0, 170);
}

/* Clock editor popup: LEFT/RIGHT picks a field, UP/DOWN spins it,
   A/START saves, B cancels to the opening values. */
static int g_clk_snap[5];
static void clockpop_open(void) {
    rtc_clamp();
    g_clk_snap[0] = app.rtc_year;  g_clk_snap[1] = app.rtc_month;
    g_clk_snap[2] = app.rtc_day;   g_clk_snap[3] = app.rtc_hour;
    g_clk_snap[4] = app.rtc_minute;
    app.emu_clock_editing = 1;
    app.emu_clock_cursor  = 0;
}
static void clockpop_render(SDL_Renderer *r) {
    modal_scrim(r);

    int gap = VFB_W / 240; if (gap < 2) gap = 2;
    char f0[6], f2[3], f3[3], f4[3];
    snprintf(f0, sizeof(f0), "%d",   app.rtc_year);
    snprintf(f2, sizeof(f2), "%02d", app.rtc_day);
    snprintf(f3, sizeof(f3), "%02d", app.rtc_hour);
    snprintf(f4, sizeof(f4), "%02d", app.rtc_minute);
    const char *fs[5] = { f0, month_names[app.rtc_month - 1], f2, f3, f4 };

    int pad = 3 * gap;
    int bw[5];
    bw[0] = str_w("8888", MN_BIG) + 2 * pad;
    bw[1] = str_w("888",  MN_BIG) + 2 * pad;
    bw[2] = str_w("88",   MN_BIG) + 2 * pad;
    bw[3] = bw[2];
    bw[4] = bw[2];
    int bh = str_h(MN_BIG) + 2 * pad;
    int colon_w = str_w(":", MN_BIG) + 2 * gap;
    int sp = 3 * gap;
    int total_w = bw[0] + bw[1] + bw[2] + bw[3] + bw[4] + 3 * sp + colon_w;

    int title_h = str_h(MN_BIG) + 4 * gap;

    /* opaque panel - nothing from the page behind shows through */
    int pw = total_w + 10 * gap;
    int ph = title_h + bh + 10 * gap;
    int px = (VFB_W - pw) / 2;
    int py = CONTENT_Y + (FOOTER_Y - CONTENT_Y - ph) / 2;
    fill_rect(r, px - 2, py - 2, pw + 4, ph + 4, C_SEP);
    fill_rect(r, px, py, pw, ph, C_BG);

    int top = py + 5 * gap;
    draw_centered(r, tr("SET CLOCK"), px, top, pw, str_h(MN_BIG), MN_BIG, COL(ACC));

    int by = top + title_h;
    int x  = px + (pw - total_w) / 2;
    for (int fi = 0; fi < 5; fi++) {
        int sel = (fi == app.emu_clock_cursor);
        if (sel) fill_rect(r, x, by, bw[fi], bh, C_ACC);
        else     fill_rect(r, x, by, bw[fi], bh, C_HDR);
        draw_centered(r, fs[fi], x, by, bw[fi], bh, MN_BIG, sel ? COL(BG) : COL(DIM));
        x += bw[fi];
        if (fi == 3) {
            draw_centered(r, ":", x, by, colon_w, bh, MN_BIG, COL(DIM));
            x += colon_w;
        } else if (fi < 4) {
            x += sp;
        }
    }

    draw_footer(r, tr("B: CANCEL  A: SAVE"), tr("L/R: FIELD  U/D: CHANGE"));
}

static void menu_render_emulator(SDL_Renderer *r) {
    menu_bg(r);
    draw_header(r);

    int a_editable = (!app.emu_clock_editing &&
                      app.menu_item == EMU_CUSTOM_CLOCK && app.rtc_mode == 2);
    draw_footer(r, tr("B: BACK"), a_editable ? tr("A: EDIT") : NULL);

    const int RX = 18, RW = VFB_W - 36, RH = MENU_RH, RY = CONTENT_Y, RPAD = 16;

    const char *labels[EMU_COUNT] = {
        "AUTO RESUME",   "CUSTOM CLOCK",
        "FRAME SKIP",    "SKIP AMOUNT",   "SAFE SKIP",
        "CPU SPEED",     "3D THREADING",
        "UNZIP ROMS",    "FIX MAIN 2D",   "SLOT 2 DEVICE",
        "COMPRESS STATES", "BACKUP SAV",  "ROM HACK MODE",
    };

    char skip_val_str[4];
    snprintf(skip_val_str, sizeof(skip_val_str), "%d", app.frameskip_val);

    static const char *rtc_mode_names[] = { "OFF", "SYSTEM", "CUSTOM" };
    char clock_custom_str[24];
    snprintf(clock_custom_str, sizeof(clock_custom_str), "%d %s %02d %02d:%02d",
             app.rtc_year, month_names[app.rtc_month - 1],
             app.rtc_day, app.rtc_hour, app.rtc_minute);
    const char *clock_val = (app.rtc_mode == 2) ? clock_custom_str : rtc_mode_names[app.rtc_mode];

    const char *vals[EMU_COUNT] = {
        app.auto_resume      ? "ON"  : "OFF",
        clock_val,
        frameskip_type_names[app.frameskip],
        skip_val_str,
        app.safe_skip        ? "ON"  : "OFF",
        clock_names[app.clock_speed],
        app.threaded_3d      ? "ON"  : "OFF",
        app.unzip_roms       ? "ON"  : "OFF",
        app.fix_2d           ? "ON"  : "OFF",
        slot2_names[app.slot2_device],
        app.compress_states  ? "ON"  : "OFF",
        app.backup_sav       ? "ON"  : "OFF",
        app.rom_hack         ? "ON"  : "OFF",
    };
    int is_on[EMU_COUNT] = {
        app.auto_resume, app.rtc_mode > 0,
        app.frameskip > 0, 0, app.safe_skip,
        0, app.threaded_3d,
        app.unzip_roms, app.fix_2d, 0,
        app.compress_states, app.backup_sav, app.rom_hack,
    };

    emu_clamp_scroll();

    for (int vi = 0; vi < g_menu_vis && (app.emu_scroll + vi) < EMU_COUNT; vi++) {
        int i   = app.emu_scroll + vi;
        int iy  = RY + vi * RH;
        int sel = (i == app.menu_item);
        int greyed = (i == EMU_FRAMESKIP_VAL && app.frameskip != 2);

        draw_list_row(r, RX, iy, RW, RH, sel, vi > 0);

        SDL_Color lc = greyed ? (SDL_Color){C_SEP} : (sel ? COL(TXT) : COL(DIM));
        int ty = iy + (RH - str_h(MN_BIG)) / 2;
        draw_string(r, tr(labels[i]), RX + RPAD, ty, MN_BIG, lc);

        {
            SDL_Color vc = greyed ? (SDL_Color){C_SEP}
                         : (sel ? COL(TXT) : (is_on[i] ? COL(ACC) : COL(DIM)));

            int show_arrows = !greyed && !(i == EMU_CUSTOM_CLOCK && app.rtc_mode == 2);
            char vbuf[64];
            if (show_arrows)
                snprintf(vbuf, sizeof(vbuf), "< %s >", tr(vals[i]));
            else
                snprintf(vbuf, sizeof(vbuf), "%s", tr(vals[i]));
            int vw = str_w(vbuf, MN_BIG);
            draw_string(r, vbuf, RX + RW - vw - RPAD, ty, MN_BIG, vc);
        }
    }

    draw_scrollbar(r, RX + RW + 6, CONTENT_Y, g_menu_vis * MENU_RH, EMU_COUNT, g_menu_vis, app.emu_scroll);

    if (app.emu_clock_editing) clockpop_render(r);
}

static void emu_clamp_scroll(void) {
    int max_scroll = EMU_COUNT - g_menu_vis;
    if (max_scroll < 0) max_scroll = 0;
    if (app.emu_scroll < 0) app.emu_scroll = 0;
    if (app.emu_scroll > max_scroll) app.emu_scroll = max_scroll;
    if (app.menu_item < app.emu_scroll) app.emu_scroll = app.menu_item;
    if (app.menu_item >= app.emu_scroll + g_menu_vis)
        app.emu_scroll = app.menu_item - g_menu_vis + 1;
}

static void menu_input_emulator(int btn) {

    if (app.emu_clock_editing) {
        switch (btn) {
        case JOY_LEFT:  if (app.emu_clock_cursor > 0) app.emu_clock_cursor--; break;
        case JOY_RIGHT: if (app.emu_clock_cursor < 4) app.emu_clock_cursor++; break;
        case JOY_UP:
            switch (app.emu_clock_cursor) {
            case 0: app.rtc_year++;  if (app.rtc_year > 2099) app.rtc_year = 2000; break;
            case 1: app.rtc_month = app.rtc_month % 12 + 1; break;
            case 2: app.rtc_day   = app.rtc_day   % 31 + 1; break;
            case 3: app.rtc_hour  = (app.rtc_hour  + 1) % 24; break;
            case 4: app.rtc_minute= (app.rtc_minute+ 1) % 60; break;
            } break;
        case JOY_DOWN:
            switch (app.emu_clock_cursor) {
            case 0: app.rtc_year--;  if (app.rtc_year < 2000) app.rtc_year = 2099; break;
            case 1: app.rtc_month = (app.rtc_month - 2 + 12) % 12 + 1; break;
            case 2: app.rtc_day   = (app.rtc_day   - 2 + 31) % 31 + 1; break;
            case 3: app.rtc_hour  = (app.rtc_hour  + 23) % 24; break;
            case 4: app.rtc_minute= (app.rtc_minute + 59) % 60; break;
            } break;
        case JOY_B:
            /* cancel - restore the values the popup opened with */
            app.rtc_year   = g_clk_snap[0]; app.rtc_month  = g_clk_snap[1];
            app.rtc_day    = g_clk_snap[2]; app.rtc_hour   = g_clk_snap[3];
            app.rtc_minute = g_clk_snap[4];
            app.emu_clock_editing = 0;
            break;
        case JOY_A: case JOY_START:
            app.emu_clock_editing = 0;
            emu_config_save(); drastic_cfg_save_all(); break;
        default: break;
        }
        return;
    }

    int changed_drastic = 0;
    switch (btn) {
    case JOY_UP:
        app.menu_item = (app.menu_item - 1 + EMU_COUNT) % EMU_COUNT;
        emu_clamp_scroll(); return;
    case JOY_DOWN:
        app.menu_item = (app.menu_item + 1) % EMU_COUNT;
        emu_clamp_scroll(); return;
    case JOY_B:
        app.view = VIEW_OPTIONS;
        app.menu_item = OPT_EMULATOR; return;
    case JOY_A:
        if (app.menu_item == EMU_CUSTOM_CLOCK && app.rtc_mode == 2) {
            clockpop_open(); return;
        }

    case JOY_LEFT:
    case JOY_RIGHT: {
        int dir = (btn == JOY_LEFT) ? -1 : 1;
        switch (app.menu_item) {
        case EMU_AUTO_RESUME:
            app.auto_resume ^= 1; break;
        case EMU_UNZIP_ROMS:
            app.unzip_roms ^= 1; changed_drastic = 1; break;
        case EMU_FRAMESKIP:
            app.frameskip = (app.frameskip + dir + 3) % 3;
            changed_drastic = 1; break;
        case EMU_FRAMESKIP_VAL:
            if (app.frameskip == 2) {
                app.frameskip_val = (app.frameskip_val - 1 + dir + 9) % 9 + 1;
                changed_drastic = 1;
            }
            break;
        case EMU_SAFE_SKIP:
            app.safe_skip ^= 1; changed_drastic = 1; break;
        case EMU_CPU_SPEED:
            app.clock_speed = (app.clock_speed + dir + 4) % 4;
            changed_drastic = 1; break;
        case EMU_THREADED_3D:
            app.threaded_3d ^= 1; changed_drastic = 1; break;
        case EMU_FIX_2D:
            app.fix_2d ^= 1; changed_drastic = 1; break;
        case EMU_SLOT2:
            app.slot2_device = (app.slot2_device + dir + 4) % 4;
            changed_drastic = 1; break;
        case EMU_COMPRESS:
            app.compress_states ^= 1; changed_drastic = 1; break;
        case EMU_BACKUP_SAV:
            app.backup_sav ^= 1; changed_drastic = 1; break;
        case EMU_ROM_HACK:
            app.rom_hack ^= 1; changed_drastic = 1; break;
        case EMU_CUSTOM_CLOCK:

            app.rtc_mode = (app.rtc_mode + dir + 3) % 3;
            app.emu_clock_editing = 0;
            changed_drastic = 1; break;
        }
        break;
    }
    default: break;
    }
    emu_config_save();
    if (changed_drastic) drastic_cfg_save_all();
}


/* On-screen keyboard (player name). Closest-x row navigation with
   visual-center anchors on the action row; A=type B=cancel X=backspace
   Y=space START=save. Charset = the DS firmware name set. */
#define OSK_ROWS 5
static const char *osk_row_chars[OSK_ROWS - 1] = {
    "1234567890", "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-",
};
/* action row: SPACE / case / DEL / OK as keys (footer stays minimal) */
#define OSK_ACT_COUNT 4
static const int osk_act_x[OSK_ACT_COUNT] = { 2, 4, 6, 8 };
static struct {
    int  active;
    int  row, col;
    int  lower;                 /* case toggle (also SELECT) */
    char buf[NAME_MAX_LEN + 1];
    int  len;
} g_osk;

static char osk_ch(int row, int col) {
    char c = osk_row_chars[row][col];
    if (g_osk.lower && c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return c;
}

static int osk_row_len(int row) {
    if (row == OSK_ROWS - 1) return OSK_ACT_COUNT;
    return (int)strlen(osk_row_chars[row]);
}
/* visual x-center of a key in col units - row 3 draws centered (7 keys),
   so its nav anchors shift right to match what the eye sees */
static int osk_col_x(int row, int col) {
    if (row == OSK_ROWS - 1) return osk_act_x[col];
    if (row == 3) return col + 1;
    return col;
}
static int osk_nearest_col(int row, int x) {
    int best = 0, bd = 1 << 20;
    for (int c = 0; c < osk_row_len(row); c++) {
        int d = osk_col_x(row, c) - x; if (d < 0) d = -d;
        if (d < bd) { bd = d; best = c; }
    }
    return best;
}
static void osk_open(const char *initial) {
    memset(&g_osk, 0, sizeof(g_osk));
    snprintf(g_osk.buf, sizeof(g_osk.buf), "%s", initial ? initial : "");
    g_osk.len = (int)strlen(g_osk.buf);
    g_osk.active = 1;
    g_osk.row = 1; g_osk.col = 0;   /* start on 'A' */
}
static void osk_type(char ch) {
    if (g_osk.len < NAME_MAX_LEN) {
        g_osk.buf[g_osk.len++] = ch;
        g_osk.buf[g_osk.len] = '\0';
    }
}
static void osk_commit(void) {
    int l = g_osk.len;
    while (l > 0 && g_osk.buf[l - 1] == ' ') l--;
    g_osk.buf[l] = '\0';
    if (l == 0) snprintf(g_osk.buf, sizeof(g_osk.buf), "PLAYER");
    snprintf(app.fw_username, sizeof(app.fw_username), "%s", g_osk.buf);
    g_osk.active = 0;
    emu_config_save();
    drastic_cfg_save_all();
}
static void osk_input(int btn) {
    switch (btn) {
    case JOY_UP:
        if (g_osk.row > 0) {
            int x = osk_col_x(g_osk.row, g_osk.col);
            g_osk.row--; g_osk.col = osk_nearest_col(g_osk.row, x);
        }
        break;
    case JOY_DOWN:
        if (g_osk.row < OSK_ROWS - 1) {
            int x = osk_col_x(g_osk.row, g_osk.col);
            g_osk.row++; g_osk.col = osk_nearest_col(g_osk.row, x);
        }
        break;
    case JOY_LEFT:  if (g_osk.col > 0) g_osk.col--; break;
    case JOY_RIGHT: if (g_osk.col < osk_row_len(g_osk.row) - 1) g_osk.col++; break;
    case JOY_A:
        if (g_osk.row == OSK_ROWS - 1) {
            switch (g_osk.col) {
            case 0: osk_type(' ');                                  break;
            case 1: g_osk.lower ^= 1;                               break;
            case 2: if (g_osk.len > 0) g_osk.buf[--g_osk.len] = 0;  break;
            case 3: osk_commit();                                   break;
            }
        } else {
            osk_type(osk_ch(g_osk.row, g_osk.col));
        }
        break;
    case JOY_X:      if (g_osk.len > 0) g_osk.buf[--g_osk.len] = '\0'; break;
    case JOY_Y:      osk_type(' '); break;
    case JOY_SELECT: g_osk.lower ^= 1; break;
    case JOY_START:  osk_commit(); break;
    case JOY_B:      g_osk.active = 0; break;   /* cancel - discard edits */
    default: break;
    }
}
static void osk_render(SDL_Renderer *r) {
    modal_scrim(r);

    int gap = VFB_W / 240; if (gap < 2) gap = 2;
    int cw  = (VFB_W * 5 / 6 - 9 * gap) / 10;
    int kh  = cw * 3 / 4;
    int gw  = 10 * cw + 9 * gap;
    int gx  = (VFB_W - gw) / 2;

    int in_h  = kh;
    int in_w  = NAME_MAX_LEN * cw + (NAME_MAX_LEN - 1) * gap;
    int in_x  = (VFB_W - in_w) / 2;

    int title_h = str_h(MN_BIG) + 3 * gap;
    int total_h = title_h + in_h + 3 * gap + OSK_ROWS * kh + (OSK_ROWS - 1) * gap;
    int top = CONTENT_Y + (FOOTER_Y - CONTENT_Y - total_h) / 2;
    if (top < CONTENT_Y) top = CONTENT_Y;

    draw_centered(r, tr("PLAYER NAME"), 0, top, VFB_W, str_h(MN_BIG), MN_BIG, COL(ACC));

    int iy = top + title_h;
    for (int i = 0; i < NAME_MAX_LEN; i++) {
        int sx = in_x + i * (cw + gap);
        fill_rect(r, sx, iy, cw, in_h, C_HDR);
        if (i == g_osk.len) fill_rect(r, sx, iy + in_h - 3, cw, 3, C_ACC);
        else                fill_rect(r, sx, iy + in_h - 2, cw, 2, C_SEP);
        if (i < g_osk.len) {
            char cs[2] = { g_osk.buf[i], 0 };
            draw_centered(r, cs, sx, iy, cw, in_h, MN_BIG, COL(DIM));
        }
    }

    int ky = iy + in_h + 3 * gap;
    for (int row = 0; row < OSK_ROWS; row++) {
        int y = ky + row * (kh + gap);
        if (row < OSK_ROWS - 1) {
            int n   = osk_row_len(row);
            int rw2 = n * cw + (n - 1) * gap;
            int rx  = (VFB_W - rw2) / 2;
            for (int c = 0; c < n; c++) {
                int x   = rx + c * (cw + gap);
                int sel = (g_osk.row == row && g_osk.col == c);
                if (sel) fill_rect(r, x, y, cw, kh, C_ACC);
                else     fill_rect(r, x, y, cw, kh, C_HDR);
                char cs[2] = { osk_ch(row, c), 0 };
                draw_centered(r, cs, x, y, cw, kh, MN_BIG, sel ? COL(BG) : COL(DIM));
            }
        } else {
            /* SPACE(4) | case(2) | DEL(2) | OK(2) on the 10-column grid */
            const char *acts[OSK_ACT_COUNT] = {
                "SPACE", g_osk.lower ? "ABC" : "abc", "DEL", "OK",
            };
            const int aw[OSK_ACT_COUNT] = {
                4 * cw + 3 * gap, 2 * cw + gap, 2 * cw + gap, 2 * cw + gap,
            };
            int ax = gx;
            for (int c = 0; c < OSK_ACT_COUNT; c++) {
                int sel = (g_osk.row == row && g_osk.col == c);
                if (sel) fill_rect(r, ax, y, aw[c], kh, C_ACC);
                else     fill_rect(r, ax, y, aw[c], kh, C_HDR);
                draw_centered(r, acts[c], ax, y, aw[c], kh, MN_NRM,
                              sel ? COL(BG) : COL(DIM));
                ax += aw[c] + gap;
            }
        }
    }

    draw_footer(r, tr("B: CANCEL"), NULL);
}

static void menu_render_user(SDL_Renderer *r) {
    menu_bg(r);
    draw_header(r);

    int a_editable = !app.emu_bday_editing &&
                     (app.menu_item == USR_PLAYER_NAME || app.menu_item == USR_BIRTHDAY);
    if (app.emu_bday_editing)
        draw_footer(r, tr("B/A: DONE  L/R: MOVE FIELD"), tr("U/D: CHANGE"));
    else
        draw_footer(r, tr("B: BACK"), a_editable ? tr("A: EDIT") : NULL);

    const int RX = 18, RW = VFB_W - 36, RH = fit_row_h(USR_COUNT), RY = CONTENT_Y, RPAD = 16;

    char bday_str[8];
    snprintf(bday_str, sizeof(bday_str), "%s %02d",
             month_names[app.fw_bday_month - 1], app.fw_bday_day);

    const char *labels[USR_COUNT] = {
        "PLAYER NAME", "LANGUAGE", "FAVE COLOR", "BIRTHDAY",
    };
    const char *vals[USR_COUNT] = {
        app.fw_username,
        language_names[app.fw_language],
        fav_color_names[app.fw_fav_color],
        bday_str,
    };

    for (int i = 0; i < USR_COUNT; i++) {
        int iy  = RY + i * RH;
        int sel = (i == app.menu_item);

        draw_list_row(r, RX, iy, RW, RH, sel, i > 0);

        SDL_Color lc = sel ? COL(TXT) : COL(DIM);
        int ty = iy + (RH - str_h(MN_BIG)) / 2;
        draw_string(r, tr(labels[i]), RX + RPAD, ty, MN_BIG, lc);

        if (i == USR_BIRTHDAY && app.emu_bday_editing && sel) {
            char mstr[4], dstr[3];
            snprintf(mstr, sizeof(mstr), "%s", month_names[app.fw_bday_month - 1]);
            snprintf(dstr, sizeof(dstr), "%02d", app.fw_bday_day);
            int tw  = str_w(mstr, MN_NRM) + str_w(" ", MN_NRM) + str_w(dstr, MN_NRM);
            int cx  = RX + RW - tw - RPAD;
            int pty = iy + (RH - str_h(MN_NRM)) / 2;
            /* On the selected pill: active field = TXT, inactive = DIM
               (ACC would match SEL on light-pill themes like Leaf). */
            SDL_Color mc = (app.emu_bday_cursor == 0) ? COL(TXT) : COL(DIM);
            draw_string(r, mstr, cx, pty, MN_NRM, mc);
            if (app.emu_bday_cursor == 0)
                fill_rect(r, cx, pty + str_h(MN_NRM) + 2, str_w(mstr, MN_NRM), 2, C_BG);
            cx += str_w(mstr, MN_NRM);
            draw_string(r, " ", cx, pty, MN_NRM, COL(DIM));
            cx += str_w(" ", MN_NRM);
            SDL_Color dc = (app.emu_bday_cursor == 1) ? COL(TXT) : COL(DIM);
            draw_string(r, dstr, cx, pty, MN_NRM, dc);
            if (app.emu_bday_cursor == 1)
                fill_rect(r, cx, pty + str_h(MN_NRM) + 2, str_w(dstr, MN_NRM), 2, C_BG);
        }

        else {
            SDL_Color vc = sel ? COL(TXT) : COL(ACC);

            int show_arrows = (i == USR_LANGUAGE || i == USR_FAV_COLOR);
            char vbuf[64];
            if (show_arrows)
                snprintf(vbuf, sizeof(vbuf), "< %s >", tr(vals[i]));
            else
                snprintf(vbuf, sizeof(vbuf), "%s", tr(vals[i]));
            int vw = str_w(vbuf, MN_BIG);
            draw_string(r, vbuf, RX + RW - vw - RPAD, ty, MN_BIG, vc);
        }
    }

    if (g_osk.active) osk_render(r);
}

static void menu_input_user(int btn) {

    if (g_osk.active) { osk_input(btn); return; }

    if (app.emu_bday_editing) {
        switch (btn) {
        case JOY_LEFT:  app.emu_bday_cursor = 0; break;
        case JOY_RIGHT: app.emu_bday_cursor = 1; break;
        case JOY_UP:
            if (app.emu_bday_cursor == 0)
                app.fw_bday_month = app.fw_bday_month % 12 + 1;
            else
                app.fw_bday_day = app.fw_bday_day % 31 + 1;
            break;
        case JOY_DOWN:
            if (app.emu_bday_cursor == 0)
                app.fw_bday_month = (app.fw_bday_month - 2 + 12) % 12 + 1;
            else
                app.fw_bday_day = (app.fw_bday_day - 2 + 31) % 31 + 1;
            break;
        case JOY_B: case JOY_A: case JOY_START:
            app.emu_bday_editing = 0;
            emu_config_save(); drastic_cfg_save_all(); break;
        default: break;
        }
        return;
    }

    switch (btn) {
    case JOY_UP:
        app.menu_item = (app.menu_item - 1 + USR_COUNT) % USR_COUNT; break;
    case JOY_DOWN:
        app.menu_item = (app.menu_item + 1) % USR_COUNT; break;
    case JOY_B:
        app.view = VIEW_OPTIONS;
        app.menu_item = OPT_USER; break;
    case JOY_A:
        if (app.menu_item == USR_PLAYER_NAME) {
            osk_open(app.fw_username);
            return;
        }
        if (app.menu_item == USR_BIRTHDAY) {
            app.emu_bday_editing = 1; app.emu_bday_cursor = 0; return;
        }

    case JOY_LEFT:
    case JOY_RIGHT: {
        int dir = (btn == JOY_LEFT) ? -1 : 1;
        switch (app.menu_item) {
        case USR_PLAYER_NAME: break;
        case USR_LANGUAGE:
            app.fw_language = (app.fw_language + dir + 8) % 8;
            emu_config_save(); drastic_cfg_save_all(); break;
        case USR_FAV_COLOR:
            app.fw_fav_color = (app.fw_fav_color + dir + 16) % 16;
            emu_config_save(); drastic_cfg_save_all(); break;
        case USR_BIRTHDAY: break;
        }
        break;
    }
    default: break;
    }
}

static void menu_open(void) {
    if (app.in_menu) return;
    app.cursor_touch_down = 0;   /* pen up - don't hold a touch through the menu */
    /* PAD edge state is only maintained inside the menu loop; a button
       still down at the last menu close stays latched in PAD_curr and
       eats its own first press next session. Start every session clean. */
    PAD_curr = PAD_prev = 0;

    static int configs_loaded = 0;
    if (!configs_loaded) {
        configs_loaded = 1;
        drastic_cfg_load();
        emu_config_load();
        sc_config_load();
        ctrl_config_load();
        overlay_scan();
        theme_load_custom(app.drastic_dir);
        if (app.theme < 0 || app.theme >= theme_total()) app.theme = 0;
        theme_apply(app.theme);
        lang_scan();
        g_lang.sel = 0;
        for (int i = 0; i < g_lang.n_names; i++)
            if (strcmp(g_lang.names[i], app.lang_name) == 0) { g_lang.sel = i; break; }
        lang_load(app.lang_name);
        font_load_all(app.drastic_dir, g_custom_font);

        app.overlay_pack_idx = 0;
        for (int i = 0; i < app.overlay_pack_count; i++) {
            if (strcmp(app.overlay_packs[i], app.overlay_pack_name) == 0) {
                app.overlay_pack_idx = i + 1;
                break;
            }
        }

        if (app.overlay_pack_idx == 0) app.overlay_pack_name[0] = '\0';
    }
    capture_game_bg();
    preview_free();
    app.in_menu   = 1;
#ifdef NDS_EVDEV_PAD
    g_evp_menu_active = 1;   /* arm the menu-exit bleed suppressor */
#endif
    app.view      = VIEW_MAIN;
    app.menu_item = 0;

    PAD_curr = app.joy_held;
    PAD_prev = app.joy_held;
}

static void menu_close(void) {
    app.in_menu = 0;
    emu_config_save();
    vfb_clear();
}

static void resolve_real(void) {
    real_SDL_Init                    = dlsym(RTLD_NEXT, "SDL_Init");
    real_SDL_CreateWindow            = dlsym(RTLD_NEXT, "SDL_CreateWindow");
    real_SDL_CreateWindowAndRenderer = dlsym(RTLD_NEXT, "SDL_CreateWindowAndRenderer");
    real_SDL_SetWindowSize           = dlsym(RTLD_NEXT, "SDL_SetWindowSize");
    real_SDL_CreateRenderer          = dlsym(RTLD_NEXT, "SDL_CreateRenderer");
    real_SDL_RenderClear             = dlsym(RTLD_NEXT, "SDL_RenderClear");
    real_SDL_RenderPresent           = dlsym(RTLD_NEXT, "SDL_RenderPresent");
    real_SDL_DestroyRenderer         = dlsym(RTLD_NEXT, "SDL_DestroyRenderer");
    real_SDL_DestroyWindow           = dlsym(RTLD_NEXT, "SDL_DestroyWindow");
    real_SDL_CloseAudio              = dlsym(RTLD_NEXT, "SDL_CloseAudio");
    real_SDL_PauseAudio              = dlsym(RTLD_NEXT, "SDL_PauseAudio");
    real_SDL_OpenAudio               = dlsym(RTLD_NEXT, "SDL_OpenAudio");
    real_SDL_Quit                    = dlsym(RTLD_NEXT, "SDL_Quit");
    real_SDL_CreateTexture           = dlsym(RTLD_NEXT, "SDL_CreateTexture");
    real_SDL_DestroyTexture          = dlsym(RTLD_NEXT, "SDL_DestroyTexture");
    real_SDL_RenderCopy              = dlsym(RTLD_NEXT, "SDL_RenderCopy");
    real_SDL_RenderCopyEx            = dlsym(RTLD_NEXT, "SDL_RenderCopyEx");
    real_SDL_SetRenderTarget         = dlsym(RTLD_NEXT, "SDL_SetRenderTarget");
    real_SDL_RenderSetLogicalSize    = dlsym(RTLD_NEXT, "SDL_RenderSetLogicalSize");
    real_SDL_PollEvent               = dlsym(RTLD_NEXT, "SDL_PollEvent");
    real_SDL_Delay                   = dlsym(RTLD_NEXT, "SDL_Delay");
    real_SDL_GetTicks                = dlsym(RTLD_NEXT, "SDL_GetTicks");
    real__libc_start_main            = dlsym(RTLD_NEXT, "__libc_start_main");
    real__snprintf_chk               = dlsym(RTLD_NEXT, "__snprintf_chk");
    real__sprintf_chk                = dlsym(RTLD_NEXT, "__sprintf_chk");
    real_exit                        = dlsym(RTLD_NEXT, "exit");
    real__exit                       = dlsym(RTLD_NEXT, "_exit");
    real_system                      = dlsym(RTLD_NEXT, "system");
    fn_SetTextureScaleMode           = (sdl_set_scale_mode_t)dlsym(RTLD_NEXT, "SDL_SetTextureScaleMode");

    app.base = find_exe_base();
#ifdef DRASTIC_ARM32
    arm32_patch_audio();
#endif

    config_init_defaults();
    ensure_dirs();
    emu_config_load();

}

static inline void hook(void) { pthread_once(&resolve_once, resolve_real); }

static void setup_virtual_fb(SDL_Renderer *r) {

    if (app.virtual_fb) {
        SDL_DestroyTexture(app.virtual_fb);
        app.virtual_fb = NULL;
    }

    g_rot_dst = (SDL_Rect){ 0, 0, VFB_W, VFB_H };

    if (ROT_ANGLE != 0.0) {

        g_rot_dst = (SDL_Rect) ROT_DST_INIT;
    }
    app.virtual_fb = real_SDL_CreateTexture(r,
        SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, VFB_W, VFB_H);

    SDL_SetTextureBlendMode(app.virtual_fb, SDL_BLENDMODE_NONE);

    real_SDL_SetRenderTarget(r, app.virtual_fb);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    real_SDL_RenderClear(r);

    overlay_reload();
}

int SDL_Init(Uint32 flags) {
    hook();
    return real_SDL_Init(flags);
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) {
    hook();
    int ret = real_SDL_OpenAudio(desired, obtained);
    if (ret == 0) return 0;
    real_SDL_Delay(50);
    return real_SDL_OpenAudio(desired, obtained);
}

static void detect_display_size(void) {
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
        int dw = (mode.w > mode.h) ? mode.w : mode.h;
        int dh = (mode.w > mode.h) ? mode.h : mode.w;
        if (dw >= 320 && dh >= 240) {
            g_vfb_w = dw;
            g_vfb_h = dh;
            if (ROT_ANGLE == 0.0) {
                g_rot_dst.x = 0; g_rot_dst.y = 0;
                g_rot_dst.w = dw; g_rot_dst.h = dh;
            }
        }
    }
    compute_layouts();
}

SDL_Window *SDL_CreateWindow(const char *title, int x, int y,
                             int w, int h, Uint32 flags) {
    hook();
    detect_display_size();
#ifdef FUN_FORCE_FULLSCREEN
    /* some compositors only grant input focus to fullscreen toplevels */
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
    app.window = real_SDL_CreateWindow(title, x, y, g_vfb_w, g_vfb_h, flags);
    return app.window;
}

void SDL_SetWindowSize(SDL_Window *win, int w, int h) {
    (void)win; (void)w; (void)h;
}

int SDL_CreateWindowAndRenderer(int w, int h, Uint32 flags,
                                SDL_Window **ow, SDL_Renderer **or_) {
    hook(); (void)w; (void)h;
    detect_display_size();
    int ret = real_SDL_CreateWindowAndRenderer(g_vfb_w, g_vfb_h, flags, ow, or_);
    if (ret == 0) {
        app.window   = *ow;
        app.renderer = *or_;
        setup_virtual_fb(app.renderer);
    }
    return ret;
}

SDL_Renderer *SDL_CreateRenderer(SDL_Window *win, int index, Uint32 flags) {
    hook();
#ifdef NDS_NO_VSYNC
    /* the emulator paces by audio, not vblank - PRESENTVSYNC only adds a
       blocking wait to every present */
    if (flags & SDL_RENDERER_PRESENTVSYNC) {
        flags &= ~(Uint32)SDL_RENDERER_PRESENTVSYNC;
        fprintf(stderr, "[fun] video: PRESENTVSYNC stripped (NDS_NO_VSYNC)\n");
    }
#endif
    app.renderer = real_SDL_CreateRenderer(win, index, flags);
#ifdef NDS_NO_VSYNC
    if (app.renderer) {
        /* runtime disable too - covers vsync the driver defaulted ON */
        int (*set_vsync)(SDL_Renderer *, int) =
            (int (*)(SDL_Renderer *, int))dlsym(RTLD_NEXT, "SDL_RenderSetVSync");
        if (!set_vsync)
            set_vsync = (int (*)(SDL_Renderer *, int))dlsym(RTLD_DEFAULT, "SDL_RenderSetVSync");
        if (set_vsync) set_vsync(app.renderer, 0);
    }
#endif
    if (app.renderer) setup_virtual_fb(app.renderer);
    return app.renderer;
}

int SDL_RenderSetLogicalSize(SDL_Renderer *ren, int w, int h) {
    (void)ren; (void)w; (void)h;
    return 0;
}

int SDL_RenderClear(SDL_Renderer *ren) {
    if (app.in_menu) return 0;
    real_SDL_SetRenderTarget(ren, app.virtual_fb);
    return real_SDL_RenderClear(ren);
}

/* One vfb clear per game frame at the first screen blit (re-armed by the
   present hook). The game repaints only its screen rects, so hook draws
   that overhang them (cursor, FF tag) would otherwise trail on the borders. */
static int g_vfb_frame_cleared = 0;

int SDL_RenderCopy(SDL_Renderer *ren, SDL_Texture *tex,
                   const SDL_Rect *src, const SDL_Rect *dst) {
    if (app.in_menu) return 0;
    real_SDL_SetRenderTarget(ren, app.virtual_fb);

    if (tex == app.screens[0] || tex == app.screens[1]) {
        if (!g_vfb_frame_cleared) {
            g_vfb_frame_cleared = 1;
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            real_SDL_RenderClear(ren);
        }
        int which = (tex == app.screens[1]) ? 1 : 0;
        int slot  = app.swap_screens ? (1 - which) : which;
        screen_slot_t pip_slots[2];
        const screen_slot_t *s;
        if (app.layout == 0) {
            get_pip_slots(app.swap_screens, app.pip_corner, app.pixel_perfect, pip_slots);
            s = &pip_slots[which];
        } else {
            const screen_slot_t (*slots)[2] = app.pixel_perfect ? layout_slots_pp : layout_slots;
            s = &slots[app.layout][slot];
        }

        if (which == 1 && dst)
            app.drastic_touch_rect = *dst;

        if (s->rect.w == 0 || s->rect.h == 0) return 0;

        apply_scale_mode(tex);

        int is_pip = (app.layout == 0) && (s->alpha < 255);
        if (is_pip) {
            app.pip_deferred_tex   = tex;
            app.pip_deferred_rect  = s->rect;
            app.pip_deferred_alpha = s->alpha;
            return 0;
        }

        int rc = real_SDL_RenderCopy(ren, tex, NULL, &s->rect);
        if (app.pip_deferred_tex) {
            if (app.layout == 0) {
                SDL_SetTextureAlphaMod(app.pip_deferred_tex, app.pip_deferred_alpha);
                SDL_SetTextureBlendMode(app.pip_deferred_tex, SDL_BLENDMODE_BLEND);
                real_SDL_RenderCopy(ren, app.pip_deferred_tex, NULL, &app.pip_deferred_rect);
                SDL_SetTextureAlphaMod(app.pip_deferred_tex, 255);
                SDL_SetTextureBlendMode(app.pip_deferred_tex, SDL_BLENDMODE_NONE);
            }
            app.pip_deferred_tex = NULL;
        }
        return rc;
    }

    if (dst && app.drastic_touch_rect.w > 0) {
        const SDL_Rect *dr = &app.drastic_touch_rect;
        screen_slot_t pip_slots_cur[2];
        const SDL_Rect *our;
        if (app.layout == 0) {
            get_pip_slots(app.swap_screens, app.pip_corner, app.pixel_perfect, pip_slots_cur);
            our = &pip_slots_cur[1].rect;
        } else {
            int ts = app.swap_screens ? 0 : 1;
            const screen_slot_t (*slots)[2] = app.pixel_perfect ? layout_slots_pp : layout_slots;
            our = &slots[app.layout][ts].rect;
        }

        if (our->w == 0 || our->h == 0) {
            if (!app.swap_screens) return 0;
            int gs = 0;
            our = &layout_slots[app.layout][gs].rect;
            if (our->w == 0 || our->h == 0) return 0;
        }

        SDL_Rect remapped = {
            our->x + (dst->x - dr->x) * our->w / dr->w,
            our->y + (dst->y - dr->y) * our->h / dr->h,
            dst->w * our->w / dr->w,
            dst->h * our->h / dr->h,
        };

        int rx2 = remapped.x + remapped.w;
        int ry2 = remapped.y + remapped.h;
        int ox2 = our->x + our->w;
        int oy2 = our->y + our->h;
        if (remapped.x < our->x) remapped.x = our->x;
        if (remapped.y < our->y) remapped.y = our->y;
        if (rx2 > ox2) rx2 = ox2;
        if (ry2 > oy2) ry2 = oy2;
        remapped.w = rx2 - remapped.x;
        remapped.h = ry2 - remapped.y;

        if (remapped.w <= 0 || remapped.h <= 0) return 0;

        return real_SDL_RenderCopy(ren, tex, src, &remapped);
    }
    return real_SDL_RenderCopy(ren, tex, src, dst);
}


#ifdef NDS_PACING_VALVE
/* Pacing valve. The scheduler only self-heals frame debt past 66.7ms; when
   a 3s window is chronically late (>=1/3 of presents), fire its own resync
   at target = elapsed - 18000 (measured recenter). Runs in the present call. */
static void nds_pacing_valve(void) {
    static uint32_t win_start = 0;
    static int      n_tot = 0, n_beh = 0;
    if (app.in_menu || app.quitting || app.ff_speed ||
        app.cheat_restore_pending || app.ff_restore_pending || !app.base) {
        win_start = 0; n_tot = n_beh = 0;
        return;
    }
    uint8_t *sys = (uint8_t *)drastic_system();
    if (!sys) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);   /* drastic's clock source, not SDL's */
    uint64_t now3     = ((uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec) * 3ull;
    uint64_t base3    = *(volatile uint64_t *)(sys + NDS_SYNC_BASELINE_OFF);
    uint64_t elapsed3 = now3 - base3;
    uint64_t target3  = *(volatile uint64_t *)(sys + NDS_SYNC_TARGET_OFF);
    int32_t  delta3   = (int32_t)((uint32_t)target3 - (uint32_t)elapsed3);
    uint32_t nowms    = real_SDL_GetTicks ? real_SDL_GetTicks() : 0;
    if (!win_start) { win_start = nowms ? nowms : 1; n_tot = n_beh = 0; }
    n_tot++;
    if (delta3 < -9000) n_beh++;           /* BEHIND_THRESH: 3ms late */
    if (nowms - win_start < 3000) return;  /* 3s observation window */
    if (n_tot >= 60 && n_beh * 3 >= n_tot) {
        *(volatile uint64_t *)(sys + NDS_SYNC_TARGET_OFF) = elapsed3 - 18000;
        if (fun_evlog())
            fprintf(stderr, "[fun] pacing valve: resync (%d/%d presents "
                    "behind, delta %d us)\n", n_beh, n_tot, delta3 / 3);
    }
    win_start = 0; n_tot = n_beh = 0;
}
#endif /* NDS_PACING_VALVE */

void SDL_RenderPresent(SDL_Renderer *ren) {
#ifdef NDS_PACING_VALVE
    nds_pacing_valve();
#endif
#ifdef NDS_NO_VSYNC
    /* With vsync stripped nothing bounds fast-forward. Govern emulated
       time directly: the frame-sync target advances 50000 (16.67ms * 3)
       per frame, so hold it to FUN_FF_SPEED x real time (default 2x). */
    if (app.ff_speed > 0 && !app.in_menu && app.base) {   /* app.base==0 on ARM32: no sync offsets */
        uint8_t *sysf = (uint8_t *)drastic_system();
        if (sysf) {
            static uint64_t ff_t0 = 0, ff_v0 = 0, ff_last_rt = 0;
            static uint64_t ff_mult = 0;
            if (!ff_mult) {
                const char *fs = getenv("FUN_FF_SPEED");
                ff_mult = fs ? (uint64_t)atoi(fs) : 2;
                if (ff_mult < 2 || ff_mult > 8) ff_mult = 2;
            }
            uint64_t v = *(volatile uint64_t *)(sysf + NDS_SYNC_TARGET_OFF);
            struct timeval ftv;
            gettimeofday(&ftv, NULL);
            uint64_t rt = (uint64_t)ftv.tv_sec * 1000000u + ftv.tv_usec;
            /* re-anchor on FF start, after a pause, or a target reset */
            if (rt - ff_last_rt > 500000 || v < ff_v0) { ff_t0 = rt; ff_v0 = v; }
            ff_last_rt = rt;
            uint64_t emu_us  = (v - ff_v0) / 3;
            uint64_t real_us = rt - ff_t0;
            if (emu_us > real_us * ff_mult + 20000) {
                uint32_t ms = (uint32_t)((emu_us - real_us * ff_mult) /
                                         (1000 * ff_mult));
                if (ms > 50) ms = 50;
                if (ms) real_SDL_Delay(ms);
            }
        }
    }
#endif
    app.last_render_ticks = SDL_GetTicks();

    if (app.cursor_mode && !app.cursor_tex)
        cursor_tex_load(ren);

    if (app.cheat_apply_pending && !app.in_menu && !app.cheat_restore_pending) {
        app.cheat_apply_pending = 0;
        cheat_apply();
    }

    if (app.quitting) {
        present_black(ren);
        return;
    }

    if (app.prepare_frames > 0) {
        app.prepare_frames--;
        if (app.prepare_frames == 0) {
#if defined(NDS_RESUME_WAIT_3D) && !defined(DRASTIC_ARM32)
            /* Boot resume defers until the geometry FIFO quartet holds live
               pointers (a load while the 3D engine is cold can crash);
               time-capped for games that never touch 3D. Mid-session
               restores are untouched; audio keeps running throughout. */
            if (app.auto_resume && !app.ff_restore_pending &&
                !app.cheat_restore_pending) {
                int ready = 0;
                uintptr_t v0 = 0, v2 = 0;
                uint8_t *s3 = (uint8_t *)drastic_system();
                if (s3) {
                    uintptr_t lo3 = (uintptr_t)s3, hi3 = lo3 + 0x4000000;
                    volatile uintptr_t *q3 =
                        (volatile uintptr_t *)(s3 + 0x8000 + 6760);
                    v0 = q3[0]; v2 = q3[2];
                    ready = (v0 >= lo3 && v0 < hi3 &&
                             v2 >= lo3 && v2 < hi3);
                }
                static int rwait_cap_ms = -1;
                if (rwait_cap_ms < 0) {
                    const char *wc = getenv("NDS_RESUME_WAIT_MS");
                    rwait_cap_ms = wc ? atoi(wc) : 1500;
                    if (rwait_cap_ms < 0 || rwait_cap_ms > 10000)
                        rwait_cap_ms = 1500;
                }
                static uint32_t rwait_start = 0;
                uint32_t rnow = real_SDL_GetTicks ? real_SDL_GetTicks() : 0;
                if (!rwait_start) {
                    rwait_start = rnow ? rnow : 1;
                    fprintf(stderr, "[fun] resume: fifo quartet %#lx/%#lx "
                            "sys=%p ready=%d (cap %d ms)\n",
                            (unsigned long)v0, (unsigned long)v2,
                            (void *)s3, ready, rwait_cap_ms);
                }
                /* black hold - the boot intro must never show before a
                   resume (the countdown branch paints the same clear) */
                if (!ready && (rnow - rwait_start) < (uint32_t)rwait_cap_ms) {
                    app.prepare_frames = 1;   /* re-check next present */
                    present_black(ren);
                    return;
                }
                if (rnow - rwait_start > 60)
                    fprintf(stderr, "[fun] resume: %s after %u ms - loading\n",
                            ready ? "3D engine ready" : "wait cap reached",
                            (unsigned)(rnow - rwait_start));
            }
#endif
            if (app.auto_resume || app.ff_restore_pending || app.cheat_restore_pending) {

                int skip = 0;
                char spath[MAX_PATH];
                skip_resume_path(spath, sizeof(spath));
                if (access(spath, F_OK) == 0) { skip = 1; unlink(spath); }

                if (app.cheat_restore_pending) {
                    app.cheat_restore_pending = 0;
                    app.last_render_ticks = SDL_GetTicks();
                    drastic_load_state(CHEAT_TEMP_SLOT);
                } else if (app.ff_restore_pending) {
                    app.ff_restore_pending = 0;
                    app.last_render_ticks = SDL_GetTicks();
                    drastic_load_state(FF_TEMP_SLOT);
                } else if (!skip) {
                    char arpath[MAX_PATH];
                    state_path(arpath, sizeof(arpath), app.state_slot);
                    struct stat arst;
                    if (stat(arpath, &arst) == 0) {
                        app.last_render_ticks = SDL_GetTicks();
                        drastic_load_state(app.state_slot);
                    }
                }
            }
        } else {
            /* pre-load hold: screen stays black until the state lands */
            present_black(ren);
            return;
        }
    }

    {
    /* Two cursor modes, one position (d-pad TOUCH CURSOR + stick auto-show):
     * the same nx/ny is drawn here and written on tap, so they can't disagree. */
    int cursor_stick_live = app.cursor_stick_seen &&
        (app.cursor_touch_down ||
         (real_SDL_GetTicks() - app.cursor_move_ms) < CURSOR_SHOW_MS);
    if ((app.cursor_mode || cursor_stick_live) && !app.in_menu) {
        if (app.cursor_mode && (app.cursor_hdx || app.cursor_hdy)) {
            float cspd = g_cursor_speed_mult[app.cursor_speed];
            app.cursor_nx += app.cursor_hdx * 2.0f * cspd;
            app.cursor_ny += app.cursor_hdy * 2.0f * cspd;
        }
        if (app.cursor_stick_seen) {
            static uint32_t _cur_last_ms = 0;
            uint32_t _cur_now = real_SDL_GetTicks();
            float dt = (_cur_last_ms && _cur_now > _cur_last_ms)
                       ? (_cur_now - _cur_last_ms) / 1000.0f : 0.0f;
            if (dt > 0.1f) dt = 0.1f;
            _cur_last_ms = _cur_now;
            {
                /* Normalize each axis by its learned per-direction range,
                 * then one radial deadzone + squared curve on the magnitude
                 * (full tilt = full speed). Per-axis silent-rail park. */
                int x_alive = (_cur_now - g_cur_live_x_ms) < CURSOR_SILENCE_MS;
                int y_alive = (_cur_now - g_cur_live_y_ms) < CURSOR_SILENCE_MS;
                float ex = x_alive ? (float)app.cursor_ax : 0.0f;
                float ey = y_alive ? (float)app.cursor_ay : 0.0f;
                int moving = 0;
                float fx = ex / ((ex < 0) ? g_cur_max_xn : g_cur_max_xp);
                float fy = ey / ((ey < 0) ? g_cur_max_yn : g_cur_max_yp);
                if (fx >  1.0f) fx =  1.0f;
                if (fx < -1.0f) fx = -1.0f;
                if (fy >  1.0f) fy =  1.0f;
                if (fy < -1.0f) fy = -1.0f;
                float m = sqrtf(fx * fx + fy * fy);
                if (m > 1.0f) { fx /= m; fy /= m; m = 1.0f; }
                if (m > CURSOR_DEADZONE_FRAC) {
                    float t = (m - CURSOR_DEADZONE_FRAC) / (1.0f - CURSOR_DEADZONE_FRAC);
                    float v = t * t * CURSOR_STICK_SPEED *
                              g_cursor_speed_mult[app.cursor_speed] * dt / m;
                    app.cursor_nx += fx * v;
                    app.cursor_ny += fy * v;
                    moving = 1;
                }
                if (moving) app.cursor_move_ms = _cur_now;
            }
        }
        if (app.cursor_nx < 0.0f)   app.cursor_nx = 0.0f;
        if (app.cursor_nx > 255.0f) app.cursor_nx = 255.0f;
        if (app.cursor_ny < 0.0f)   app.cursor_ny = 0.0f;
        if (app.cursor_ny > 191.0f) app.cursor_ny = 191.0f;

        if (app.cursor_tex) {
            SDL_Rect tr = cursor_touch_rect();
            if (tr.w > 0 && tr.h > 0) {
                int cx = tr.x + (int)(app.cursor_nx * (tr.w - 1) / 255.0f);
                int cy = tr.y + (int)(app.cursor_ny * (tr.h - 1) / 191.0f);
                int tw, th;
                SDL_QueryTexture(app.cursor_tex, NULL, NULL, &tw, &th);
                float scale = (float)VFB_H / 480.0f;
                int csz_w = (int)(tw * scale);
                int csz_h = (int)(th * scale);
                if (csz_w < 4) csz_w = 4;
                if (csz_h < 4) csz_h = 4;
                SDL_Rect cdst = { cx, cy, csz_w, csz_h };
                real_SDL_SetRenderTarget(ren, app.virtual_fb);
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                SDL_SetTextureBlendMode(app.cursor_tex, SDL_BLENDMODE_BLEND);
                real_SDL_RenderCopy(ren, app.cursor_tex, NULL, &cdst);
                real_SDL_SetRenderTarget(ren, NULL);
            }
        } else {
            real_SDL_SetRenderTarget(ren, app.virtual_fb);
            const char *clbl = tr("CURSOR");
            int clw = str_w(clbl, MN_BIG), clh = str_h(MN_BIG), clx, cly;
            switch (app.pip_corner) {
                default:
                case 0: clx = VFB_W - clw - PIP_PAD; cly = PIP_PAD;                break;
                case 1: clx = VFB_W - clw - PIP_PAD; cly = VFB_H - clh - PIP_PAD; break;
                case 2: clx = PIP_PAD;                cly = VFB_H - clh - PIP_PAD; break;
                case 3: clx = PIP_PAD;                cly = PIP_PAD;               break;
            }
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
            SDL_Rect cbg = {clx - 6, cly - 4, clw + 12, clh + 8};
            SDL_RenderFillRect(ren, &cbg);
            draw_string(ren, clbl, clx, cly, MN_BIG, COL(ACC));
            real_SDL_SetRenderTarget(ren, NULL);
        }
    }
    }

    /* end-of-frame touch write - paired with the PollEvent-side call so
     * no internal drastic stage can stomp the pen unnoticed */
    nds_touch_enforce();

    if (app.ff_speed > 0 && !app.ff_restore_pending) {
        real_SDL_SetRenderTarget(ren, app.virtual_fb);
        const char *lbl = ">>FF";
        int lw = str_w(lbl, MN_BIG);
        int lh = str_h(MN_BIG);
        int opp = (app.pip_corner + 2) % 4;
        int lx, ly;
        switch (opp) {
            default:
            case 0: lx = VFB_W - lw - PIP_PAD; ly = PIP_PAD;               break;
            case 1: lx = VFB_W - lw - PIP_PAD; ly = VFB_H - lh - PIP_PAD; break;
            case 2: lx = PIP_PAD;               ly = VFB_H - lh - PIP_PAD; break;
            case 3: lx = PIP_PAD;               ly = PIP_PAD;              break;
        }
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
        SDL_Rect ffbg = {lx - 6, ly - 4, lw + 12, lh + 8};
        SDL_RenderFillRect(ren, &ffbg);
        draw_string(ren, lbl, lx, ly, MN_BIG, COL(ACC));
        real_SDL_SetRenderTarget(ren, NULL);
    }

    update_overlay(ren);

    real_SDL_SetRenderTarget(ren, NULL);
    real_SDL_RenderClear(ren);
    real_SDL_RenderCopyEx(ren, app.virtual_fb, NULL, &ROT_DST,
                          ROT_ANGLE, NULL, SDL_FLIP_NONE);

    if (app.overlay_tex && !app.in_menu) {
        real_SDL_RenderCopy(ren, app.overlay_tex, NULL, &ROT_DST);
    }

    real_SDL_RenderPresent(ren);
    g_vfb_frame_cleared = 0;   /* next game frame clears the vfb once */

    real_SDL_SetRenderTarget(ren, app.virtual_fb);
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *ren, Uint32 fmt,
                               int access, int w, int h) {
    SDL_Texture *t = real_SDL_CreateTexture(ren, fmt, access, w, h);
    if (access == SDL_TEXTUREACCESS_STREAMING && w == DS_W && h == DS_H) {
        if      (!app.screens[0]) app.screens[0] = t;
        else if (!app.screens[1]) app.screens[1] = t;
    }
    return t;
}

void SDL_DestroyTexture(SDL_Texture *tex) {
    if (tex == app.screens[0])      app.screens[0] = NULL;
    else if (tex == app.screens[1]) app.screens[1] = NULL;
    real_SDL_DestroyTexture(tex);
}

void SDL_Delay(uint32_t ms) {
    if (app.ff_speed > 0) return;
    if (ms > 16) ms = 16;
    real_SDL_Delay(ms);
}

/* One dispatch point for menu navigation - used by the nav loop and by
   the wrap-undo below. */
static void menu_nav_dispatch(int b) {
    switch (app.view) {
    case VIEW_MAIN:      menu_input_main(b);      break;
    case VIEW_OPTIONS:   menu_input_options(b);   break;
    case VIEW_LAYOUT:    menu_input_layout(b);    break;
    case VIEW_SHORTCUTS: menu_input_shortcuts(b); break;
    case VIEW_CONTROLS:     menu_input_controls(b);     break;
    case VIEW_AUDIOVISUAL:  menu_input_audiovisual(b);  break;
    case VIEW_EMULATOR:     menu_input_emulator(b);     break;
    case VIEW_USER:         menu_input_user(b);         break;
    case VIEW_OVERLAY:      menu_input_overlay(b);      break;
    case VIEW_CHEATS:       menu_input_cheats(b);       break;
    }
}

uint32_t SDL_GetTicks(void) {
    static uint32_t offset       = 0;
    static uint32_t menu_enter   = 0;
    uint32_t now = real_SDL_GetTicks();
    if (app.in_menu) {
        if (!menu_enter) menu_enter = now;
        return now - offset - (now - menu_enter);
    }
    if (menu_enter) {
        offset    += now - menu_enter;
        menu_enter = 0;
    }
    return now - offset;
}

int SDL_PollEvent(SDL_Event *event) {

    if (!app.in_menu) {
        void *sys = drastic_system();
        if (sys) {
            uint32_t *bs = (uint32_t *)((uint8_t *)sys + NDS_INPUT_OFFSET + 0x10);
            if (app.ff_speed > 0)
                *bs |=  NDS_KEY_BIT_FAST;
            else
                *bs &= ~NDS_KEY_BIT_FAST;
        }
    }
    nds_touch_enforce();
#ifdef NDS_EVDEV_PAD
    nds_evpad_update();   /* raw-evdev pad -> menu + DraStic button_status */
    /* no SDL axis events on this image: drive the touch cursor from the evdev
       left stick (read in nds_evpad_poll), mirroring the AXIS_LX/LY handler. */
    if (!app.in_menu && !app.quitting) {
        uint32_t _st = real_SDL_GetTicks ? real_SDL_GetTicks() : 0;
        Sint16 vx = (Sint16)g_evp_lsx, vy = (Sint16)g_evp_lsy;
        app.cursor_ax = vx; g_cur_live_x_ms = _st;
        if      (vx > 0 &&  (float)vx > g_cur_max_xp) g_cur_max_xp =  (float)vx;
        else if (vx < 0 && -(float)vx > g_cur_max_xn) g_cur_max_xn = -(float)vx;
        app.cursor_ay = vy; g_cur_live_y_ms = _st;
        if      (vy > 0 &&  (float)vy > g_cur_max_yp) g_cur_max_yp =  (float)vy;
        else if (vy < 0 && -(float)vy > g_cur_max_yn) g_cur_max_yn = -(float)vy;
        float _mx = vx < 0 ? -(float)vx : (float)vx;
        float _my = vy < 0 ? -(float)vy : (float)vy;
        float _mag = _mx > _my ? _mx : _my;
        if (_mag > (app.cursor_stick_seen ? (CURSOR_DEADZONE_FRAC * CURSOR_RANGE_FLOOR)
                                          : (float)CURSOR_STICK_DETECT)) {
            app.cursor_stick_seen = 1;
            app.cursor_move_ms    = _st;
            if (!app.cursor_tex) cursor_tex_load(app.renderer);
        }
    }
#endif
    /* Fake mic: hold the bound trigger -> feed DraStic's mic from the WAV. */
    mic_inject(!app.in_menu && mic_trigger_held());
#ifndef DRASTIC_ARM32
    /* post-load poison watch: drastic polls at FRAME START, before the
       geometry pass - the right moment to catch late FIFO poison */
    if (g_nds_fifo_watch > 0) {
        g_nds_fifo_watch--;
        nds_fifo_sanitize("poll-watch");
    }
#endif
    hook();
    int result;
repoll:
    /* consumed events jump back here to fetch the next event - ending
       the poll per consumed event would starve the emulator's loop */
    result = real_SDL_PollEvent(event);
#ifdef NDS_EVDEV_PAD
    if (!result && !app.in_menu) result = nds_evpad_next_event(event);
#endif
    if (result) evlog_event(event, "main");

    if (result && event->type == SDL_JOYBUTTONDOWN)
        app.joy_held |=  (1u << event->jbutton.button);
    if (result && event->type == SDL_JOYBUTTONUP)
        app.joy_held &= ~(1u << event->jbutton.button);

    /* L2/R2 analog-trigger state for the fake mic (they can arrive as an axis
       rather than a button on this hardware). Not consumed - let DraStic see it. */
    if (result && event->type == SDL_JOYAXISMOTION) {
#if defined(AXIS_L2_TRIG) && AXIS_L2_TRIG >= 0
        if (event->jaxis.axis == AXIS_L2_TRIG)
            g_l2_axis_held = event->jaxis.value > TRIG_THRESHOLD;
#endif
#if defined(AXIS_R2_TRIG) && AXIS_R2_TRIG >= 0
        if (event->jaxis.axis == AXIS_R2_TRIG)
            g_r2_axis_held = event->jaxis.value > TRIG_THRESHOLD;
#endif
    }

    if (result && event->type == SDL_JOYHATMOTION && event->jhat.hat != 0)
        goto repoll;
    if (result && event->type == SDL_JOYHATMOTION && event->jhat.hat == 0) {
        uint8_t _hv = (uint8_t)event->jhat.value;
        app.joy_held &= ~((1u<<HAT_UP_BIT)|(1u<<HAT_DOWN_BIT)|(1u<<HAT_LEFT_BIT)|(1u<<HAT_RIGHT_BIT));
        if (_hv & SDL_HAT_UP)    app.joy_held |= (1u<<HAT_UP_BIT);
        if (_hv & SDL_HAT_DOWN)  app.joy_held |= (1u<<HAT_DOWN_BIT);
        if (_hv & SDL_HAT_LEFT)  app.joy_held |= (1u<<HAT_LEFT_BIT);
        if (_hv & SDL_HAT_RIGHT) app.joy_held |= (1u<<HAT_RIGHT_BIT);
#if JOY_UP == HAT_UP_BIT

        {
            static uint8_t _prev_hv = 0;
            uint8_t _changed = _hv ^ _prev_hv;
            static const struct { uint8_t bit; uint8_t vbtn; } _dirs[4] = {
                {SDL_HAT_UP,    HAT_UP_BIT},
                {SDL_HAT_DOWN,  HAT_DOWN_BIT},
                {SDL_HAT_LEFT,  HAT_LEFT_BIT},
                {SDL_HAT_RIGHT, HAT_RIGHT_BIT},
            };
            for (int _d = 0; _d < 4; _d++) {
                if (!(_changed & _dirs[_d].bit)) continue;
                SDL_Event _syn;
                SDL_memset(&_syn, 0, sizeof(_syn));
                _syn.type             = (_hv & _dirs[_d].bit) ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
                _syn.jbutton.which    = event->jhat.which;
                _syn.jbutton.button   = _dirs[_d].vbtn;
                _syn.jbutton.state    = (_hv & _dirs[_d].bit) ? SDL_PRESSED : SDL_RELEASED;
                SDL_PushEvent(&_syn);
            }
            _prev_hv = _hv;
        }

        goto repoll;
#endif
    }

    static uint32_t menu_press_time  = 0;
    static int      menu_combo_fired = 0;

#ifdef FUN_KB_INPUT
    /* Keyboard input layer: mirror mapped keys onto joy_held, own the Menu
       key (tap opens this menu), and turn Menu-held combos into synthetic
       pad events for the shortcut machinery. Other keys pass through. */
    if (result && (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) &&
        !event->key.repeat) {
        int kb_btn = kb_to_btn((int)event->key.keysym.sym);
        if (kb_btn >= 0) {
            int kb_down = (event->type == SDL_KEYDOWN);
            if (kb_down) app.joy_held |=  (1u << kb_btn);
            else         app.joy_held &= ~(1u << kb_btn);

            if (kb_btn == JOY_MENU) {
                if (!app.in_menu) {
                    if (kb_down) {
                        menu_press_time  = SDL_GetTicks();
                        menu_combo_fired = 0;
                    } else {
                        if (!menu_combo_fired &&
                            (SDL_GetTicks() - menu_press_time) < MENU_TAP_MS)
                            menu_open();
                        menu_combo_fired = 0;
                    }
                }
                SDL_memset(event, 0, sizeof(*event));
                result = 0;
            } else if (app.joy_held & (1u << JOY_MENU)) {
                SDL_memset(event, 0, sizeof(*event));
                event->type           = kb_down ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
                event->jbutton.button = (Uint8)kb_btn;
                event->jbutton.state  = kb_down ? SDL_PRESSED : SDL_RELEASED;
            }
        }
    }
#endif

    if (!app.in_menu) {
        if (result && event->type == SDL_JOYBUTTONDOWN &&
            event->jbutton.button == JOY_MENU) {
            menu_press_time  = SDL_GetTicks();
            menu_combo_fired = 0;
            SDL_memset(event, 0, sizeof(*event));
            result = 0;
        }
        if (result && event->type == SDL_JOYBUTTONUP &&
            event->jbutton.button == JOY_MENU) {
            if (!menu_combo_fired &&
                (SDL_GetTicks() - menu_press_time) < MENU_TAP_MS) {
                menu_open();
            }
            menu_combo_fired = 0;
            SDL_memset(event, 0, sizeof(*event));
            result = 0;
        }
    }

    if (app.in_menu) {
        while (app.in_menu) {
            PAD_poll();

            /* JOY_MENU is intentionally ignored while the menu is open */

            static const int nav[] = {
                JOY_UP, JOY_DOWN, JOY_LEFT, JOY_RIGHT,
                JOY_A, JOY_B, JOY_X, JOY_L1, JOY_R1, JOY_START, JOY_SELECT
            };
            /* hold-to-repeat on directions; action buttons tap-only */
            #define NAV_REPEAT_DELAY 300
            #define NAV_REPEAT_MS     85
            int rep_delay = g_osk.active ? 220 : NAV_REPEAT_DELAY;
            int rep_ms    = g_osk.active ?  55 : NAV_REPEAT_MS;
            static int      nav_rep_btn = -1;
            static uint32_t nav_rep_at  = 0;
            if (nav_rep_btn >= 0 && !PAD_isPressed(nav_rep_btn))
                nav_rep_btn = -1;
            for (int ni = 0; ni < (int)(sizeof(nav)/sizeof(nav[0])); ni++) {
                int b = nav[ni];
                int nav_fire = 0;
                if (PAD_justPressed(b)) {
                    nav_fire = 1;
                    int is_dir = (b == JOY_UP || b == JOY_DOWN ||
                                  b == JOY_LEFT || b == JOY_RIGHT);
                    nav_rep_btn = is_dir ? b : -1;
                    /* real_ ticks: the SDL_GetTicks interposer freezes
                       time while in_menu */
                    nav_rep_at  = real_SDL_GetTicks() + rep_delay;
                } else if (b == nav_rep_btn && PAD_isPressed(b)) {
                    uint32_t _nnow = real_SDL_GetTicks();
                    if ((int32_t)(_nnow - nav_rep_at) >= 0) {
                        nav_fire = 1;
                        nav_rep_at = _nnow + rep_ms;
                    }
                }
                if (nav_fire) {
                    int was_repeat = !PAD_justPressed(b);
                    int prev_item  = app.menu_item;
                    menu_nav_dispatch(b);
                    /* a held direction stops at the list edge; a fresh
                       press still wraps. Wrap shows as the cursor jumping
                       the wrong way - undo via the opposite step. */
                    if (was_repeat && (b == JOY_UP || b == JOY_DOWN)) {
                        int wrapped = (b == JOY_UP   && app.menu_item > prev_item) ||
                                      (b == JOY_DOWN && app.menu_item < prev_item);
                        if (wrapped)
                            menu_nav_dispatch(b == JOY_UP ? JOY_DOWN : JOY_UP);
                    }
                    break;
                }
            }

            if (!app.in_menu) break;
            if (app.quitting) break;

            real_SDL_SetRenderTarget(app.renderer, app.virtual_fb);
            switch (app.view) {
            case VIEW_MAIN:      menu_render_main(app.renderer);      break;
            case VIEW_OPTIONS:   menu_render_options(app.renderer);   break;
            case VIEW_LAYOUT:    menu_render_layout(app.renderer);    break;
            case VIEW_SHORTCUTS: menu_render_shortcuts(app.renderer); break;
            case VIEW_CONTROLS:     menu_render_controls(app.renderer);     break;
            case VIEW_AUDIOVISUAL:  menu_render_audiovisual(app.renderer);  break;
            case VIEW_EMULATOR:     menu_render_emulator(app.renderer);     break;
            case VIEW_USER:         menu_render_user(app.renderer);         break;
            case VIEW_OVERLAY:      menu_render_overlay(app.renderer);      break;
            case VIEW_CHEATS:       menu_render_cheats(app.renderer);       break;
            }
            {
                real_SDL_SetRenderTarget(app.renderer, NULL);
                real_SDL_RenderClear(app.renderer);
                real_SDL_RenderCopyEx(app.renderer, app.virtual_fb, NULL, &ROT_DST,
                                      ROT_ANGLE, NULL, SDL_FLIP_NONE);
                real_SDL_RenderPresent(app.renderer);
            }
            real_SDL_Delay(16);
        }

        real_SDL_SetRenderTarget(app.renderer, app.virtual_fb);
        SDL_memset(event, 0, sizeof(*event));
        return 0;
    }

#if defined(AXIS_LX) && defined(AXIS_LY)
    /* Left stick = touch cursor. Raw axis events are consumed so the emulator
     * never sees LX/LY (its internal stick-cursor would engage). */
    if (result && !app.in_menu && event->type == SDL_JOYAXISMOTION &&
        (event->jaxis.axis == AXIS_LX || event->jaxis.axis == AXIS_LY)) {
        Sint16 v = NDS_AXV(event->jaxis.value);
        {
            uint32_t _t = real_SDL_GetTicks();
            if (event->jaxis.axis == AXIS_LX) {
                app.cursor_ax = v; g_cur_live_x_ms = _t;
                if      (v > 0 &&  (float)v > g_cur_max_xp) g_cur_max_xp =  (float)v;
                else if (v < 0 && -(float)v > g_cur_max_xn) g_cur_max_xn = -(float)v;
            } else {
                app.cursor_ay = v; g_cur_live_y_ms = _t;
                if      (v > 0 &&  (float)v > g_cur_max_yp) g_cur_max_yp =  (float)v;
                else if (v < 0 && -(float)v > g_cur_max_yn) g_cur_max_yn = -(float)v;
            }
        }
        {
            float mag = (v < 0) ? -(float)v : (float)v;
            if (mag > (app.cursor_stick_seen ? (CURSOR_DEADZONE_FRAC * CURSOR_RANGE_FLOOR)
                                             : (float)CURSOR_STICK_DETECT)) {
                app.cursor_stick_seen = 1;
                app.cursor_move_ms    = real_SDL_GetTicks();
                if (!app.cursor_tex) cursor_tex_load(app.renderer);
            }
        }
        goto repoll;
    }
#endif

#if CURSOR_PRESS_BTN >= 0
    /* Stick click (L3) = pen down at the hook cursor. Consumed so drastic's
     * touch_cursor_press can't fire (it would tap at its own cursor, not ours). */
    if (result && !app.in_menu &&
        (event->type == SDL_JOYBUTTONDOWN || event->type == SDL_JOYBUTTONUP) &&
        event->jbutton.button == CURSOR_PRESS_BTN) {
        int pen_down = (event->type == SDL_JOYBUTTONDOWN);
        app.cursor_touch_down = pen_down;
        if (pen_down) {
            app.cursor_move_ms = real_SDL_GetTicks();
            app.cursor_stick_seen = 1;   /* stick click implies a stick */
            if (!app.cursor_tex) cursor_tex_load(app.renderer);
        }
        goto repoll;
    }
#endif

    if (result && event->type == SDL_JOYBUTTONDOWN) {
        int phys      = event->jbutton.button;
        int menu_held = (app.joy_held & (1u << JOY_MENU)) != 0;
        int fired     = 0;

        if (menu_held && fun_evlog()) {
            fprintf(stderr, "[fun] combo probe: btn=%d bindings", phys);
            for (int i = 0; i < SC_COUNT; i++)
                fprintf(stderr, " %d/%d", app.sc_btn[i], app.sc_mod[i]);
            fprintf(stderr, "\n");
        }

        for (int i = 0; i < SC_COUNT && !fired; i++) {
            int sb = app.sc_btn[i];
            int sm = app.sc_mod[i];
            if (sb < 0 || phys != sb) continue;
            if (sm == 1 && !menu_held) continue;
            if (sm == 0 &&  menu_held) continue;

            SDL_memset(event, 0, sizeof(*event));
            result = 0;
            menu_combo_fired = 1;
            fired = 1;

            switch (i) {
            case SC_SAVE_STATE:
                drastic_save_state(app.state_slot);
                preview_save(app.state_slot);
                break;
            case SC_LOAD_STATE:
                app.ff_speed = 0;
                emu_config_save();
                ff_apply();
                drastic_load_state(app.load_slot);
                break;
            case SC_FAST_FORWARD:
                app.ff_speed ^= 1;
                emu_config_save();
                ff_reset_apply();
                break;
            case SC_SWAP_SCREENS:
                app.swap_screens ^= 1;
                emu_config_save();
                break;
            case SC_NEXT_LAYOUT:
                app.layout = (app.layout + 1) % LAYOUT_COUNT;
                overlay_reload();
                emu_config_save();
                vfb_clear();
                break;
            case SC_PREV_LAYOUT:
                app.layout = (app.layout - 1 + LAYOUT_COUNT) % LAYOUT_COUNT;
                overlay_reload();
                emu_config_save();
                vfb_clear();
                break;
            case SC_CURSOR:
                app.cursor_mode ^= 1;
                app.cursor_hdx = 0; app.cursor_hdy = 0;
                if (app.cursor_mode) {
                    cursor_tex_load(app.renderer);
                }
                if (!app.cursor_mode) {
                    app.cursor_touch_down = 0;   /* pen up */
                    vfb_clear();
                }
                emu_config_save();
                break;
            }
        }

        if (!fired && menu_held)
            menu_combo_fired = 1;
    }

    if (app.cursor_mode && !app.in_menu && result &&
        (event->type == SDL_JOYBUTTONDOWN || event->type == SDL_JOYBUTTONUP)) {
        int btn  = event->jbutton.button;
        int down = (event->type == SDL_JOYBUTTONDOWN);

        int is_dpad = (btn == JOY_UP || btn == JOY_DOWN ||
                       btn == JOY_LEFT || btn == JOY_RIGHT);
        int is_a = (btn == JOY_A);

        int is_x = (btn == JOY_X);

        if (is_dpad) {
            /* Hook-owned pen: track held directions only. Position integrates
             * per-frame in the present hook; taps land via the direct touch write. */
            if (btn == JOY_LEFT)  app.cursor_hdx = down ? -1 : (app.cursor_hdx == -1 ? 0 : app.cursor_hdx);
            if (btn == JOY_RIGHT) app.cursor_hdx = down ?  1 : (app.cursor_hdx ==  1 ? 0 : app.cursor_hdx);
            if (btn == JOY_UP)    app.cursor_hdy = down ? -1 : (app.cursor_hdy == -1 ? 0 : app.cursor_hdy);
            if (btn == JOY_DOWN)  app.cursor_hdy = down ?  1 : (app.cursor_hdy ==  1 ? 0 : app.cursor_hdy);

            goto repoll;
        }
        if (is_x && down) {
            app.cursor_nx = 128.0f;
            app.cursor_ny = 92.0f;
            goto repoll;
        }
        if (is_a) {
            /* A = pen down/up at the hook cursor (direct input_struct write). */
            app.cursor_touch_down = down;
            goto repoll;
        }
    }

    if (result && (event->type == SDL_JOYBUTTONDOWN || event->type == SDL_JOYBUTTONUP)) {
        if (event->jbutton.button != JOY_MENU)
            event->jbutton.button = (Uint8)remap_button(event->jbutton.button);
    }

    return result;
}

int __snprintf_chk(char *s, size_t maxlen, int flag, size_t slen,
                   const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (fmt && strcmp(fmt, "%s%cbackup%c%s.dsv") == 0) {
        int r = real__snprintf_chk(s, maxlen, flag, slen,
                    "%s/%s/%s.sram", sdcard_path(), SAVES_DIR, app.rom_name);
        va_end(ap); return r;
    }
    int r = vsnprintf(s, maxlen, fmt, ap);
    va_end(ap); return r;
}

int __sprintf_chk(char *s, int flag, size_t slen, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (fmt && strcmp(fmt, "%s%csystem%c%s") == 0) {
        va_arg(ap, const char*); va_arg(ap, int); va_arg(ap, int);
        const char *bios = va_arg(ap, const char*);
        va_end(ap);
        if (bios && strncmp(bios, "nds_", 4) == 0)
            return real__sprintf_chk(s, flag, slen,
                       "%s/bios/%s", sdcard_path(), bios);
        va_start(ap, fmt);
    }
    int r = vsnprintf(s, slen, fmt, ap);
    va_end(ap); return r;
}

static drastic_main_t drastic_real_main = NULL;
static char **g_argv = NULL;

static void *boot_watchdog(void *arg) {
    (void)arg;
    real_SDL_Delay(500);
    while (1) {
        real_SDL_Delay(1000);
        if (app.quitting) return NULL;
        if (app.in_menu || app.ff_restore_pending || app.cheat_restore_pending) continue;
        if (app.ff_speed > 0 || app.plat_busy > 0) continue;
        uint32_t now  = SDL_GetTicks();
        uint32_t last = app.last_render_ticks;
        int hung = (last == 0) ? ((now - app.launch_time) > 2000)
                               : ((now - last) > 2000);
        if (!hung) continue;
        static int retried = 0;
        if (!retried) {
            retried = 1;
            /* keep the launcher's LD_PRELOAD if set - a wrong guess here
               relaunches the emulator without the hook */
            const char *cur_preload = getenv("LD_PRELOAD");
            if (!cur_preload || !cur_preload[0]) {
                char preload[MAX_PATH];
                snprintf(preload, sizeof(preload), "%s/lib/libfundrastic.so", app.drastic_dir);
                setenv("LD_PRELOAD", preload, 1);
            }
            execv(g_argv[0], g_argv);
        }
        real_exit(1);
    }
    return NULL;
}

static int hook_main(int argc, char **argv, char **envp) {
    hook();
    /* Data home: the binary's own directory by default; ports whose
       binary sits in bin/ export FUN_DRASTIC_DIR to name it explicitly. */
    const char *fdir = getenv("FUN_DRASTIC_DIR");
    if (fdir && fdir[0]) {
        strncpy(app.drastic_dir, fdir, MAX_PATH - 1);
    } else if (argc >= 1 && argv[0] && argv[0][0]) {
        strncpy(app.drastic_dir, argv[0], MAX_PATH - 1);
        char *slash = strrchr(app.drastic_dir, '/');
        if (slash) *slash = '\0';
        else app.drastic_dir[0] = '\0';
    }
    if (app.drastic_dir[0]) {
        emu_config_load();

        {
            char cfg[MAX_PATH];
            snprintf(cfg, sizeof(cfg), "%s/config/drastic.cfg", app.drastic_dir);
            drastic_cfg_set(cfg, "fast_forward", "0");
        }
        mic_apply();
    }
    if (argc >= 2 && argv[1] && argv[1][0]) {
        strncpy(app.rom_path, argv[1], MAX_PATH - 1);
        rom_name_from_path(app.rom_path);
        app.prepare_frames = 30;
        app.launch_time = SDL_GetTicks();
        g_argv = argv;

        {
            pthread_t wdog;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            pthread_create(&wdog, &attr, boot_watchdog, NULL);
            pthread_attr_destroy(&attr);
        }
        return drastic_real_main(argc, argv, envp);
    }
    return 1;
}

int __libc_start_main(int (*main)(int, char **, char **), int argc, char **ubp_av,
                      void (*init)(void), void (*fini)(void),
                      void (*rtld_fini)(void), void *stack_end) {
    hook();
    drastic_real_main = main;
    return real__libc_start_main(hook_main, argc, ubp_av,
                                 init, fini, rtld_fini, stack_end);
}

static void sdl_teardown(void) {

    if (real_SDL_PauseAudio) real_SDL_PauseAudio(1);
    if (real_SDL_CloseAudio) real_SDL_CloseAudio();

    if (app.overlay_tex && real_SDL_DestroyTexture) { real_SDL_DestroyTexture(app.overlay_tex); app.overlay_tex = NULL; }
    if (app.virtual_fb  && real_SDL_DestroyTexture) { real_SDL_DestroyTexture(app.virtual_fb);  app.virtual_fb  = NULL; }
    if (app.renderer && real_SDL_DestroyRenderer)   { real_SDL_DestroyRenderer(app.renderer);   app.renderer    = NULL; }
    if (app.window   && real_SDL_DestroyWindow)     { real_SDL_DestroyWindow(app.window);        app.window      = NULL; }
    if (real_SDL_Quit) real_SDL_Quit();
}

__attribute__((noreturn)) void exit(int status) {
    hook();
    sdl_teardown();
    real_exit(status);
    __builtin_unreachable();
}

__attribute__((noreturn)) void _exit(int status) {
    hook();
    sdl_teardown();
    real__exit(status);
    __builtin_unreachable();
}

int system(const char *cmd) {
    unsetenv("LD_PRELOAD");
    return real_system(cmd);
}

__attribute__((constructor))
static void lib_init(void) {
    app.preview_slot = -1;
    compute_layouts();
}
