/*
 * ST7789 / ILI9341 color TFTs on SPI. Text cells of the 6x8 font scaled by CONFIG_GROWBOX_DISPLAY_FONT_SCALE;
 * only cells that changed are redrawn.
 */
#include "sdkconfig.h"

#if CONFIG_GROWBOX_DISPLAY_TFT

#include <string.h>

#include "delay.h"
#include "display_drv.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "spi_bus.h"

static const char *TAG = "tft";

#define SCALE  CONFIG_GROWBOX_DISPLAY_FONT_SCALE
#define CELL_W (6 * SCALE)
#define CELL_H (8 * SCALE)

#if CONFIG_GROWBOX_DISPLAY_ROTATION == 1 || CONFIG_GROWBOX_DISPLAY_ROTATION == 3
#define SCREEN_W CONFIG_GROWBOX_DISPLAY_HEIGHT
#define SCREEN_H CONFIG_GROWBOX_DISPLAY_WIDTH
#else
#define SCREEN_W CONFIG_GROWBOX_DISPLAY_WIDTH
#define SCREEN_H CONFIG_GROWBOX_DISPLAY_HEIGHT
#endif

#define COLS_RAW (SCREEN_W / CELL_W)
#define ROWS_RAW (SCREEN_H / CELL_H)
#define COLS     (COLS_RAW > GB_UI_MAX_COLS ? GB_UI_MAX_COLS : COLS_RAW)
#define ROWS     (ROWS_RAW > GB_UI_MAX_ROWS ? GB_UI_MAX_ROWS : ROWS_RAW)
/* The text grid is centered; the margins stay black. */
#define GRID_X ((SCREEN_W - COLS * CELL_W) / 2)
#define GRID_Y ((SCREEN_H - ROWS * CELL_H) / 2)

#define SPI_HOST_ID GB_SPI_HOST

/* MIPI DCS commands shared by both controllers. */
#define CMD_SWRESET 0x01
#define CMD_SLPIN   0x10
#define CMD_SLPOUT  0x11
#define CMD_INVOFF  0x20
#define CMD_INVON   0x21
#define CMD_DISPOFF 0x28
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_MADCTL  0x36
#define CMD_RAMWRC  0x3C
#define CMD_COLMOD  0x3A

#define MADCTL_MY  0x80
#define MADCTL_MX  0x40
#define MADCTL_MV  0x20
#define MADCTL_BGR 0x08

#define RGB565(rgb) \
    ((uint16_t)((((rgb) >> 19) & 0x1F) << 11 | (((rgb) >> 10) & 0x3F) << 5 | (((rgb) >> 3) & 0x1F)))

static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_cell; /* one text cell of pixels, DMA capable, byte-swapped RGB565 */
static char s_shown[GB_UI_MAX_ROWS][GB_UI_MAX_COLS];
static uint8_t s_shown_style[GB_UI_MAX_ROWS];
static bool s_shown_valid;

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[15];
    uint16_t delay_ms;
} init_cmd_t;

#if CONFIG_GROWBOX_DISPLAY_ILI9341
/* Panel setup as used by the common Adafruit/TFT_eSPI drivers. */
static const init_cmd_t k_init[] = {
    {0xEF, 3, {0x03, 0x80, 0x02}, 0},
    {0xCF, 3, {0x00, 0xC1, 0x30}, 0},
    {0xED, 4, {0x64, 0x03, 0x12, 0x81}, 0},
    {0xE8, 3, {0x85, 0x00, 0x78}, 0},
    {0xCB, 5, {0x39, 0x2C, 0x00, 0x34, 0x02}, 0},
    {0xF7, 1, {0x20}, 0},
    {0xEA, 2, {0x00, 0x00}, 0},
    {0xC0, 1, {0x23}, 0},       /* power control 1 */
    {0xC1, 1, {0x10}, 0},       /* power control 2 */
    {0xC5, 2, {0x3E, 0x28}, 0}, /* VCOM 1 */
    {0xC7, 1, {0x86}, 0},       /* VCOM 2 */
    {0xB1, 2, {0x00, 0x18}, 0}, /* frame rate */
    {0xB6, 3, {0x08, 0x82, 0x27}, 0},
    {0xF2, 1, {0x00}, 0},       /* 3-gamma off */
    {0x26, 1, {0x01}, 0},       /* gamma curve 1 */
    {0xE0, 15, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}, 0},
    {0xE1, 15, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}, 0},
};
static const uint8_t k_madctl[4] = {MADCTL_MX, MADCTL_MV, MADCTL_MY, MADCTL_MX | MADCTL_MY | MADCTL_MV};
#else
static const init_cmd_t k_init[] = {
    {0xB2, 5, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 0}, /* porch */
    {0xB7, 1, {0x35}, 0},                         /* gate control */
    {0xBB, 1, {0x19}, 0},                         /* VCOM */
    {0xC0, 1, {0x2C}, 0},
    {0xC2, 1, {0x01}, 0},
    {0xC3, 1, {0x12}, 0},
    {0xC4, 1, {0x20}, 0},
    {0xC6, 1, {0x0F}, 0},                         /* 60 Hz */
    {0xD0, 2, {0xA4, 0xA1}, 0},
};
static const uint8_t k_madctl[4] = {0, MADCTL_MX | MADCTL_MV, MADCTL_MX | MADCTL_MY, MADCTL_MY | MADCTL_MV};
#endif

static uint16_t style_color(uint8_t style)
{
    switch (style) {
    case GB_UI_STYLE_TITLE:
        return RGB565(0x00E0FF);
    case GB_UI_STYLE_GOOD:
        return RGB565(0x40FF40);
    case GB_UI_STYLE_WARN:
        return RGB565(0xFF4040);
    case GB_UI_STYLE_DIM:
        return RGB565(0x808080);
    default:
        return RGB565(0xFFFFFF);
    }
}

static inline uint16_t swap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static esp_err_t param(uint8_t cmd, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_io, cmd, data, len);
}

/* Sets the drawing window (inclusive, screen coordinates). Waits for queued pixel transfers to finish. */
static esp_err_t set_window(int x0, int y0, int x1, int y1)
{
    x0 += CONFIG_GROWBOX_DISPLAY_X_OFFSET;
    x1 += CONFIG_GROWBOX_DISPLAY_X_OFFSET;
    y0 += CONFIG_GROWBOX_DISPLAY_Y_OFFSET;
    y1 += CONFIG_GROWBOX_DISPLAY_Y_OFFSET;
    const uint8_t cols[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t rows[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    esp_err_t err = param(CMD_CASET, cols, 4);
    if (err == ESP_OK) {
        err = param(CMD_RASET, rows, 4);
    }
    return err;
}

static esp_err_t clear_screen(void)
{
    esp_err_t err = set_window(0, 0, SCREEN_W - 1, SCREEN_H - 1);
    if (err != ESP_OK) {
        return err;
    }
    /* s_cell is all black and is not modified while these transfers are queued. */
    memset(s_cell, 0, CELL_W * CELL_H * sizeof(uint16_t));
    const size_t chunk = CELL_W * CELL_H * sizeof(uint16_t);
    size_t left = (size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t);
    bool first = true;
    while (left > 0 && err == ESP_OK) {
        size_t n = left < chunk ? left : chunk;
        err = esp_lcd_panel_io_tx_color(s_io, first ? CMD_RAMWR : CMD_RAMWRC, s_cell, n);
        first = false;
        left -= n;
    }
    return err;
}

esp_err_t disp_drv_init(uint8_t *cols, uint8_t *rows)
{
    esp_err_t err;
    if (s_cell == NULL) {
        s_cell = heap_caps_malloc(CELL_W * CELL_H * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (s_cell == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    _Static_assert(CELL_W * CELL_H * sizeof(uint16_t) <= GB_SPI_MAX_TRANSFER, "TFT cell larger than an SPI transfer");
    err = spi_bus_setup();
    if (err != ESP_OK) {
        return err;
    }
    if (s_io == NULL) {
        const esp_lcd_panel_io_spi_config_t io = {
            .cs_gpio_num = CONFIG_GROWBOX_DISPLAY_SPI_CS_GPIO,
            .dc_gpio_num = CONFIG_GROWBOX_DISPLAY_SPI_DC_GPIO,
            .spi_mode = CONFIG_GROWBOX_DISPLAY_SPI_MODE,
            .pclk_hz = CONFIG_GROWBOX_DISPLAY_SPI_HZ,
            .trans_queue_depth = 8,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
        };
        err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_ID, &io, &s_io);
        if (err != ESP_OK) {
            s_io = NULL;
            return err;
        }
    }

#if CONFIG_GROWBOX_DISPLAY_BL_GPIO >= 0
    gpio_reset_pin(CONFIG_GROWBOX_DISPLAY_BL_GPIO);
    gpio_set_direction(CONFIG_GROWBOX_DISPLAY_BL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_GROWBOX_DISPLAY_BL_GPIO, 0);
#endif
#if CONFIG_GROWBOX_DISPLAY_SPI_RST_GPIO >= 0
    gpio_reset_pin(CONFIG_GROWBOX_DISPLAY_SPI_RST_GPIO);
    gpio_set_direction(CONFIG_GROWBOX_DISPLAY_SPI_RST_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_GROWBOX_DISPLAY_SPI_RST_GPIO, 0);
    delay_at_least_ms(10);
    gpio_set_level(CONFIG_GROWBOX_DISPLAY_SPI_RST_GPIO, 1);
    delay_at_least_ms(120);
#endif

    err = param(CMD_SWRESET, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    delay_at_least_ms(150);
    err = param(CMD_SLPOUT, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    delay_at_least_ms(120);
    for (size_t i = 0; i < sizeof(k_init) / sizeof(k_init[0]) && err == ESP_OK; i++) {
        err = param(k_init[i].cmd, k_init[i].data, k_init[i].len);
        if (k_init[i].delay_ms) {
            delay_at_least_ms(k_init[i].delay_ms);
        }
    }
    uint8_t madctl = k_madctl[CONFIG_GROWBOX_DISPLAY_ROTATION & 3];
#if CONFIG_GROWBOX_DISPLAY_BGR
    madctl |= MADCTL_BGR;
#endif
    const uint8_t colmod = 0x55; /* 16 bits per pixel */
    if (err == ESP_OK) {
        err = param(CMD_MADCTL, &madctl, 1);
    }
    if (err == ESP_OK) {
        err = param(CMD_COLMOD, &colmod, 1);
    }
    if (err == ESP_OK) {
#if CONFIG_GROWBOX_DISPLAY_INVERT
        err = param(CMD_INVON, NULL, 0);
#else
        err = param(CMD_INVOFF, NULL, 0);
#endif
    }
    if (err == ESP_OK) {
        err = clear_screen();
    }
    if (err == ESP_OK) {
        err = param(CMD_DISPON, NULL, 0);
    }
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_GROWBOX_DISPLAY_BL_GPIO >= 0
    gpio_set_level(CONFIG_GROWBOX_DISPLAY_BL_GPIO, 1);
#endif
    memset(s_shown, ' ', sizeof(s_shown));
    memset(s_shown_style, 0, sizeof(s_shown_style));
    s_shown_valid = true; /* just cleared to black, which is what blank cells look like */
    ESP_LOGI(TAG, "%dx%d px, %dx%d characters", SCREEN_W, SCREEN_H, COLS, ROWS);
    *cols = COLS;
    *rows = ROWS;
    return ESP_OK;
}

static esp_err_t draw_cell(int col, int row, char ch, uint16_t fg)
{
    int x = GRID_X + col * CELL_W;
    int y = GRID_Y + row * CELL_H;
    esp_err_t err = set_window(x, y, x + CELL_W - 1, y + CELL_H - 1); /* also waits until s_cell is free */
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t *g = gb_font_glyph(ch);
    const uint16_t on = swap16(fg);
    for (int py = 0; py < CELL_H; py++) {
        int font_row = py / SCALE;
        for (int px = 0; px < CELL_W; px++) {
            int font_col = px / SCALE;
            bool set = font_col < 5 && font_row < 7 && (g[font_col] >> font_row) & 1;
            s_cell[py * CELL_W + px] = set ? on : 0;
        }
    }
    return esp_lcd_panel_io_tx_color(s_io, CMD_RAMWR, s_cell, CELL_W * CELL_H * sizeof(uint16_t));
}

esp_err_t disp_drv_draw(const gb_ui_screen_t *s)
{
    if (s_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int r = 0; r < ROWS; r++) {
        uint8_t style = r < s->rows ? s->style[r] : GB_UI_STYLE_NORMAL;
        bool restyle = !s_shown_valid || style != s_shown_style[r];
        uint16_t fg = style_color(style);
        for (int c = 0; c < COLS; c++) {
            char ch = (r < s->rows && c < s->cols) ? s->text[r][c] : ' ';
            if (!restyle && ch == s_shown[r][c]) {
                continue;
            }
            /* A space looks the same in every color. */
            if (s_shown_valid && ch == ' ' && s_shown[r][c] == ' ') {
                continue;
            }
            esp_err_t err = draw_cell(c, r, ch, fg);
            if (err != ESP_OK) {
                s_shown_valid = false;
                return err;
            }
            s_shown[r][c] = ch;
        }
        s_shown_style[r] = style;
    }
    s_shown_valid = true;
    return ESP_OK;
}

esp_err_t disp_drv_power(bool on)
{
    if (s_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    if (on) {
        err = param(CMD_SLPOUT, NULL, 0);
        delay_at_least_ms(120);
        if (err == ESP_OK) {
            err = param(CMD_DISPON, NULL, 0);
        }
#if CONFIG_GROWBOX_DISPLAY_BL_GPIO >= 0
        gpio_set_level(CONFIG_GROWBOX_DISPLAY_BL_GPIO, 1);
#endif
    } else {
#if CONFIG_GROWBOX_DISPLAY_BL_GPIO >= 0
        gpio_set_level(CONFIG_GROWBOX_DISPLAY_BL_GPIO, 0);
#endif
        err = param(CMD_DISPOFF, NULL, 0);
        if (err == ESP_OK) {
            err = param(CMD_SLPIN, NULL, 0);
        }
    }
    return err;
}

#endif
