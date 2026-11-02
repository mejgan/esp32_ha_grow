#pragma once

#include <stdint.h>

typedef enum {
    APP_FIELD_ENABLED,
    APP_FIELD_LIGHT_ON,
    APP_FIELD_LIGHT_LEVEL,
    APP_FIELD_AUTO_MODE,
    APP_FIELD_ON_HOUR,
    APP_FIELD_OFF_HOUR,
    APP_FIELD_AUTO_BRIGHTNESS,
    APP_FIELD_MAX_TEMP,
} app_field_t;

/* Applies a setting of `floor` (0-based) changed from Zigbee, persists it and wakes the control loop.
 * Called from Zigbee context. */
void app_remote_set(uint8_t floor, app_field_t field, uint32_t value);
