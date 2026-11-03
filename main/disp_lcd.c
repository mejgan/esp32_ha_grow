/*
 * HD44780 character LCD (16x2, 20x4) through a PCF8574 I2C backpack, in 4-bit mode.
 * Backpack wiring (the common one): P0 RS, P1 RW, P2 E, P3 backlight, P4-P7 D4-D7.
 */
#include "sdkconfig.h"

#if CONFIG_GROWBOX_DISPLAY_LCD

#include <string.h>

#include "delay.h"
#include "display_drv.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "sensors.h"

static const char *TAG = "lcd";

#if CONFIG_GROWBOX_DISPLAY_HD44780_2004
#define COLS 20
#define ROWS 4
#else
#define COLS 16
#define ROWS 2
#endif

#define PIN_RS     0x01
#define PIN_E      0x04
#define PIN_BL     0x08
#define TIMEOUT_MS 100
#define DEGREE     0xDF /* degree sign in the HD44780 A00 character ROM */

#if CONFIG_GROWBOX_DISPLAY_I2C_BUS_LP
#define DISPLAY_BUS SENSORS_BUS_LP
#else
#define DISPLAY_BUS SENSORS_BUS_MAIN
#endif

static const uint8_t k_row_addr[4] = {0x00, 0x40, COLS, 0x40 + COLS};

static i2c_master_dev_handle_t s_dev;
static uint8_t s_backlight = PIN_BL;
static char s_shown[ROWS][COLS];
static bool s_shown_valid;

/* Appends the 3 expander writes that clock one nibble (upper 4 bits of `v`) into the LCD. */
static size_t put_nibble(uint8_t *buf, uint8_t v, uint8_t rs)
{
    uint8_t b = (uint8_t)((v & 0xF0) | rs | s_backlight);
    buf[0] = b;
    buf[1] = b | PIN_E;
    buf[2] = b; /* latched on the falling edge of E */
    return 3;
}

static size_t put_byte(uint8_t *buf, uint8_t v, uint8_t rs)
{
    size_t n = put_nibble(buf, v, rs);
    return n + put_nibble(buf + n, (uint8_t)(v << 4), rs);
}

static esp_err_t send(const uint8_t *buf, size_t n)
{
    return i2c_master_transmit(s_dev, buf, n, TIMEOUT_MS);
}

static esp_err_t command(uint8_t cmd)
{
    uint8_t buf[6];
    return send(buf, put_byte(buf, cmd, 0));
}

static esp_err_t find_address(i2c_master_bus_handle_t bus, uint16_t *addr)
{
    if (CONFIG_GROWBOX_DISPLAY_I2C_ADDR != 0) {
        *addr = CONFIG_GROWBOX_DISPLAY_I2C_ADDR;
        return ESP_OK;
    }
    /* PCF8574 0x20-0x27, PCF8574A 0x38-0x3F. 0x38 is skipped: that is the AHT10 sharing the bus. */
    static const uint8_t candidates[] = {0x27, 0x3F, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
                                         0x20, 0x3E, 0x3D, 0x3C, 0x3B, 0x3A, 0x39};
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
    esp_err_t err;
    if (s_dev == NULL) {
        uint16_t addr;
        err = find_address(bus, &addr);
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
        ESP_LOGI(TAG, "LCD backpack at 0x%02x", addr);
    }

    s_backlight = PIN_BL;
    delay_at_least_ms(50);
    /* Datasheet "initialization by instruction": three times 8-bit mode, then switch to 4-bit. */
    uint8_t buf[6];
    const uint32_t waits_ms[] = {5, 1, 1, 1};
    const uint8_t nibbles[] = {0x30, 0x30, 0x30, 0x20};
    for (int i = 0; i < 4; i++) {
        err = send(buf, put_nibble(buf, nibbles[i], 0));
        if (err != ESP_OK) {
            return err;
        }
        delay_at_least_ms(waits_ms[i]);
    }
    const uint8_t setup[] = {
        0x28, /* 4-bit, 2 lines (4-line modules are 2 lines internally), 5x8 */
        0x08, /* display off */
        0x01, /* clear */
        0x06, /* entry mode: increment, no shift */
        0x0C, /* display on, no cursor */
    };
    for (size_t i = 0; i < sizeof(setup); i++) {
        err = command(setup[i]);
        if (err != ESP_OK) {
            return err;
        }
        delay_at_least_ms(setup[i] == 0x01 ? 2 : 1);
    }
    memset(s_shown, ' ', sizeof(s_shown));
    s_shown_valid = true; /* just cleared */
    *cols = COLS;
    *rows = ROWS;
    return ESP_OK;
}

static uint8_t map_char(char c)
{
    if (c == GB_UI_DEG) {
        return DEGREE;
    }
    unsigned char u = (unsigned char)c;
    return (u >= 0x20 && u < 0x7F) ? u : '?';
}

esp_err_t disp_drv_draw(const gb_ui_screen_t *s)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int r = 0; r < ROWS; r++) {
        char want[COLS];
        for (int c = 0; c < COLS; c++) {
            want[c] = (r < s->rows && c < s->cols) ? s->text[r][c] : ' ';
        }
        /* Rewrite the span from the first to the last changed character of the row. */
        int first = 0, last = COLS - 1;
        if (s_shown_valid) {
            while (first < COLS && want[first] == s_shown[r][first]) {
                first++;
            }
            if (first == COLS) {
                continue;
            }
            while (last > first && want[last] == s_shown[r][last]) {
                last--;
            }
        }
        uint8_t buf[6 * (COLS + 1)];
        size_t n = put_byte(buf, (uint8_t)(0x80 | (k_row_addr[r] + first)), 0);
        for (int c = first; c <= last; c++) {
            n += put_byte(buf + n, map_char(want[c]), PIN_RS);
        }
        esp_err_t err = send(buf, n);
        if (err != ESP_OK) {
            s_shown_valid = false;
            return err;
        }
        memcpy(&s_shown[r][first], &want[first], (size_t)(last - first + 1));
    }
    s_shown_valid = true;
    return ESP_OK;
}

esp_err_t disp_drv_power(bool on)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_backlight = on ? PIN_BL : 0;
    return command(on ? 0x0C : 0x08);
}

#endif
