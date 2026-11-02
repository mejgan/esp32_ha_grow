#include "dht11.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "growbox_logic.h"

static const char *TAG = "dht11";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static gpio_num_t s_configured_pin = GPIO_NUM_NC;

/* Busy-waits until `pin` reads `level`; returns elapsed microseconds or -1 on timeout. */
static int wait_level(gpio_num_t pin, int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(pin) != level) {
        if (esp_timer_get_time() - start > timeout_us) {
            return -1;
        }
    }
    return (int)(esp_timer_get_time() - start);
}

esp_err_t dht11_read(gpio_num_t pin, float *temp_c, float *rh)
{
    if (s_configured_pin != pin) {
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_INPUT_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gpio_config failed: %s", esp_err_to_name(err));
            return err;
        }
        gpio_set_level(pin, 1);
        s_configured_pin = pin;
    }

    uint8_t data[5] = {0};
    bool timeout = false;

    gpio_set_level(pin, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    portENTER_CRITICAL(&s_mux);
    gpio_set_level(pin, 1);
    if (wait_level(pin, 0, 100) < 0 || wait_level(pin, 1, 100) < 0 || wait_level(pin, 0, 100) < 0) {
        timeout = true;
    }
    for (int i = 0; i < 40 && !timeout; i++) {
        if (wait_level(pin, 1, 80) < 0) {
            timeout = true;
            break;
        }
        int high_us = wait_level(pin, 0, 100);
        if (high_us < 0) {
            timeout = true;
            break;
        }
        data[i / 8] <<= 1;
        if (high_us > 40) {
            data[i / 8] |= 1;
        }
    }
    portEXIT_CRITICAL(&s_mux);

    if (timeout) {
        ESP_LOGW(TAG, "timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (!gb_dht11_decode(data, temp_c, rh)) {
        ESP_LOGW(TAG, "bad frame %02x %02x %02x %02x %02x", data[0], data[1], data[2], data[3], data[4]);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}
