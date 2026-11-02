#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "growbox_logic.h"
#include "sensors.h"

/* Builds the Zigbee data model (endpoint N+1 = floor N) and spawns the "zb_main" task (router, joins via steering). */
esp_err_t zb_device_start(const gb_settings_t initial[GB_FLOOR_COUNT]);

bool zb_device_is_joined(void);

/* Reads LocalTime from the coordinator's Time cluster; the answer arrives asynchronously. */
void zb_device_request_time(void);

/* Current local hour from the last coordinator time sync; false until the first successful sync. */
bool zb_device_get_local_hour(uint8_t *hour);

/* Writes valid readings of `floor` into its ZCL attributes (invalid ones are left untouched). */
void zb_device_publish_floor_sensors(uint8_t floor, const floor_readings_t *r);

/* Writes the floor's light output and settings into its ZCL attributes, only where changed. */
void zb_device_publish_floor_state(uint8_t floor, const gb_settings_t *s, gb_light_output_t out, bool heat_lock);

void zb_device_publish_time_synced(bool synced);
