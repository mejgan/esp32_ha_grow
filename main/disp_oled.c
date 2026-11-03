/* SSD1306 (128x64, 128x32) and SH1106 (128x64) OLEDs on I2C, 21 columns of the 6x8 font. */
#include "sdkconfig.h"

#if CONFIG_GROWBOX_DISPLAY_OLED

#include <string.h>

#include "display_drv.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "sensors.h"

static const char *TAG = "oled";

#define OLED_W 128
#if CONFIG_GROWBOX_DISPLAY_SSD1306_128X32
#define OLED_H 32
#else
#define OLED_H 64
#endif
#define OLED_PAGES (OLED_H / 8)
#define CELL_W     6
#define COLS       (OLED_W / CELL_W)
#define ROWS       OLED_PAGES
#define TIMEOUT_MS 100

#define CTRL_CMD  0x00
#define CTRL_DATA 0x40

#if CONFIG_GROWBOX_DISPLAY_I2C_BUS_LP
#define DISPLAY_BUS SENSORS_BUS_LP
#else
#define DISPLAY_BUS SENSORS_BUS_MAIN
#endif

static i2c_master_dev_handle_t s_dev;
static uint8_t s_fb[OLED_PAGES][OLED_W];
static uint8_t s_shown[OLED_PAGES][OLED_W];
static bool s_shown_valid;

static esp_err_t send_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[1 + 32];
    if (n > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = CTRL_CMD;
    memcpy(buf + 1, cmds, n);
    return i2c_master_transmit(s_dev, buf, n + 1, TIMEOUT_MS);
}

static esp_err_t find_address(i2c_master_bus_handle_t bus, uint16_t *addr)
{
    if (CONFIG_GROWBOX_DISPLAY_I2C_ADDR != 0) {
        *addr = CONFIG_GROWBOX_DISPLAY_I2C_ADDR;
        return ESP_OK;
    }
    static const uint8_t candidates[] = {0x3C, 0x3D};
    for (size_t i = 0; i < sizeof(candidates); i++) {
        if (i2c_master_probe(bus, candidates[i], 50) == ESP_OK) {
            *addr = candidates[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t disp_drv_init(uint8_t *cols, uint8_t *rows)
{
    i2c_master_bus_handle_t bus = sensors_i2c_bus(DISPLAY_BUS);
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_dev == NULL) {
        uint16_t addr;
        esp_err_t err = find_address(bus, &addr);
        if (err != ESP_OK) {
            return err;
        }
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = CONFIG_GROWBOX_DISPLAY_I2C_HZ,
        };
        err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
        if (err != ESP_OK) {
            s_dev = NULL;
            return err;
        }
        ESP_LOGI(TAG, "OLED at 0x%02x", addr);
    }

#if CONFIG_GROWBOX_DISPLAY_OLED_FLIP
    const uint8_t seg_remap = 0xA0, com_scan = 0xC0;
#else
    const uint8_t seg_remap = 0xA1, com_scan = 0xC8;
#endif
#if CONFIG_GROWBOX_DISPLAY_SH1106_128X64
    const uint8_t init[] = {
        0xAE,                                 /* display off */
        0xD5, 0x80,                           /* clock */
        0xA8, OLED_H - 1,                     /* multiplex */
        0xD3, 0x00,                           /* display offset */
        0x40,                                 /* start line */
        0xAD, 0x8B,                           /* DC-DC on */
        seg_remap, com_scan,                  /* orientation */
        0xDA, 0x12,                           /* COM pins */
        0x81, CONFIG_GROWBOX_DISPLAY_OLED_CONTRAST,
        0xD9, 0x1F,                           /* pre-charge */
        0xDB, 0x40,                           /* VCOM detect */
        0x32,                                 /* pump voltage 8 V */
        0xA4, 0xA6,                           /* show RAM, normal (not inverted) */
    };
#else
    const uint8_t init[] = {
        0xAE,
        0xD5, 0x80,
        0xA8, OLED_H - 1,
        0xD3, 0x00,
        0x40,
        0x8D, 0x14,                           /* charge pump on */
        0x20, 0x00,                           /* horizontal addressing */
        seg_remap, com_scan,
        0xDA, OLED_H == 64 ? 0x12 : 0x02,
        0x81, CONFIG_GROWBOX_DISPLAY_OLED_CONTRAST,
        0xD9, 0xF1,
        0xDB, 0x40,
        0x2E,                                 /* no scrolling */
        0xA4, 0xA6,
    };
#endif
    esp_err_t err = send_cmds(init, sizeof(init));
    if (err != ESP_OK) {
        return err;
    }
    /* Clear the RAM before switching on, so no noise shows. */
    memset(s_fb, 0, sizeof(s_fb));
    s_shown_valid = false;
    gb_ui_screen_t blank = {.cols = COLS, .rows = ROWS};
    for (int r = 0; r < ROWS; r++) {
        memset(blank.text[r], ' ', COLS);
    }
    err = disp_drv_draw(&blank);
    if (err == ESP_OK) {
        err = send_cmds((const uint8_t[]){0xAF}, 1);
    }
    *cols = COLS;
    *rows = ROWS;
    return err;
}

static esp_err_t write_page(int page)
{
#if CONFIG_GROWBOX_DISPLAY_SH1106_128X64
    /* SH1106 RAM is 132 columns wide, the visible 128 start at column 2. */
    const uint8_t addr[] = {(uint8_t)(0xB0 | page), 0x02, 0x10};
#else
    const uint8_t addr[] = {0x21, 0, OLED_W - 1, 0x22, (uint8_t)page, (uint8_t)page};
#endif
    esp_err_t err = send_cmds(addr, sizeof(addr));
    if (err != ESP_OK) {
        return err;
    }
    uint8_t buf[1 + OLED_W];
    buf[0] = CTRL_DATA;
    memcpy(buf + 1, s_fb[page], OLED_W);
    return i2c_master_transmit(s_dev, buf, sizeof(buf), TIMEOUT_MS);
}

esp_err_t disp_drv_draw(const gb_ui_screen_t *s)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int r = 0; r < ROWS; r++) {
        uint8_t *line = s_fb[r];
        memset(line, 0, OLED_W);
        if (r < s->rows) {
            for (int c = 0; c < COLS && c < s->cols; c++) {
                memcpy(line + c * CELL_W, gb_font_glyph(s->text[r][c]), 5);
            }
            /* Title and warning rows are drawn inverted, as a bar across the whole width. */
            if (s->style[r] == GB_UI_STYLE_TITLE || s->style[r] == GB_UI_STYLE_WARN) {
                for (int x = 0; x < OLED_W; x++) {
                    line[x] ^= 0xFF;
                }
            }
        }
    }
    for (int p = 0; p < OLED_PAGES; p++) {
        if (s_shown_valid && memcmp(s_fb[p], s_shown[p], OLED_W) == 0) {
            continue;
        }
        esp_err_t err = write_page(p);
        if (err != ESP_OK) {
            s_shown_valid = false;
            return err;
        }
        memcpy(s_shown[p], s_fb[p], OLED_W);
    }
    s_shown_valid = true;
    return ESP_OK;
}

esp_err_t disp_drv_power(bool on)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t cmd = on ? 0xAF : 0xAE;
    return send_cmds(&cmd, 1);
}

#endif
