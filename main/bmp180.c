#include "bmp180.h"

#include "delay.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "growbox_logic.h"

static const char *TAG = "bmp180";

#define BMP180_ADDR       0x77
#define BMP180_CHIP_ID    0x55
#define BMP180_OSS        3
#define BMP180_TIMEOUT_MS 100

#define BMP180_REG_CTRL      0xF4
#define BMP180_REG_OUT       0xF6
#define BMP180_CTRL_SCO      0x20 /* start of conversion; reads 1 until the conversion has finished */
#define BMP180_TEMP_MAX_MS   5    /* datasheet 4.5 ms */
#define BMP180_PRESS_MAX_MS  26   /* datasheet 25.5 ms at OSS = 3 */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static gb_bmp180_calib_t s_calib;
static bool s_ready;

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, BMP180_TIMEOUT_MS);
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    const uint8_t buf[] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), BMP180_TIMEOUT_MS);
}

esp_err_t bmp180_init(i2c_master_bus_handle_t bus)
{
    s_bus = bus;
    s_ready = false;
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    if (s_dev == NULL) {
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BMP180_ADDR,
            .scl_speed_hz = 100000,
        };
        err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "add device failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    uint8_t id = 0;
    err = read_regs(0xD0, &id, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "chip id read failed: %s", esp_err_to_name(err));
        return err;
    }
    if (id != BMP180_CHIP_ID) {
        ESP_LOGW(TAG, "unexpected chip id 0x%02x", id);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t c[22];
    err = read_regs(0xAA, c, sizeof(c));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "calibration read failed: %s", esp_err_to_name(err));
        return err;
    }
#define BE16(i) ((uint16_t)((c[(i)] << 8) | c[(i) + 1]))
    s_calib.ac1 = (int16_t)BE16(0);
    s_calib.ac2 = (int16_t)BE16(2);
    s_calib.ac3 = (int16_t)BE16(4);
    s_calib.ac4 = BE16(6);
    s_calib.ac5 = BE16(8);
    s_calib.ac6 = BE16(10);
    s_calib.b1 = (int16_t)BE16(12);
    s_calib.b2 = (int16_t)BE16(14);
    s_calib.mb = (int16_t)BE16(16);
    s_calib.mc = (int16_t)BE16(18);
    s_calib.md = (int16_t)BE16(20);
#undef BE16
    s_ready = true;
    return ESP_OK;
}

/* Waits the datasheet maximum, then polls Sco until the conversion result is in the output registers. */
static esp_err_t wait_conversion(uint32_t max_ms)
{
    delay_at_least_ms(max_ms);
    for (int i = 0; i < 10; i++) {
        uint8_t ctrl;
        esp_err_t err = read_regs(BMP180_REG_CTRL, &ctrl, 1);
        if (err != ESP_OK) {
            return err;
        }
        if (!(ctrl & BMP180_CTRL_SCO)) {
            return ESP_OK;
        }
        delay_at_least_ms(2);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t bmp180_read(float *temp_c, float *pressure_hpa)
{
    if (!s_ready) {
        esp_err_t err = bmp180_init(s_bus);
        if (err != ESP_OK) {
            return err;
        }
    }
    uint8_t buf[3];
    int32_t ut = 0;
    esp_err_t err = write_reg(BMP180_REG_CTRL, 0x2E);
    if (err == ESP_OK) {
        err = wait_conversion(BMP180_TEMP_MAX_MS);
    }
    if (err == ESP_OK) {
        err = read_regs(BMP180_REG_OUT, buf, 2);
    }
    if (err == ESP_OK) {
        ut = (buf[0] << 8) | buf[1];
        err = write_reg(BMP180_REG_CTRL, 0x34 + (BMP180_OSS << 6));
    }
    if (err == ESP_OK) {
        err = wait_conversion(BMP180_PRESS_MAX_MS);
    }
    if (err == ESP_OK) {
        err = read_regs(BMP180_REG_OUT, buf, 3);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
        s_ready = false;
        return err;
    }
    int32_t up = (((int32_t)buf[0] << 16) | ((int32_t)buf[1] << 8) | buf[2]) >> (8 - BMP180_OSS);
    int32_t t10, pa;
    gb_bmp180_compensate(&s_calib, ut, up, BMP180_OSS, &t10, &pa);
    *temp_c = t10 / 10.0f;
    *pressure_hpa = pa / 100.0f;
    return ESP_OK;
}
