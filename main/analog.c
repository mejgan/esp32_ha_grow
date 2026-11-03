#include "analog.h"

#include <stdbool.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "analog";

#define ANALOG_SAMPLES   16
#define ANALOG_MAX_GPIO  6 /* ADC1 channels are GPIO0-6 on the ESP32-C6 */

static adc_oneshot_unit_handle_t s_unit;
static adc_channel_t s_chan[ANALOG_MAX_GPIO + 1];
static adc_cali_handle_t s_cali[ANALOG_MAX_GPIO + 1];

esp_err_t analog_init(void)
{
    if (s_unit != NULL) {
        return ESP_OK;
    }
    const adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_unit);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adc unit init failed: %s", esp_err_to_name(err));
        s_unit = NULL;
    }
    return err;
}

static esp_err_t channel_setup(int gpio)
{
    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(gpio, &unit, &s_chan[gpio]);
    if (err == ESP_OK && unit != ADC_UNIT_1) {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) {
        const adc_oneshot_chan_cfg_t chan_cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        err = adc_oneshot_config_channel(s_unit, s_chan[gpio], &chan_cfg);
    }
    if (err == ESP_OK) {
        const adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1,
            .chan = s_chan[gpio],
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali[gpio]);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d setup failed: %s", gpio, esp_err_to_name(err));
        s_cali[gpio] = NULL;
    }
    return err;
}

esp_err_t analog_read_mv(int gpio, int *mv)
{
    if (gpio < 0 || gpio > ANALOG_MAX_GPIO || s_unit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cali[gpio] == NULL) {
        esp_err_t err = channel_setup(gpio);
        if (err != ESP_OK) {
            return err;
        }
    }
    int sum = 0;
    for (int i = 0; i < ANALOG_SAMPLES; i++) {
        int sample;
        esp_err_t err = adc_oneshot_get_calibrated_result(s_unit, s_cali[gpio], s_chan[gpio], &sample);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GPIO%d read failed: %s", gpio, esp_err_to_name(err));
            return err;
        }
        sum += sample;
    }
    *mv = sum / ANALOG_SAMPLES;
    return ESP_OK;
}
