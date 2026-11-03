#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "growbox_logic.h"
#include "sensors.h"

/* Builds the Zigbee data model (endpoint N+1 = floor N) and spawns the "zb_main" task (router, joins via steering). */
esp_err_t zb_device_start(const gb_settings_t initial[GB_FLOOR_COUNT]);

/* Asks the stack whether the device is on a network. */
bool zb_device_is_joined(void);

/* Leaves the network and forgets it (back to pairing mode); restarts the device when not joined. */
void zb_device_factory_reset(void);

/* Reads LocalTime from the coordinator's Time cluster; the answer sets the system clock asynchronously. */
void zb_device_request_time(void);

/*
 * Current local minute of the day from the system clock, which holds the coordinator's local time. The clock
 * survives software resets (crash, watchdog, OTA), so auto mode keeps working without the network; false only
 * until the first sync after power-up.
 */
bool zb_device_get_local_minute(uint16_t *minute_of_day);

/* Writes valid readings of `floor` into its ZCL attributes (invalid ones are left untouched). */
void zb_device_publish_floor_sensors(uint8_t floor, const floor_readings_t *r);

/* Writes the room sensor's valid readings (endpoint 4; no-op without a room sensor). */
void zb_device_publish_room(const floor_readings_t *r);

/* Writes the floor's light output, settings and status flags into its ZCL attributes, only where changed. */
void zb_device_publish_floor_state(uint8_t floor, const gb_settings_t *s, gb_light_output_t out, bool heat_lock,
                                   bool sensor_fault);

void zb_device_publish_time_synced(bool synced);
