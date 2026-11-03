#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app.h"
#include "button.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "growbox_logic.h"
#include "light_pwm.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sensors.h"
#include "settings.h"
#include "zb_device.h"
#include "zb_ota.h"

static const char *TAG = "growbox";

#define US_PER_S               1000000LL
#define TIME_RETRY_UNSYNCED_US (60 * US_PER_S)
#define TIME_RESYNC_US         (3600 * US_PER_S)
/* Settings are written to flash this long after the last remote change (a dimmer drag sends many). */
#define SAVE_DELAY_US          (5 * US_PER_S)
/* Without any valid temperature for this long the light of an enabled floor is forced off (0 disables). */
#define FAILSAFE_US            ((int64_t)CONFIG_GROWBOX_TEMP_FAILSAFE_MIN * 60 * US_PER_S)
/* A new firmware image is confirmed (no rollback) once joined, or after running this long without a network. */
#define OTA_CONFIRM_UPTIME_US  (10 * 60 * US_PER_S)

/* Everything below is guarded by s_lock. Never hold s_lock while taking the Zigbee lock. */
static gb_settings_t s_settings[GB_FLOOR_COUNT];
static gb_light_output_t s_out[GB_FLOOR_COUNT];
static gb_ui_status_t s_ui;
static bool s_heat_lock[GB_FLOOR_COUNT];
static bool s_save_pending;
static int64_t s_save_due_us;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_app_task;

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void app_remote_set(uint8_t floor, app_field_t field, uint32_t value)
{
    if (floor >= GB_FLOOR_COUNT) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    gb_settings_t s = s_settings[floor];

    /* ON can't take effect while the light is forced off, so it is refused rather than remembered for later:
     * the light then shows OFF in Zigbee2MQTT, and nothing surprising happens once the lock clears. */
    if (field == APP_FIELD_LIGHT_ON && value != 0 && (!s.enabled || s_heat_lock[floor])) {
        bool heat = s_heat_lock[floor];
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "floor %u: ON refused (%s)", floor + 1, heat ? "heat lock" : "floor disabled");
        xTaskNotifyGive(s_app_task); /* republish the real state right away */
        return;
    }

    /* Leaving auto mode keeps the light exactly as the schedule had it; the command then adjusts that state. */
    bool leaves_auto = s.auto_mode && (field == APP_FIELD_LIGHT_ON || field == APP_FIELD_LIGHT_LEVEL ||
                                       (field == APP_FIELD_AUTO_MODE && value == 0));
    if (leaves_auto) {
        s.manual_on = s_out[floor].on;
        s.manual_level = s_out[floor].level;
    }

    switch (field) {
    case APP_FIELD_ENABLED:
        s.enabled = value != 0;
        break;
    case APP_FIELD_LIGHT_ON:
        s.auto_mode = false;
        s.manual_on = value != 0;
        break;
    case APP_FIELD_LIGHT_LEVEL:
        s.auto_mode = false;
        s.manual_level = (uint8_t)clamp_u32(value, 1, 254);
        break;
    case APP_FIELD_AUTO_MODE:
        s.auto_mode = value != 0;
        break;
    case APP_FIELD_ON_HOUR:
        s.on_hour = (uint8_t)clamp_u32(value, 0, 23);
        break;
    case APP_FIELD_ON_MINUTE:
        s.on_minute = (uint8_t)clamp_u32(value, 0, 59);
        break;
    case APP_FIELD_OFF_HOUR:
        s.off_hour = (uint8_t)clamp_u32(value, 0, 23);
        break;
    case APP_FIELD_OFF_MINUTE:
        s.off_minute = (uint8_t)clamp_u32(value, 0, 59);
        break;
    case APP_FIELD_AUTO_BRIGHTNESS:
        s.auto_brightness_pct = (uint8_t)clamp_u32(value, 1, 100);
        break;
    case APP_FIELD_MAX_TEMP:
        s.max_temp_c = (uint8_t)clamp_u32(value, 20, 50);
        break;
    }

    bool changed = memcmp(&s, &s_settings[floor], sizeof(s)) != 0;
    if (changed) {
        s_settings[floor] = s;
        s_save_pending = true;
        s_save_due_us = esp_timer_get_time() + SAVE_DELAY_US;
        ESP_LOGI(TAG,
                 "floor %u: remote field %d = %lu -> enabled=%d auto=%d manual_on=%d level=%u schedule "
                 "%02u:%02u-%02u:%02u brightness=%u%% max=%uC",
                 floor + 1, field, (unsigned long)value, s.enabled, s.auto_mode, s.manual_on, s.manual_level,
                 s.on_hour, s.on_minute, s.off_hour, s.off_minute, s.auto_brightness_pct, s.max_temp_c);
    }
    xSemaphoreGive(s_lock);

    if (changed) {
        xTaskNotifyGive(s_app_task);
    }
}

void app_get_ui_status(gb_ui_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_ui;
    xSemaphoreGive(s_lock);
}

/* Saves the settings if a remote change is older than SAVE_DELAY_US. Flash is written outside the lock. */
static void save_if_due(int64_t now)
{
    gb_settings_t copy[GB_FLOOR_COUNT];
    bool due = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_save_pending && now >= s_save_due_us) {
        memcpy(copy, s_settings, sizeof(copy));
        s_save_pending = false;
        due = true;
    }
    xSemaphoreGive(s_lock);
    if (due) {
        settings_save(copy);
    }
}

void app_save_settings_now(void)
{
    save_if_due(INT64_MAX);
}

static void fill_ui_floor(gb_ui_floor_t *u, uint8_t floor, const gb_settings_t *s, gb_light_output_t out,
                          bool heat_lock, bool fault, const floor_readings_t *r)
{
    uint8_t caps = sensors_floor_caps(floor);
    *u = (gb_ui_floor_t){
        .enabled = s->enabled,
        .light_on = out.on,
        .level_pct = gb_level_to_pct(out.level),
        .auto_mode = s->auto_mode,
        .on_min = gb_on_minute_of_day(s),
        .off_min = gb_off_minute_of_day(s),
        .auto_brightness_pct = s->auto_brightness_pct,
        .max_temp_c = s->max_temp_c,
        .heat_lock = heat_lock,
        .sensor_fault = fault,
        .has_humidity = (caps & SENSOR_CAP_HUMIDITY) != 0,
        .has_soil = (caps & SENSOR_CAP_SOIL) != 0,
        .has_pressure = (caps & SENSOR_CAP_PRESSURE) != 0,
        .has_lux = (caps & SENSOR_CAP_LUX) != 0,
    };
    if (r != NULL) {
        u->temp_valid = r->temp_valid;
        u->temp_c = r->temp_c;
        u->rh_valid = r->rh_valid;
        u->rh = r->rh;
        u->soil_valid = r->soil_valid;
        u->soil_pct = r->soil_pct_x100 / 100.0f;
        u->pressure_valid = r->pressure_valid;
        u->pressure_hpa = r->pressure_hpa;
        u->lux_valid = r->lux_valid;
        u->lux = r->lux;
    }
}

static void app_task(void *arg)
{
    (void)arg;
    static floor_readings_t readings[GB_FLOOR_COUNT];
    bool have_reading[GB_FLOOR_COUNT] = {false};
    bool temp_valid[GB_FLOOR_COUNT] = {false};
    float temp_c[GB_FLOOR_COUNT] = {0};
    bool fault[GB_FLOOR_COUNT] = {false};
    bool was_enabled[GB_FLOOR_COUNT] = {false};
    bool heat_lock[GB_FLOOR_COUNT] = {false};
    int64_t ctl_valid_us[GB_FLOOR_COUNT];
    bool first_read = true;
    int64_t last_read_us = 0;
    bool was_joined = false;
    bool time_requested = false;
    int64_t last_time_req_us = 0;
    bool image_confirmed = false;

    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        ctl_valid_us[f] = esp_timer_get_time();
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        bool joined = zb_device_is_joined();

        if (!image_confirmed && (joined || now >= OTA_CONFIRM_UPTIME_US)) {
            zb_ota_confirm_running_image();
            image_confirmed = true;
        }

        bool enabled[GB_FLOOR_COUNT];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int f = 0; f < GB_FLOOR_COUNT; f++) {
            enabled[f] = s_settings[f].enabled;
        }
        xSemaphoreGive(s_lock);

        /* Read at start, every period, right after joining (real values for the coordinator at once)
         * and when a floor gets enabled. Disabled floors are not read and don't count for the cutoff. */
        bool read_due = first_read || now - last_read_us >= CONFIG_GROWBOX_SENSOR_PERIOD_S * US_PER_S ||
                        (joined && !was_joined);
        for (int f = 0; f < GB_FLOOR_COUNT; f++) {
            read_due |= enabled[f] && !was_enabled[f];
            was_enabled[f] = enabled[f];
        }
        if (read_due) {
            for (int f = 0; f < GB_FLOOR_COUNT; f++) {
                temp_valid[f] = false;
                fault[f] = false;
                have_reading[f] = false;
                if (!enabled[f]) {
                    continue;
                }
                sensors_read_floor(f, &readings[f]);
                have_reading[f] = true;
                temp_valid[f] = readings[f].temp_valid;
                temp_c[f] = readings[f].temp_c;
                fault[f] = sensors_floor_fault(f, &readings[f]);
                zb_device_publish_floor_sensors(f, &readings[f]);
            }
            first_read = false;
            last_read_us = esp_timer_get_time();
        }
        if (!joined) {
            time_requested = false;
        }
        was_joined = joined;

        uint16_t minute = 0;
        bool time_valid = zb_device_get_local_minute(&minute);
        now = esp_timer_get_time();
        if (joined && (!time_requested ||
                       now - last_time_req_us >= (time_valid ? TIME_RESYNC_US : TIME_RETRY_UNSYNCED_US))) {
            zb_device_request_time();
            time_requested = true;
            last_time_req_us = now;
        }
        zb_device_publish_time_synced(time_valid);

        /* Settings snapshot after the (slow) sensor reads, so a command that arrived meanwhile is applied now. */
        gb_settings_t s[GB_FLOOR_COUNT];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s, s_settings, sizeof(s));
        xSemaphoreGive(s_lock);

        gb_light_output_t out[GB_FLOOR_COUNT];
        for (int f = 0; f < GB_FLOOR_COUNT; f++) {
            bool locked = false;
            if (!s[f].enabled) {
                ctl_valid_us[f] = now; /* a floor that gets enabled starts with a full failsafe grace period */
            } else {
                float ctl = 0;
                bool ctl_valid = gb_floor_control_temp(temp_valid, temp_c, GB_FLOOR_COUNT, f, &ctl);
                if (ctl_valid) {
                    ctl_valid_us[f] = now;
                }
                locked = gb_heat_lock_update(heat_lock[f], ctl_valid, ctl, s[f].max_temp_c);
                if (FAILSAFE_US > 0 && !ctl_valid && now - ctl_valid_us[f] > FAILSAFE_US) {
                    locked = true;
                }
                if (locked != heat_lock[f]) {
                    if (ctl_valid) {
                        ESP_LOGW(TAG, "floor %d: heat lock %s (%.1fC, max %uC)", f + 1, locked ? "ON" : "OFF", ctl,
                                 s[f].max_temp_c);
                    } else {
                        ESP_LOGW(TAG, "floor %d: heat lock %s (no valid temperature)", f + 1, locked ? "ON" : "OFF");
                    }
                }
            }
            heat_lock[f] = locked;
            out[f] = gb_light_decide(&s[f], time_valid, minute, locked);
            light_pwm_set(f, out[f].on, out[f].level);
        }

        gb_ui_status_t ui = {
            .joined = joined,
            .time_valid = time_valid,
            .minute_of_day = minute,
        };
        for (int f = 0; f < GB_FLOOR_COUNT; f++) {
            fill_ui_floor(&ui.floor[f], f, &s[f], out[f], heat_lock[f], fault[f],
                          have_reading[f] ? &readings[f] : NULL);
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_out, out, sizeof(s_out));
        memcpy(s_heat_lock, heat_lock, sizeof(s_heat_lock));
        s_ui = ui;
        xSemaphoreGive(s_lock);

        for (int f = 0; f < GB_FLOOR_COUNT; f++) {
            zb_device_publish_floor_state(f, &s[f], out[f], heat_lock[f], fault[f]);
        }

        save_if_due(esp_timer_get_time());
    }
}

static void on_button_short_press(void)
{
    display_next_page();
}

static void on_button_long_press(void)
{
    ESP_LOGW(TAG, "button held: Zigbee factory reset");
    app_save_settings_now(); /* the reset may restart the chip */
    zb_device_factory_reset();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock);
    settings_load(s_settings);
    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        /* The display shows the settings right away, readings follow with the first loop. */
        fill_ui_floor(&s_ui.floor[f], f, &s_settings[f], (gb_light_output_t){.on = false, .level = 1}, false, false,
                      NULL);
    }

    light_pwm_init();
    err = sensors_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "some sensors failed to initialize (%s); continuing", esp_err_to_name(err));
    }

    BaseType_t ok = xTaskCreate(app_task, "growbox", 6144, NULL, 4, &s_app_task);
    configASSERT(ok == pdPASS);

    display_start();
    button_start(on_button_short_press, on_button_long_press);

    ESP_ERROR_CHECK(zb_device_start(s_settings));
}
