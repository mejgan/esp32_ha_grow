/*
 * Display pages as text grids, independent of the display hardware: every driver shows a cols x rows grid of
 * characters (OLED/TFT draw them with gb_font5x7, character LCDs use their own ROM font). No ESP-IDF dependencies,
 * so layouts are unit-tested and previewed on the host (see test/host).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "growbox_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Degree sign inside screen text; every driver maps it to its own glyph. */
#define GB_UI_DEG '\x7f'

#define GB_UI_MAX_COLS 40
#define GB_UI_MAX_ROWS 30

typedef enum {
    GB_UI_STYLE_NORMAL,
    GB_UI_STYLE_TITLE,
    GB_UI_STYLE_GOOD,
    GB_UI_STYLE_WARN,
    GB_UI_STYLE_DIM,
} gb_ui_style_t;

typedef struct {
    bool enabled;
    bool light_on;
    uint8_t level_pct; /* output brightness while on, 1..100 */
    bool auto_mode;
    uint16_t on_min, off_min; /* schedule, minutes of the day */
    uint8_t auto_brightness_pct;
    uint8_t max_temp_c;
    bool heat_lock;
    bool sensor_fault; /* a sensor of this floor gives no reading */
    bool has_temp, has_humidity, has_soil, has_pressure, has_lux;
    bool temp_valid, rh_valid, soil_valid, pressure_valid, lux_valid;
    float temp_c, rh, soil_pct, pressure_hpa, lux;
} gb_ui_floor_t;

/* Optional room sensor next to the controller. */
typedef struct {
    bool present;
    bool has_pressure;
    bool temp_valid, rh_valid, pressure_valid;
    float temp_c, rh, pressure_hpa;
} gb_ui_room_t;

typedef struct {
    gb_ui_floor_t floor[GB_FLOOR_COUNT];
    gb_ui_room_t room;
    bool joined;
    bool time_valid;
    uint16_t minute_of_day;
    uint8_t reset_countdown_s; /* > 0 while the button is held for a factory reset */
} gb_ui_status_t;

typedef struct {
    uint8_t cols, rows;
    char text[GB_UI_MAX_ROWS][GB_UI_MAX_COLS + 1]; /* every row space-padded to exactly `cols` */
    uint8_t style[GB_UI_MAX_ROWS];                 /* gb_ui_style_t per row */
} gb_ui_screen_t;

/* Number of pages for this status on a cols x rows grid (at least 1). */
uint8_t gb_ui_page_count(const gb_ui_status_t *st, uint8_t cols, uint8_t rows);

/* Renders page `page` (taken modulo the page count). cols/rows are clamped to GB_UI_MAX_COLS/GB_UI_MAX_ROWS. */
void gb_ui_render(const gb_ui_status_t *st, uint8_t page, uint8_t cols, uint8_t rows, gb_ui_screen_t *out);

/* 5x7 font for 0x20..0x7F, one byte per column, bit 0 = top row. 0x7F is the degree sign (GB_UI_DEG). */
#define GB_FONT_FIRST 0x20
#define GB_FONT_LAST  0x7F
extern const uint8_t gb_font5x7[GB_FONT_LAST - GB_FONT_FIRST + 1][5];

/* Glyph columns for `c`; characters outside the font render as '?'. */
const uint8_t *gb_font_glyph(char c);

#ifdef __cplusplus
}
#endif
