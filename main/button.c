#include "button.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define POLL_US          (20 * 1000)
#define DEBOUNCE_SAMPLES 2
#define HINT_US          (3 * 1000000LL) /* hold longer than this: no short press, countdown shown */

#if CONFIG_GROWBOX_BUTTON_GPIO >= 0

static const char *TAG = "button";

#define RESET_US ((int64_t)CONFIG_GROWBOX_BUTTON_RESET_S * 1000000LL)

static button_cb_t s_short;
static button_cb_t s_long;
static bool s_raw;
static int s_same;
static bool s_down;
static bool s_long_fired;
static int64_t s_press_us;
static volatile uint8_t s_countdown;

static void poll_cb(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    bool raw = gpio_get_level(CONFIG_GROWBOX_BUTTON_GPIO) == 0;
    if (raw == s_raw) {
        if (s_same < DEBOUNCE_SAMPLES) {
            s_same++;
        }
    } else {
        s_raw = raw;
        s_same = 0;
    }

    if (s_same >= DEBOUNCE_SAMPLES && s_raw != s_down) {
        s_down = s_raw;
        if (s_down) {
            s_press_us = now;
            s_long_fired = false;
        } else if (!s_long_fired && now - s_press_us < HINT_US && s_short != NULL) {
            s_short();
        }
    }

    uint8_t countdown = 0;
    if (s_down && !s_long_fired) {
        int64_t held = now - s_press_us;
        if (held >= RESET_US) {
            s_long_fired = true;
            if (s_long != NULL) {
                s_long();
            }
        } else if (held >= HINT_US) {
            countdown = (uint8_t)((RESET_US - held + 999999) / 1000000);
        }
    }
    s_countdown = countdown;
}

void button_start(button_cb_t short_press, button_cb_t long_press)
{
    s_short = short_press;
    s_long = long_press;
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_GROWBOX_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d setup failed: %s", CONFIG_GROWBOX_BUTTON_GPIO, esp_err_to_name(err));
        return;
    }
    /* A button held at boot (e.g. BOOT while resetting) must be released first. */
    s_raw = s_down = gpio_get_level(CONFIG_GROWBOX_BUTTON_GPIO) == 0;
    s_long_fired = s_down;

    const esp_timer_create_args_t args = {.callback = poll_cb, .name = "button"};
    esp_timer_handle_t timer;
    err = esp_timer_create(&args, &timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(timer, POLL_US);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "poll timer failed: %s", esp_err_to_name(err));
    }
}

uint8_t button_reset_countdown_s(void)
{
    return s_countdown;
}

#else

void button_start(button_cb_t short_press, button_cb_t long_press)
{
    (void)short_press;
    (void)long_press;
}

uint8_t button_reset_countdown_s(void)
{
    return 0;
}

#endif
