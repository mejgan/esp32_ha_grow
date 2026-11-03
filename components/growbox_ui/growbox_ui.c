#include "growbox_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_BLOCK_LINES 10
#define MAX_BLOCKS      (1 + GB_FLOOR_COUNT)

typedef struct {
    char text[GB_UI_MAX_COLS + 1];
    uint8_t style;
} line_t;

typedef struct {
    line_t line[MAX_BLOCK_LINES];
    uint8_t n;
    bool titled; /* line 0 is repeated on every page the block spans */
} block_t;

typedef struct {
    block_t block[MAX_BLOCKS];
    uint8_t nblocks;
    uint8_t cols, rows;
    bool single_page; /* all blocks stacked onto one page */
} layout_t;

/* Appends the first of `count` candidate strings that fits in `cols`; the last one is truncated if none fits. */
static void add_line(block_t *b, uint8_t cols, uint8_t style, int count, ...)
{
    if (b->n >= MAX_BLOCK_LINES) {
        return;
    }
    va_list ap;
    va_start(ap, count);
    const char *chosen = "";
    for (int i = 0; i < count; i++) {
        const char *c = va_arg(ap, const char *);
        chosen = c;
        if (strlen(c) <= cols) {
            break;
        }
    }
    va_end(ap);
    line_t *l = &b->line[b->n++];
    snprintf(l->text, sizeof(l->text), "%.*s", cols, chosen);
    l->style = style;
}

static void fmt_hhmm(char *buf, size_t n, uint16_t minute_of_day)
{
    snprintf(buf, n, "%02u:%02u", (unsigned)(minute_of_day / 60 % 24), (unsigned)(minute_of_day % 60));
}

static void fmt_temp(char *buf, size_t n, bool valid, float t, bool with_unit)
{
    if (!valid) {
        snprintf(buf, n, "ERR");
    } else if (with_unit) {
        snprintf(buf, n, "%.1f%cC", t, GB_UI_DEG);
    } else {
        snprintf(buf, n, "%.1f%c", t, GB_UI_DEG);
    }
}

/* A floor without its own temperature sensor shows "--" (its cutoff uses the hottest other floor). */
static void fmt_floor_temp(char *buf, size_t n, const gb_ui_floor_t *f, bool with_unit)
{
    if (f->has_temp) {
        fmt_temp(buf, n, f->temp_valid, f->temp_c, with_unit);
    } else {
        snprintf(buf, n, "--");
    }
}

static void fmt_pct(char *buf, size_t n, bool valid, float v)
{
    if (valid) {
        snprintf(buf, n, "%.0f%%", v);
    } else {
        snprintf(buf, n, "ERR");
    }
}

/* ---------- blocks ---------- */

static void add_header(block_t *b, const gb_ui_status_t *st, uint8_t cols)
{
    char t[8];
    if (st->time_valid) {
        fmt_hhmm(t, sizeof(t), st->minute_of_day);
    } else {
        snprintf(t, sizeof(t), "--:--");
    }
    const char *zb = st->joined ? "OK" : "--";
    char c1[48], c2[48], c3[48], c4[48];
    snprintf(c1, sizeof(c1), "GrowBox  %s  Zigbee %s", t, zb);
    snprintf(c2, sizeof(c2), "GrowBox  %s  ZB:%s", t, zb);
    snprintf(c3, sizeof(c3), "GrowBox %s ZB:%s", t, zb);
    snprintf(c4, sizeof(c4), "%s  ZB:%s", t, zb);
    add_line(b, cols, GB_UI_STYLE_TITLE, 4, c1, c2, c3, c4);
}

/* "Room 22.1°C  45%" (+ pressure when it fits). */
static void add_room_line(block_t *b, const gb_ui_room_t *r, uint8_t cols)
{
    char t_l[16], t_s[16], h[8], c1[48], c2[48], c3[48];
    fmt_temp(t_l, sizeof(t_l), r->temp_valid, r->temp_c, true);
    fmt_temp(t_s, sizeof(t_s), r->temp_valid, r->temp_c, false);
    fmt_pct(h, sizeof(h), r->rh_valid, r->rh);
    if (r->has_pressure && r->pressure_valid) {
        snprintf(c1, sizeof(c1), "Room %s  %s  %.0fhPa", t_l, h, r->pressure_hpa);
    } else {
        snprintf(c1, sizeof(c1), "Room %s  %s", t_l, h);
    }
    snprintf(c2, sizeof(c2), "Room %s %s", t_l, h);
    snprintf(c3, sizeof(c3), "Rm %s %s", t_s, h);
    /* A BMP280 has no humidity; it only counts as a fault when there is no pressure either. */
    bool ok = r->temp_valid && (r->rh_valid || (r->has_pressure && r->pressure_valid));
    uint8_t style = ok ? GB_UI_STYLE_NORMAL : GB_UI_STYLE_WARN;
    add_line(b, cols, style, 3, c1, c2, c3);
}

static void add_overview_line(block_t *b, const gb_ui_floor_t *f, unsigned num, uint8_t cols)
{
    char c1[48], c2[48];
    if (!f->enabled) {
        snprintf(c1, sizeof(c1), "F%u  disabled", num);
        snprintf(c2, sizeof(c2), "%u disabled", num);
        add_line(b, cols, GB_UI_STYLE_DIM, 2, c1, c2);
        return;
    }
    char state[8];
    if (f->heat_lock) {
        snprintf(state, sizeof(state), " HOT");
    } else if (f->light_on) {
        snprintf(state, sizeof(state), "%3u%%", f->level_pct);
    } else {
        snprintf(state, sizeof(state), " off");
    }
    char temp_l[16], temp_s[16], hum_l[16], hum_s[16];
    char t[12];
    fmt_floor_temp(t, sizeof(t), f, true);
    snprintf(temp_l, sizeof(temp_l), "%7s", t);
    fmt_floor_temp(t, sizeof(t), f, false);
    snprintf(temp_s, sizeof(temp_s), "%5s", t);
    if (f->has_humidity) {
        fmt_pct(t, sizeof(t), f->rh_valid, f->rh);
        snprintf(hum_l, sizeof(hum_l), " %4s", t);
        snprintf(hum_s, sizeof(hum_s), " %3s", t);
    } else {
        hum_l[0] = hum_s[0] = '\0';
    }
    snprintf(c1, sizeof(c1), "F%u %s %s%s", num, state, temp_l, hum_l);
    snprintf(c2, sizeof(c2), "%u %s %s%s", num, state, temp_s, hum_s);
    uint8_t style = GB_UI_STYLE_NORMAL;
    if (f->heat_lock || f->sensor_fault) {
        style = GB_UI_STYLE_WARN;
    } else if (f->light_on) {
        style = GB_UI_STYLE_GOOD;
    }
    add_line(b, cols, style, 2, c1, c2);
}

static void add_floor_block(block_t *b, const gb_ui_floor_t *f, unsigned num, uint8_t cols, bool compact)
{
    char c1[48], c2[48], c3[48];
    b->titled = true;

    if (!f->enabled) {
        snprintf(c1, sizeof(c1), "Floor %u: disabled", num);
        snprintf(c2, sizeof(c2), "F%u disabled", num);
        add_line(b, cols, GB_UI_STYLE_DIM, 2, c1, c2);
        return;
    }
    if (f->heat_lock) {
        snprintf(c1, sizeof(c1), "Floor %u: HEAT LOCK", num);
        snprintf(c2, sizeof(c2), "F%u HEAT LOCK", num);
        add_line(b, cols, GB_UI_STYLE_WARN, 2, c1, c2);
    } else if (f->light_on) {
        snprintf(c1, sizeof(c1), "Floor %u: ON %u%%", num, f->level_pct);
        snprintf(c2, sizeof(c2), "F%u ON %u%%", num, f->level_pct);
        add_line(b, cols, GB_UI_STYLE_TITLE, 2, c1, c2);
    } else {
        snprintf(c1, sizeof(c1), "Floor %u: off", num);
        snprintf(c2, sizeof(c2), "F%u off", num);
        add_line(b, cols, GB_UI_STYLE_TITLE, 2, c1, c2);
    }

    if (f->sensor_fault) {
        add_line(b, cols, GB_UI_STYLE_WARN, 2, "SENSOR FAULT", "SENS FAULT");
    }

    char temp[16], v[24];
    fmt_floor_temp(temp, sizeof(temp), f, true);
    if (compact) {
        /* Few rows: pack the readings densely. */
        if (f->has_humidity) {
            fmt_pct(v, sizeof(v), f->rh_valid, f->rh);
            snprintf(c1, sizeof(c1), "%s  %s", temp, v);
            snprintf(c2, sizeof(c2), "%s %s", temp, v);
            add_line(b, cols, GB_UI_STYLE_NORMAL, 2, c1, c2);
        } else if (f->has_temp) {
            add_line(b, cols, GB_UI_STYLE_NORMAL, 1, temp);
        }
        char extra[48] = "";
        if (f->has_soil) {
            fmt_pct(v, sizeof(v), f->soil_valid, f->soil_pct);
            snprintf(extra, sizeof(extra), "Soil %s", v);
        }
        if (f->has_pressure) {
            size_t len = strlen(extra);
            if (f->pressure_valid) {
                snprintf(extra + len, sizeof(extra) - len, "%s%.0fhPa", len ? " " : "", f->pressure_hpa);
            } else {
                snprintf(extra + len, sizeof(extra) - len, "%sP:ERR", len ? " " : "");
            }
        }
        if (f->has_lux) {
            size_t len = strlen(extra);
            if (f->lux_valid) {
                snprintf(extra + len, sizeof(extra) - len, "%s%.0flx", len ? " " : "", f->lux);
            } else {
                snprintf(extra + len, sizeof(extra) - len, "%sL:ERR", len ? " " : "");
            }
        }
        if (extra[0] != '\0') {
            add_line(b, cols, GB_UI_STYLE_NORMAL, 1, extra);
        }
    } else {
        snprintf(c1, sizeof(c1), "Temp %s  max %u%cC", temp, f->max_temp_c, GB_UI_DEG);
        snprintf(c2, sizeof(c2), "Temp %s max %u%cC", temp, f->max_temp_c, GB_UI_DEG);
        snprintf(c3, sizeof(c3), "T %s max %u", temp, f->max_temp_c);
        add_line(b, cols, GB_UI_STYLE_NORMAL, 3, c1, c2, c3);
        if (f->has_humidity) {
            fmt_pct(v, sizeof(v), f->rh_valid, f->rh);
            snprintf(c1, sizeof(c1), "Humidity %s", v);
            snprintf(c2, sizeof(c2), "Hum %s", v);
            add_line(b, cols, GB_UI_STYLE_NORMAL, 2, c1, c2);
        }
        if (f->has_soil) {
            fmt_pct(v, sizeof(v), f->soil_valid, f->soil_pct);
            snprintf(c1, sizeof(c1), "Soil moisture %s", v);
            snprintf(c2, sizeof(c2), "Soil %s", v);
            add_line(b, cols, GB_UI_STYLE_NORMAL, 2, c1, c2);
        }
        if (f->has_pressure) {
            if (f->pressure_valid) {
                snprintf(v, sizeof(v), "%.0f hPa", f->pressure_hpa);
            } else {
                snprintf(v, sizeof(v), "ERR");
            }
            snprintf(c1, sizeof(c1), "Pressure %s", v);
            snprintf(c2, sizeof(c2), "P %s", v);
            add_line(b, cols, GB_UI_STYLE_NORMAL, 2, c1, c2);
        }
        if (f->has_lux) {
            if (f->lux_valid) {
                snprintf(v, sizeof(v), "%.0f lx", f->lux);
            } else {
                snprintf(v, sizeof(v), "ERR");
            }
            snprintf(c1, sizeof(c1), "Light %s", v);
            snprintf(c2, sizeof(c2), "L %s", v);
            add_line(b, cols, GB_UI_STYLE_NORMAL, 2, c1, c2);
        }
    }

    if (f->auto_mode) {
        char on[8], off[8];
        fmt_hhmm(on, sizeof(on), f->on_min);
        fmt_hhmm(off, sizeof(off), f->off_min);
        snprintf(c1, sizeof(c1), "Auto %s-%s %u%%", on, off, f->auto_brightness_pct);
        snprintf(c2, sizeof(c2), "Auto %s-%s", on, off);
        snprintf(c3, sizeof(c3), "A %s-%s", on, off);
        add_line(b, cols, GB_UI_STYLE_NORMAL, 3, c1, c2, c3);
    } else {
        add_line(b, cols, GB_UI_STYLE_NORMAL, 2, "Manual mode", "Manual");
    }
}

static void add_reset_block(block_t *b, const gb_ui_status_t *st, uint8_t cols)
{
    char c1[48], c2[48];
    add_line(b, cols, GB_UI_STYLE_WARN, 2, "ZIGBEE RESET", "ZB RESET");
    snprintf(c1, sizeof(c1), "Keep holding: %us", st->reset_countdown_s);
    snprintf(c2, sizeof(c2), "Hold %us", st->reset_countdown_s);
    add_line(b, cols, GB_UI_STYLE_NORMAL, 2, c1, c2);
    add_line(b, cols, GB_UI_STYLE_DIM, 2, "Release to cancel", "Release=cancel");
}

/* ---------- layout ---------- */

static uint8_t clamp_dim(uint8_t v, uint8_t max)
{
    return v < 1 ? 1 : (v > max ? max : v);
}

static void build_layout(const gb_ui_status_t *st, uint8_t cols, uint8_t rows, layout_t *lay)
{
    memset(lay, 0, sizeof(*lay));
    lay->cols = cols;
    lay->rows = rows;

    if (st->reset_countdown_s > 0) {
        add_reset_block(&lay->block[0], st, cols);
        lay->nblocks = 1;
        return;
    }

    bool compact = rows < 4;
    block_t floors[GB_FLOOR_COUNT];
    memset(floors, 0, sizeof(floors));
    unsigned floor_lines = 0;
    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        add_floor_block(&floors[f], &st->floor[f], f + 1, cols, compact);
        floor_lines += floors[f].n;
    }

    unsigned header_lines = 1 + (st->room.present ? 1 : 0);
    if (header_lines + floor_lines <= rows) {
        /* Everything fits: one page with the header (and room line) followed by every floor. */
        lay->single_page = true;
        add_header(&lay->block[0], st, cols);
        if (st->room.present) {
            add_room_line(&lay->block[0], &st->room, cols);
        }
        for (int f = 0; f < GB_FLOOR_COUNT; f++) {
            lay->block[1 + f] = floors[f];
        }
        lay->nblocks = 1 + GB_FLOOR_COUNT;
        return;
    }

    block_t *ov = &lay->block[0];
    add_header(ov, st, cols);
    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        add_overview_line(ov, &st->floor[f], f + 1, cols);
    }
    if (st->room.present) {
        add_room_line(ov, &st->room, cols);
    }
    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        lay->block[1 + f] = floors[f];
    }
    lay->nblocks = 1 + GB_FLOOR_COUNT;
}

/* Pages one block spans on `rows` rows. */
static unsigned block_pages(const block_t *b, uint8_t rows)
{
    if (b->n == 0) {
        return 0;
    }
    if (b->n <= rows) {
        return 1;
    }
    if (b->titled && rows >= 2) {
        unsigned cap = rows - 1u;
        return (b->n - 1u + cap - 1u) / cap;
    }
    return (b->n + rows - 1u) / rows;
}

static unsigned layout_pages(const layout_t *lay)
{
    if (lay->single_page) {
        return 1;
    }
    unsigned total = 0;
    for (uint8_t i = 0; i < lay->nblocks; i++) {
        total += block_pages(&lay->block[i], lay->rows);
    }
    return total ? total : 1;
}

static void put_row(gb_ui_screen_t *out, uint8_t row, const line_t *l)
{
    if (row >= out->rows) {
        return;
    }
    size_t len = strlen(l->text);
    if (len > out->cols) {
        len = out->cols;
    }
    memcpy(out->text[row], l->text, len);
    out->style[row] = l->style;
}

static void render_page(const layout_t *lay, unsigned page, gb_ui_screen_t *out)
{
    if (lay->single_page) {
        uint8_t row = 0;
        for (uint8_t i = 0; i < lay->nblocks; i++) {
            for (uint8_t j = 0; j < lay->block[i].n; j++) {
                put_row(out, row++, &lay->block[i].line[j]);
            }
        }
        return;
    }
    for (uint8_t i = 0; i < lay->nblocks; i++) {
        const block_t *b = &lay->block[i];
        unsigned pages = block_pages(b, lay->rows);
        if (page >= pages) {
            page -= pages;
            continue;
        }
        uint8_t row = 0;
        if (b->n <= lay->rows) {
            for (uint8_t j = 0; j < b->n; j++) {
                put_row(out, row++, &b->line[j]);
            }
        } else if (b->titled && lay->rows >= 2) {
            unsigned cap = lay->rows - 1u;
            put_row(out, row++, &b->line[0]);
            for (unsigned j = 1 + page * cap; j < b->n && row < lay->rows; j++) {
                put_row(out, row++, &b->line[j]);
            }
        } else {
            for (unsigned j = page * lay->rows; j < b->n && row < lay->rows; j++) {
                put_row(out, row++, &b->line[j]);
            }
        }
        return;
    }
}

uint8_t gb_ui_page_count(const gb_ui_status_t *st, uint8_t cols, uint8_t rows)
{
    layout_t lay;
    build_layout(st, clamp_dim(cols, GB_UI_MAX_COLS), clamp_dim(rows, GB_UI_MAX_ROWS), &lay);
    unsigned n = layout_pages(&lay);
    return (uint8_t)(n > 255 ? 255 : n);
}

void gb_ui_render(const gb_ui_status_t *st, uint8_t page, uint8_t cols, uint8_t rows, gb_ui_screen_t *out)
{
    cols = clamp_dim(cols, GB_UI_MAX_COLS);
    rows = clamp_dim(rows, GB_UI_MAX_ROWS);
    out->cols = cols;
    out->rows = rows;
    for (uint8_t r = 0; r < GB_UI_MAX_ROWS; r++) {
        memset(out->text[r], ' ', cols);
        out->text[r][cols] = '\0';
        out->style[r] = GB_UI_STYLE_NORMAL;
    }
    layout_t lay;
    build_layout(st, cols, rows, &lay);
    render_page(&lay, page % layout_pages(&lay), out);
}
