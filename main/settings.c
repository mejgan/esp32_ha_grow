#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "settings";

#define SETTINGS_NS      "growbox"
#define SETTINGS_KEY     "cfg"
#define SETTINGS_VERSION 3

typedef struct {
    uint8_t version;
    gb_settings_t floors[GB_FLOOR_COUNT];
} settings_blob_t;

/* Version 2 layout (whole hours only), migrated on load. */
typedef struct {
    uint8_t enabled;
    uint8_t auto_mode;
    uint8_t manual_on;
    uint8_t manual_level;
    uint8_t on_hour;
    uint8_t off_hour;
    uint8_t auto_brightness_pct;
    uint8_t max_temp_c;
} settings_v2_floor_t;

typedef struct {
    uint8_t version;
    settings_v2_floor_t floors[GB_FLOOR_COUNT];
} settings_v2_blob_t;

static void defaults(gb_settings_t *s)
{
    *s = (gb_settings_t){
        .enabled = true,
        .auto_mode = true,
        .manual_on = false,
        .manual_level = 254,
        .on_hour = CONFIG_GROWBOX_DEFAULT_ON_HOUR,
        .on_minute = 0,
        .off_hour = CONFIG_GROWBOX_DEFAULT_OFF_HOUR,
        .off_minute = 0,
        .auto_brightness_pct = CONFIG_GROWBOX_DEFAULT_BRIGHTNESS,
        .max_temp_c = CONFIG_GROWBOX_DEFAULT_MAX_TEMP,
    };
}

static bool load_v3(const uint8_t *raw, size_t len, gb_settings_t out[GB_FLOOR_COUNT])
{
    settings_blob_t blob;
    if (len != sizeof(blob)) {
        return false;
    }
    memcpy(&blob, raw, sizeof(blob));
    if (blob.version != SETTINGS_VERSION) {
        return false;
    }
    memcpy(out, blob.floors, sizeof(blob.floors));
    return true;
}

static bool load_v2(const uint8_t *raw, size_t len, gb_settings_t out[GB_FLOOR_COUNT])
{
    settings_v2_blob_t blob;
    if (len != sizeof(blob)) {
        return false;
    }
    memcpy(&blob, raw, sizeof(blob));
    if (blob.version != 2) {
        return false;
    }
    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        const settings_v2_floor_t *o = &blob.floors[f];
        out[f] = (gb_settings_t){
            .enabled = o->enabled != 0,
            .auto_mode = o->auto_mode != 0,
            .manual_on = o->manual_on != 0,
            .manual_level = o->manual_level,
            .on_hour = o->on_hour,
            .on_minute = 0,
            .off_hour = o->off_hour,
            .off_minute = 0,
            .auto_brightness_pct = o->auto_brightness_pct,
            .max_temp_c = o->max_temp_c,
        };
    }
    ESP_LOGI(TAG, "migrated settings from version 2");
    return true;
}

void settings_load(gb_settings_t out[GB_FLOOR_COUNT])
{
    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        defaults(&out[f]);
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings (%s), using defaults", esp_err_to_name(err));
        return;
    }
    uint8_t raw[sizeof(settings_blob_t) > sizeof(settings_v2_blob_t) ? sizeof(settings_blob_t)
                                                                      : sizeof(settings_v2_blob_t)];
    size_t len = sizeof(raw);
    err = nvs_get_blob(h, SETTINGS_KEY, raw, &len);
    nvs_close(h);

    gb_settings_t loaded[GB_FLOOR_COUNT];
    if (err != ESP_OK || !(load_v3(raw, len, loaded) || load_v2(raw, len, loaded))) {
        ESP_LOGI(TAG, "saved settings unusable (%s, len %u), using defaults", esp_err_to_name(err), (unsigned)len);
        return;
    }
    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        out[f] = loaded[f];
        gb_settings_clamp(&out[f]);
        ESP_LOGI(TAG,
                 "floor %d: enabled=%d auto=%d manual_on=%d level=%u schedule %02u:%02u-%02u:%02u brightness=%u%% "
                 "max=%uC",
                 f + 1, out[f].enabled, out[f].auto_mode, out[f].manual_on, out[f].manual_level, out[f].on_hour,
                 out[f].on_minute, out[f].off_hour, out[f].off_minute, out[f].auto_brightness_pct, out[f].max_temp_c);
    }
}

esp_err_t settings_save(const gb_settings_t floors[GB_FLOOR_COUNT])
{
    settings_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = SETTINGS_VERSION;
    memcpy(blob.floors, floors, sizeof(blob.floors));

    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, SETTINGS_KEY, &blob, sizeof(blob));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "settings saved");
    }
    return err;
}
