#pragma once

#include <stdint.h>

#include "growbox_ui.h"

typedef enum {
    APP_FIELD_ENABLED,
    APP_FIELD_LIGHT_ON,
    APP_FIELD_LIGHT_LEVEL,
    APP_FIELD_AUTO_MODE,
    APP_FIELD_ON_HOUR,
    APP_FIELD_OFF_HOUR,
    APP_FIELD_AUTO_BRIGHTNESS,
    APP_FIELD_MAX_TEMP,
    APP_FIELD_ON_MINUTE,
    APP_FIELD_OFF_MINUTE,
    APP_FIELD_TAKE_PHOTO, /* not a setting: asks the floor's camera for a photo now */
} app_field_t;

/* Applies a setting of `floor` (0-based) changed from Zigbee, schedules saving it and wakes the control loop.
 * Called from Zigbee context. */
void app_remote_set(uint8_t floor, app_field_t field, uint32_t value);

/* Writes pending setting changes to flash right away (before a restart). Safe from any task. */
void app_save_settings_now(void);

/* Snapshot of everything the display shows (reset countdown not included). Safe from any task. */
void app_get_ui_status(gb_ui_status_t *out);
