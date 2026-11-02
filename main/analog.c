#include "analog.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "analog";

#define ANALOG_SAMPLES 16

static const int s_gpio[ANALOG_COUNT] = {
    [ANALOG_SOIL_CAP] = CONFIG_GROWBOX_F1_SOIL_CAP_GPIO,
    [ANALOG_SOIL_RES] = CONFIG_GROWBOX_F2_SOIL_RES_GPIO,
    [ANALOG_LIGHT] = CONFIG_GROWBOX_F3_LIGHT_SENSOR_GPIO,
};

static adc_oneshot_unit_handle_t s_unit;
static adc_channel_t s_chan[ANALOG_COUNT];
static adc_cali_handle_t s_cali[ANALOG_COUNT];

esp_err_t analog_init(void)
{
    const adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_unit);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adc unit init failed: %s", esp_err_to_name(err));
        s_unit = NULL;
        return err;
    }
    esp_err_t result = ESP_OK;
    for (int i = 0; i < ANALOG_COUNT; i++) {
        adc_unit_t unit;
        err = adc_oneshot_io_to_channel(s_gpio[i], &unit, &s_chan[i]);
        if (err == ESP_OK && unit != ADC_UNIT_1) {
            err = ESP_ERR_INVALID_ARG;
        }
        if (err == ESP_OK) {
            const adc_oneshot_chan_cfg_t chan_cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
            err = adc_oneshot_config_channel(s_unit, s_chan[i], &chan_cfg);
        }
        if (err == ESP_OK) {
            const adc_cali_curve_fitting_config_t cali_cfg = {
                .unit_id = ADC_UNIT_1,
                .chan = s_chan[i],
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali[i]);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GPIO%d setup failed: %s", s_gpio[i], esp_err_to_name(err));
            s_cali[i] = NULL;
            result = err;
        }
    }
    return result;
}

esp_err_t analog_read_mv(analog_ch_t ch, int *mv)
{
    if (ch >= ANALOG_COUNT || s_unit == NULL || s_cali[ch] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int sum = 0;
    for (int i = 0; i < ANALOG_SAMPLES; i++) {
        int sample;
        esp_err_t err = adc_oneshot_get_calibrated_result(s_unit, s_cali[ch], s_chan[ch], &sample);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GPIO%d read failed: %s", s_gpio[ch], esp_err_to_name(err));
            return err;
        }
        sum += sample;
    }
    *mv = sum / ANALOG_SAMPLES;
    return ESP_OK;
}
