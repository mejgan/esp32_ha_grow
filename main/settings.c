#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "settings";

#define SETTINGS_NS      "growbox"
#define SETTINGS_KEY     "cfg"
#define SETTINGS_VERSION 2

typedef struct {
    uint8_t version;
    gb_settings_t floors[GB_FLOOR_COUNT];
} settings_blob_t;

static uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void defaults(gb_settings_t *s)
{
    *s = (gb_settings_t){
        .enabled = true,
        .auto_mode = true,
        .manual_on = false,
        .manual_level = 254,
        .on_hour = CONFIG_GROWBOX_DEFAULT_ON_HOUR,
        .off_hour = CONFIG_GROWBOX_DEFAULT_OFF_HOUR,
        .auto_brightness_pct = CONFIG_GROWBOX_DEFAULT_BRIGHTNESS,
        .max_temp_c = CONFIG_GROWBOX_DEFAULT_MAX_TEMP,
    };
}

static void clamp(gb_settings_t *s)
{
    s->enabled = s->enabled != 0;
    s->auto_mode = s->auto_mode != 0;
    s->manual_on = s->manual_on != 0;
    s->manual_level = clamp_u8(s->manual_level, 1, 254);
    s->on_hour = clamp_u8(s->on_hour, 0, 23);
    s->off_hour = clamp_u8(s->off_hour, 0, 23);
    s->auto_brightness_pct = clamp_u8(s->auto_brightness_pct, 1, 100);
    s->max_temp_c = clamp_u8(s->max_temp_c, 20, 50);
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
    settings_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, SETTINGS_KEY, &blob, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(blob) || blob.version != SETTINGS_VERSION) {
        ESP_LOGI(TAG, "saved settings unusable (%s, len %u), using defaults", esp_err_to_name(err), (unsigned)len);
        return;
    }
    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        out[f] = blob.floors[f];
        clamp(&out[f]);
        ESP_LOGI(TAG, "floor %d: enabled=%d auto=%d manual_on=%d level=%u hours %u-%u brightness=%u%% max=%uC", f + 1,
                 out[f].enabled, out[f].auto_mode, out[f].manual_on, out[f].manual_level, out[f].on_hour,
                 out[f].off_hour, out[f].auto_brightness_pct, out[f].max_temp_c);
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
    }
    return err;
}
