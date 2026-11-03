#include "aht10.h"

#include "delay.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "growbox_logic.h"

static const char *TAG = "aht10";

#define AHT10_ADDR       0x38
#define AHT10_TIMEOUT_MS 100
#define AHT10_MEASURE_MS 80 /* datasheet: > 75 ms */
#define AHT10_BUSY_POLLS 5

esp_err_t aht10_init(aht10_t *aht, i2c_master_bus_handle_t bus)
{
    aht->bus = bus;
    aht->ready = false;
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    if (aht->dev == NULL) {
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AHT10_ADDR,
            .scl_speed_hz = 100000,
        };
        err = i2c_master_bus_add_device(bus, &cfg, &aht->dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "add device failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    const uint8_t reset[] = {0xBA};
    err = i2c_master_transmit(aht->dev, reset, sizeof(reset), AHT10_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "soft reset failed: %s", esp_err_to_name(err));
        return err;
    }
    delay_at_least_ms(20);
    const uint8_t calib[] = {0xE1, 0x08, 0x00};
    err = i2c_master_transmit(aht->dev, calib, sizeof(calib), AHT10_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "calibrate failed: %s", esp_err_to_name(err));
        return err;
    }
    delay_at_least_ms(10);
    aht->ready = true;
    return ESP_OK;
}

esp_err_t aht10_read(aht10_t *aht, float *temp_c, float *rh)
{
    if (!aht->ready) {
        esp_err_t err = aht10_init(aht, aht->bus);
        if (err != ESP_OK) {
            return err;
        }
    }
    const uint8_t trigger[] = {0xAC, 0x33, 0x00};
    esp_err_t err = i2c_master_transmit(aht->dev, trigger, sizeof(trigger), AHT10_TIMEOUT_MS);
    if (err == ESP_OK) {
        delay_at_least_ms(AHT10_MEASURE_MS);
        /* Still busy (status bit 7) right after the nominal time happens; give it a few more tries. */
        err = ESP_ERR_TIMEOUT;
        for (int i = 0; i < AHT10_BUSY_POLLS; i++) {
            uint8_t raw[6];
            esp_err_t rx = i2c_master_receive(aht->dev, raw, sizeof(raw), AHT10_TIMEOUT_MS);
            if (rx != ESP_OK) {
                err = rx;
                break;
            }
            if (gb_aht10_convert(raw, temp_c, rh)) {
                err = ESP_OK;
                break;
            }
            delay_at_least_ms(10);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
        aht->ready = false;
    }
    return err;
}
