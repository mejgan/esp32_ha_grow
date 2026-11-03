#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "growbox_ui.h"
#include "ui_sample.h"

static const struct {
    uint8_t cols, rows;
} k_grids[] = {
    {21, 8},  /* SSD1306/SH1106 128x64 */
    {21, 4},  /* SSD1306 128x32 */
    {16, 2},  /* HD44780 1602 */
    {20, 4},  /* HD44780 2004 */
    {20, 20}, /* 240x320 TFT, font x2 */
    {26, 15}, /* 320x240 TFT, font x2 */
    {20, 15}, /* 240x240 TFT, font x2 */
    {40, 30}, /* 240x320 TFT, font x1 */
    {11, 3},  /* odd tiny grid */
};

static bool screen_contains(const gb_ui_screen_t *s, const char *needle)
{
    for (uint8_t r = 0; r < s->rows; r++) {
        if (strstr(s->text[r], needle)) {
            return true;
        }
    }
    return false;
}

static void check_screen_shape(const gb_ui_screen_t *s, uint8_t cols, uint8_t rows)
{
    CHECK_EQ(s->cols, cols);
    CHECK_EQ(s->rows, rows);
    for (uint8_t r = 0; r < rows; r++) {
        CHECK_EQ(strlen(s->text[r]), cols);
        for (uint8_t c = 0; c < cols; c++) {
            unsigned char ch = (unsigned char)s->text[r][c];
            CHECK(ch >= 0x20 && ch <= 0x7F);
        }
        CHECK(s->style[r] <= GB_UI_STYLE_DIM);
    }
}

/* Every floor shows up on some page of the grid, and every page is well formed. */
static void check_grid(const gb_ui_status_t *st, uint8_t cols, uint8_t rows)
{
    static gb_ui_screen_t s;
    uint8_t pages = gb_ui_page_count(st, cols, rows);
    CHECK(pages >= 1);
    bool seen[GB_FLOOR_COUNT] = {false};
    bool seen_time = false;
    bool seen_room = false;
    for (uint8_t p = 0; p < pages; p++) {
        gb_ui_render(st, p, cols, rows, &s);
        check_screen_shape(&s, cols, rows);
        for (int f = 0; f < GB_FLOOR_COUNT; f++) {
            char a[16], b[16];
            snprintf(a, sizeof(a), "Floor %d", f + 1);
            snprintf(b, sizeof(b), "F%d ", f + 1);
            seen[f] |= screen_contains(&s, a) || screen_contains(&s, b);
        }
        seen_time |= screen_contains(&s, "13:05");
        seen_room |= screen_contains(&s, "Room ") || screen_contains(&s, "Rm ");
    }
    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        if (!seen[f]) {
            fprintf(stderr, "grid %ux%u: floor %d never shown\n", cols, rows, f + 1);
        }
        CHECK(seen[f]);
    }
    CHECK(seen_time);
    CHECK_EQ(seen_room, st->room.present);

    /* Pages wrap around. */
    static gb_ui_screen_t a, b;
    gb_ui_render(st, 0, cols, rows, &a);
    gb_ui_render(st, pages, cols, rows, &b);
    CHECK(memcmp(a.text, b.text, sizeof(a.text)) == 0);
}

static void test_all_grids(void)
{
    gb_ui_status_t st = ui_sample_status();
    for (size_t i = 0; i < sizeof(k_grids) / sizeof(k_grids[0]); i++) {
        check_grid(&st, k_grids[i].cols, k_grids[i].rows);
    }
    /* Same with every floor in some unusual state. */
    st.floor[0].sensor_fault = true;
    st.floor[0].temp_valid = false;
    st.floor[0].rh_valid = false;
    st.floor[1].auto_mode = false;
    st.floor[2].pressure_valid = false;
    st.floor[2].lux_valid = false;
    st.joined = false;
    for (size_t i = 0; i < sizeof(k_grids) / sizeof(k_grids[0]); i++) {
        check_grid(&st, k_grids[i].cols, k_grids[i].rows);
    }
    /* Room sensor with pressure, failing humidity, and no room sensor at all. */
    st.room.has_pressure = true;
    st.room.pressure_valid = true;
    st.room.pressure_hpa = 1009.0f;
    st.room.rh_valid = false;
    for (size_t i = 0; i < sizeof(k_grids) / sizeof(k_grids[0]); i++) {
        check_grid(&st, k_grids[i].cols, k_grids[i].rows);
    }
    st.room.present = false;
    for (size_t i = 0; i < sizeof(k_grids) / sizeof(k_grids[0]); i++) {
        check_grid(&st, k_grids[i].cols, k_grids[i].rows);
    }
    /* Floors with other sensor sets: no sensors at all, temperature only, light only. */
    st = ui_sample_status();
    st.floor[0] = (gb_ui_floor_t){.enabled = true, .light_on = true, .level_pct = 50, .auto_mode = true};
    st.floor[1].has_humidity = st.floor[1].has_soil = false;
    st.floor[2].has_temp = st.floor[2].has_humidity = st.floor[2].has_pressure = false;
    st.floor[2].enabled = true;
    for (size_t i = 0; i < sizeof(k_grids) / sizeof(k_grids[0]); i++) {
        check_grid(&st, k_grids[i].cols, k_grids[i].rows);
    }
}

static void test_floor_without_temp(void)
{
    gb_ui_status_t st = ui_sample_status();
    static gb_ui_screen_t s;
    st.floor[0].has_temp = st.floor[0].has_humidity = false;
    gb_ui_render(&st, 0, 21, 8, &s);
    CHECK(screen_contains(&s, "F1  80%      --"));
    CHECK(!screen_contains(&s, "F1  80%  24.5"));
    gb_ui_render(&st, 1, 21, 8, &s);
    CHECK(screen_contains(&s, "Temp --  max 35\x7f" "C"));
    CHECK(!screen_contains(&s, "Humidity"));
}

static void test_content(void)
{
    gb_ui_status_t st = ui_sample_status();
    static gb_ui_screen_t s;

    /* 128x64 OLED: overview first, then one page per floor. */
    CHECK_EQ(gb_ui_page_count(&st, 21, 8), 4);
    gb_ui_render(&st, 0, 21, 8, &s);
    CHECK(screen_contains(&s, "GrowBox  13:05  ZB:OK"));
    CHECK(screen_contains(&s, "F1  80%  24.5\x7f" "C  55%"));
    CHECK(screen_contains(&s, "F2  HOT"));
    CHECK(screen_contains(&s, "F3  disabled"));
    CHECK(screen_contains(&s, "Room 22.4\x7f" "C  47%"));
    CHECK_EQ(s.style[0], GB_UI_STYLE_TITLE);
    CHECK_EQ(s.style[1], GB_UI_STYLE_GOOD);
    CHECK_EQ(s.style[2], GB_UI_STYLE_WARN);
    CHECK_EQ(s.style[3], GB_UI_STYLE_DIM);

    gb_ui_render(&st, 1, 21, 8, &s);
    CHECK(screen_contains(&s, "Floor 1: ON 80%"));
    CHECK(screen_contains(&s, "Temp 24.5\x7f" "C  max 35\x7f" "C"));
    CHECK(screen_contains(&s, "Humidity 55%"));
    CHECK(screen_contains(&s, "Soil moisture 43%"));
    CHECK(screen_contains(&s, "Auto 06:30-00:00 80%"));

    gb_ui_render(&st, 2, 21, 8, &s);
    CHECK(screen_contains(&s, "Floor 2: HEAT LOCK"));
    CHECK_EQ(s.style[0], GB_UI_STYLE_WARN);
    CHECK(screen_contains(&s, "Manual"));

    /* Big TFT: everything on one page. */
    CHECK_EQ(gb_ui_page_count(&st, 20, 20), 1);
    gb_ui_render(&st, 0, 20, 20, &s);
    CHECK(screen_contains(&s, "Room 22.4"));
    CHECK(screen_contains(&s, "Floor 1: ON 80%"));
    CHECK(screen_contains(&s, "Floor 3: disabled"));

    /* 16x2 LCD: short forms. */
    gb_ui_render(&st, 0, 16, 2, &s);
    CHECK(screen_contains(&s, "13:05  ZB:OK"));

    /* Reset countdown replaces every page. */
    st.reset_countdown_s = 7;
    CHECK_EQ(gb_ui_page_count(&st, 21, 8), 1);
    gb_ui_render(&st, 3, 21, 8, &s);
    CHECK(screen_contains(&s, "ZIGBEE RESET"));
    CHECK(screen_contains(&s, "Keep holding: 7s"));
    gb_ui_render(&st, 0, 16, 2, &s);
    CHECK(screen_contains(&s, "ZIGBEE RESET"));
    CHECK(screen_contains(&s, "Keep holding: 7s"));

    /* No clock yet. */
    st.reset_countdown_s = 0;
    st.time_valid = false;
    gb_ui_render(&st, 0, 21, 8, &s);
    CHECK(screen_contains(&s, "--:--"));
}

static void test_font(void)
{
    CHECK(gb_font_glyph(' ') == gb_font5x7[0]);
    CHECK(gb_font_glyph(GB_UI_DEG) == gb_font5x7[GB_FONT_LAST - GB_FONT_FIRST]);
    CHECK(gb_font_glyph('\x01') == gb_font_glyph('?'));
    CHECK(gb_font_glyph((char)0xC3) == gb_font_glyph('?'));
    /* Every printable glyph except space has pixels, and only in the 7 font rows. */
    for (int c = '!'; c <= GB_FONT_LAST; c++) {
        const uint8_t *g = gb_font_glyph((char)c);
        uint8_t any = 0;
        for (int i = 0; i < 5; i++) {
            any |= g[i];
            CHECK((g[i] & 0x80) == 0);
        }
        CHECK(any != 0);
    }
}

int main(void)
{
    test_all_grids();
    test_content();
    test_floor_without_temp();
    test_font();
    TEST_MAIN_END();
}
