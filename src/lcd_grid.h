#ifndef FUN_LCD_GRID_H
#define FUN_LCD_GRID_H

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Private implementation included by funhook.c so the hook still builds
   from a single translation unit. */
struct lcd_grid_mask {
    SDL_Texture *texture;
    int width, height;
};

static struct {
    bool enabled;
    int native_width, native_height;
    struct lcd_grid_mask masks[2];
} lcd_grid;

static void lcd_grid_init(int native_width, int native_height) {
    const char *shader = getenv("FUN_DRASTIC_SHADER");
    lcd_grid.enabled = shader && (!strcmp(shader, "lcd3x") || !strcmp(shader, "lcd-grid"));
    lcd_grid.native_width = native_width;
    lcd_grid.native_height = native_height;
}

static bool lcd_grid_is_enabled(void) {
    return lcd_grid.enabled;
}

static void lcd_grid_clear_cache(void) {
    for (int screen = 0; screen < 2; screen++) {
        struct lcd_grid_mask *mask = &lcd_grid.masks[screen];
        if (mask->texture) SDL_DestroyTexture(mask->texture);
        *mask = (struct lcd_grid_mask){0};
    }
}

/* Cache the native-pixel pattern at the destination size. Each frame
   multiplies the game image by the mask with a single SDL blit. */
static void draw_lcd_grid(SDL_Renderer *renderer, const SDL_Rect *rect, int screen,
                          int (*render_copy)(SDL_Renderer *, SDL_Texture *,
                                             const SDL_Rect *, const SDL_Rect *)) {
    if (!lcd_grid_is_enabled() || rect->w <= 0 || rect->h <= 0) return;

    int width = rect->w, height = rect->h;
    struct lcd_grid_mask *mask = &lcd_grid.masks[screen];
    if (mask->width != width || mask->height != height) {
        if (mask->texture) SDL_DestroyTexture(mask->texture);
        *mask = (struct lcd_grid_mask){.width = width, .height = height};

        uint32_t *pixels = malloc((size_t)width * height * sizeof(*pixels));
        if (!pixels) return;

        int native_width = lcd_grid.native_width;
        int native_height = lcd_grid.native_height;
        for (int y = 0; y < height; y++) {
            bool row = (2 * y + 1) * native_height % (2 * height) <
                       SDL_min(height, 2 * native_height);
            for (int x = 0; x < width; x++) {
                bool column = (2 * x + 1) * native_width % (2 * width) <
                              SDL_min(width, 2 * native_width);
                uint32_t gray = row && column ? 98 : (row || column ? 158 : 255);
                pixels[(size_t)y * width + x] = gray * 0x01010100u | 0xffu;
            }
        }

        SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                                 SDL_TEXTUREACCESS_STATIC, width, height);
        if (texture && SDL_UpdateTexture(texture, NULL, pixels, width * sizeof(*pixels)) == 0 &&
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_MOD) == 0) {
            mask->texture = texture;
        } else if (texture) {
            SDL_DestroyTexture(texture);
        }
        free(pixels);
    }
    if (mask->texture) render_copy(renderer, mask->texture, NULL, rect);
}

#endif
