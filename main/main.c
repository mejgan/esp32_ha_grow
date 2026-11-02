#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app.h"
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

static const char *TAG = "growbox";

#define US_PER_S               1000000LL
#define TIME_RETRY_UNSYNCED_US (60 * US_PER_S)
#define TIME_RESYNC_US         (3600 * US_PER_S)

/* s_settings and s_out are guarded by s_lock. Never hold s_lock while taking the Zigbee lock. */
static gb_settings_t s_settings[GB_FLOOR_COUNT];
static gb_light_output_t s_out[GB_FLOOR_COUNT];
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
    case APP_FIELD_OFF_HOUR:
        s.off_hour = (uint8_t)clamp_u32(value, 0, 23);
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
        settings_save(s_settings);
        ESP_LOGI(TAG,
                 "floor %u: remote field %d = %lu -> enabled=%d auto=%d manual_on=%d level=%u hours %u-%u "
                 "brightness=%u%% max=%uC",
                 floor + 1, field, (unsigned long)value, s.enabled, s.auto_mode, s.manual_on, s.manual_level,
                 s.on_hour, s.off_hour, s.auto_brightness_pct, s.max_temp_c);
    }
    xSemaphoreGive(s_lock);

    if (changed) {
        xTaskNotifyGive(s_app_task);
    }
}

static void app_task(void *arg)
{
    (void)arg;
    floor_readings_t readings;
    bool temp_valid[GB_FLOOR_COUNT] = {false};
    float temp_c[GB_FLOOR_COUNT] = {0};
    bool was_enabled[GB_FLOOR_COUNT] = {false};
    bool heat_lock[GB_FLOOR_COUNT] = {false};
    bool first_read = true;
    int64_t last_read_us = 0;
    bool was_joined = false;
    bool time_requested = false;
    int64_t last_time_req_us = 0;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        bool joined = zb_device_is_joined();

        gb_settings_t s[GB_FLOOR_COUNT];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s, s_settings, sizeof(s));
        xSemaphoreGive(s_lock);

        /* Read at start, every period, right after joining (real values for the coordinator at once)
         * and when a floor gets enabled. Disabled floors are not read and don't count for the cutoff. */
        bool read_due = first_read || now - last_read_us >= CONFIG_GROWBOX_SENSOR_PERIOD_S * US_PER_S ||
                        (joined && !was_joined);
        for (int f = 0; f < GB_FLOOR_COUNT; f++) {
            read_due |= s[f].enabled && !was_enabled[f];
        }
        if (read_due) {
            for (int f = 0; f < GB_FLOOR_COUNT; f++) {
                temp_valid[f] = false;
                if (!s[f].enabled) {
                    continue;
                }
                sensors_read_floor(f, &readings);
                temp_valid[f] = readings.temp_valid;
                temp_c[f] = readings.temp_c;
                zb_device_publish_floor_sensors(f, &readings);
            }
            first_read = false;
            last_read_us = esp_timer_get_time();
        }
        if (!joined) {
            time_requested = false;
        }
        was_joined = joined;

        uint8_t hour = 0;
        bool synced = zb_device_get_local_hour(&hour);
        if (joined && (!time_requested ||
                       now - last_time_req_us >= (synced ? TIME_RESYNC_US : TIME_RETRY_UNSYNCED_US))) {
            zb_device_request_time();
            time_requested = true;
            last_time_req_us = now;
        }
        zb_device_publish_time_synced(synced);

        gb_light_output_t out[GB_FLOOR_COUNT];
        for (int f = 0; f < GB_FLOOR_COUNT; f++) {
            was_enabled[f] = s[f].enabled;
            bool locked = false;
            if (s[f].enabled) {
                float ctl = 0;
                bool ctl_valid = gb_floor_control_temp(temp_valid, temp_c, GB_FLOOR_COUNT, f, &ctl);
                locked = gb_heat_lock_update(heat_lock[f], ctl_valid, ctl, s[f].max_temp_c);
                if (locked != heat_lock[f]) {
                    ESP_LOGW(TAG, "floor %d: heat lock %s (%.1fC, max %uC)", f + 1, locked ? "ON" : "OFF", ctl,
                             s[f].max_temp_c);
                }
            }
            heat_lock[f] = locked;
            out[f] = gb_light_decide(&s[f], synced, hour, heat_lock[f]);
            light_pwm_set(f, out[f].on, out[f].level);
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_out, out, sizeof(s_out));
        xSemaphoreGive(s_lock);

        for (int f = 0; f < GB_FLOOR_COUNT; f++) {
            zb_device_publish_floor_state(f, &s[f], out[f], heat_lock[f]);
        }
    }
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

    light_pwm_init();
    err = sensors_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "some sensors failed to initialize (%s); continuing", esp_err_to_name(err));
    }

    BaseType_t ok = xTaskCreate(app_task, "growbox", 6144, NULL, 4, &s_app_task);
    configASSERT(ok == pdPASS);

    ESP_ERROR_CHECK(zb_device_start(s_settings));
}
