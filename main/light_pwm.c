#include "light_pwm.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "growbox_logic.h"
#include "sdkconfig.h"

static const char *TAG = "light_pwm";

#define PWM_MODE  LEDC_LOW_SPEED_MODE
#define PWM_TIMER LEDC_TIMER_0
#define PWM_MAX   1023u

static const int s_gpio[GB_FLOOR_COUNT] = {
    CONFIG_GROWBOX_F1_LIGHT_PWM_GPIO,
    CONFIG_GROWBOX_F2_LIGHT_PWM_GPIO,
    CONFIG_GROWBOX_F3_LIGHT_PWM_GPIO,
};

static bool s_ready[GB_FLOOR_COUNT];
static uint32_t s_duty[GB_FLOOR_COUNT];

esp_err_t light_pwm_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = PWM_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = PWM_TIMER,
        .freq_hz = CONFIG_GROWBOX_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_err_t result = ESP_OK;
    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        const ledc_channel_config_t channel = {
            .gpio_num = s_gpio[f],
            .speed_mode = PWM_MODE,
            .channel = (ledc_channel_t)(LEDC_CHANNEL_0 + f),
            .timer_sel = PWM_TIMER,
            .duty = 0,
            .hpoint = 0,
        };
        err = ledc_channel_config(&channel);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "floor %d: ledc_channel_config on GPIO%d failed: %s", f + 1, s_gpio[f], esp_err_to_name(err));
            result = err;
            continue;
        }
        s_ready[f] = true;
        s_duty[f] = 0;
    }
    return result;
}

void light_pwm_set(uint8_t floor, bool on, uint8_t level)
{
    if (floor >= GB_FLOOR_COUNT || !s_ready[floor]) {
        return;
    }
    uint32_t duty = on ? (uint32_t)level * PWM_MAX / 254u : 0;
    if (duty > PWM_MAX) {
        duty = PWM_MAX;
    }
    if (duty == s_duty[floor]) {
        return;
    }
    ledc_channel_t ch = (ledc_channel_t)(LEDC_CHANNEL_0 + floor);
    esp_err_t err = ledc_set_duty(PWM_MODE, ch, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(PWM_MODE, ch);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "floor %u: set duty %lu failed: %s", floor + 1, (unsigned long)duty, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "floor %u light %s, level %u (duty %lu/%u)", floor + 1, on ? "ON" : "OFF", level,
             (unsigned long)duty, PWM_MAX);
    s_duty[floor] = duty;
}
