/*
 * Renders every page of the sample status for each display type into PPM images, approximating how the firmware
 * draws them (OLED: mono with inverted title rows; LCD: character cells; TFT: colored text). Usage:
 *   ui_preview <output-dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "growbox_ui.h"
#include "ui_sample.h"

typedef enum { LOOK_OLED, LOOK_LCD, LOOK_TFT } look_t;

typedef struct {
    const char *name;
    look_t look;
    uint16_t width, height; /* pixels of the panel (character LCD: computed) */
    uint8_t scale;          /* font scale for TFT */
    uint8_t cols, rows;     /* character LCD grid */
} preview_t;

static const preview_t k_previews[] = {
    {"oled_128x64", LOOK_OLED, 128, 64, 1, 0, 0},
    {"oled_128x32", LOOK_OLED, 128, 32, 1, 0, 0},
    {"lcd_1602", LOOK_LCD, 0, 0, 1, 16, 2},
    {"lcd_2004", LOOK_LCD, 0, 0, 1, 20, 4},
    {"tft_240x320", LOOK_TFT, 240, 320, 2, 0, 0},
    {"tft_320x240", LOOK_TFT, 320, 240, 2, 0, 0},
    {"tft_240x240", LOOK_TFT, 240, 240, 2, 0, 0},
};

typedef struct {
    int w, h;
    uint8_t *px; /* RGB */
} image_t;

static void put(image_t *img, int x, int y, uint32_t rgb)
{
    if (x < 0 || y < 0 || x >= img->w || y >= img->h) {
        return;
    }
    uint8_t *p = img->px + 3 * (y * img->w + x);
    p[0] = rgb >> 16;
    p[1] = rgb >> 8;
    p[2] = rgb;
}

static void fill(image_t *img, int x, int y, int w, int h, uint32_t rgb)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            put(img, x + i, y + j, rgb);
        }
    }
}

/* Same colors as the TFT driver. */
static uint32_t tft_color(uint8_t style)
{
    switch (style) {
    case GB_UI_STYLE_TITLE:
        return 0x00E0FF;
    case GB_UI_STYLE_GOOD:
        return 0x40FF40;
    case GB_UI_STYLE_WARN:
        return 0xFF4040;
    case GB_UI_STYLE_DIM:
        return 0x808080;
    default:
        return 0xFFFFFF;
    }
}

static void draw_glyph(image_t *img, int x0, int y0, char c, int scale, uint32_t fg, uint32_t bg, int cell_w,
                       int cell_h)
{
    fill(img, x0, y0, cell_w * scale, cell_h * scale, bg);
    const uint8_t *g = gb_font_glyph(c);
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (g[col] & (1 << row)) {
                fill(img, x0 + col * scale, y0 + row * scale, scale, scale, fg);
            }
        }
    }
}

static void render_preview(const preview_t *pv, const char *dir)
{
    gb_ui_status_t st = ui_sample_status();
    uint8_t cols, rows;
    int panel_w, panel_h;
    if (pv->look == LOOK_LCD) {
        cols = pv->cols;
        rows = pv->rows;
        panel_w = 4 + cols * 6;
        panel_h = 4 + rows * 9;
    } else {
        cols = (uint8_t)(pv->width / (6 * pv->scale));
        rows = (uint8_t)(pv->height / (8 * pv->scale));
        panel_w = pv->width;
        panel_h = pv->height;
    }
    uint8_t pages = gb_ui_page_count(&st, cols, rows);
    const int gap = 6;
    image_t img = {.w = pages * (panel_w + gap) + gap, .h = panel_h + 2 * gap};
    img.px = calloc((size_t)img.w * img.h, 3);
    fill(&img, 0, 0, img.w, img.h, 0x303030);

    static gb_ui_screen_t s;
    for (uint8_t p = 0; p < pages; p++) {
        gb_ui_render(&st, p, cols, rows, &s);
        int ox = gap + p * (panel_w + gap), oy = gap;
        if (pv->look == LOOK_OLED) {
            fill(&img, ox, oy, panel_w, panel_h, 0x000000);
            for (int r = 0; r < rows; r++) {
                bool inv = s.style[r] == GB_UI_STYLE_TITLE || s.style[r] == GB_UI_STYLE_WARN;
                uint32_t fg = inv ? 0x000000 : 0xE0F0FF, bg = inv ? 0xE0F0FF : 0x000000;
                fill(&img, ox, oy + r * 8, panel_w, 8, bg);
                for (int c = 0; c < cols; c++) {
                    draw_glyph(&img, ox + c * 6, oy + r * 8, s.text[r][c], 1, fg, bg, 6, 8);
                }
            }
        } else if (pv->look == LOOK_LCD) {
            fill(&img, ox, oy, panel_w, panel_h, 0x1030C0);
            for (int r = 0; r < rows; r++) {
                for (int c = 0; c < cols; c++) {
                    int x = ox + 2 + c * 6, y = oy + 2 + r * 9;
                    fill(&img, x, y, 5, 8, 0x2040D0); /* unlit cell */
                    const uint8_t *g = gb_font_glyph(s.text[r][c]);
                    for (int col = 0; col < 5; col++) {
                        for (int row = 0; row < 7; row++) {
                            if (g[col] & (1 << row)) {
                                put(&img, x + col, y + row, 0xF0F0FF);
                            }
                        }
                    }
                }
            }
        } else {
            fill(&img, ox, oy, panel_w, panel_h, 0x000000);
            for (int r = 0; r < rows; r++) {
                for (int c = 0; c < cols; c++) {
                    draw_glyph(&img, ox + c * 6 * pv->scale, oy + r * 8 * pv->scale, s.text[r][c], pv->scale,
                               tft_color(s.style[r]), 0x000000, 6, 8);
                }
            }
        }
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, pv->name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
    fwrite(img.px, 3, (size_t)img.w * img.h, f);
    fclose(f);
    free(img.px);
    printf("%s: %ux%u chars, %u page(s)\n", path, cols, rows, pages);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    for (size_t i = 0; i < sizeof(k_previews) / sizeof(k_previews[0]); i++) {
        render_preview(&k_previews[i], dir);
    }
    return 0;
}
