#include "aht10.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "growbox_logic.h"

static const char *TAG = "aht10";

#define AHT10_ADDR       0x38
#define AHT10_TIMEOUT_MS 100

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
    vTaskDelay(pdMS_TO_TICKS(20));
    const uint8_t calib[] = {0xE1, 0x08, 0x00};
    err = i2c_master_transmit(aht->dev, calib, sizeof(calib), AHT10_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "calibrate failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
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
        vTaskDelay(pdMS_TO_TICKS(80));
        uint8_t raw[6];
        err = i2c_master_receive(aht->dev, raw, sizeof(raw), AHT10_TIMEOUT_MS);
        if (err == ESP_OK && !gb_aht10_convert(raw, temp_c, rh)) {
            err = ESP_ERR_INVALID_STATE;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
        aht->ready = false;
    }
    return err;
}
