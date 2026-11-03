/*
 * Optional room sensor next to the controller (outside the grow box): temperature + humidity, plus pressure for a
 * BME280. Shown in Zigbee2MQTT / Home Assistant like a plain Zigbee climate sensor (endpoint "room") and on the
 * display. Picked in menuconfig: Grow Box -> Room sensor.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sensors.h"

/* Called by sensors_init() once the I2C buses exist. */
esp_err_t room_sensor_init(void);

/* SENSOR_CAP_* of the configured sensor; 0 when there is none. */
uint8_t room_sensor_caps(void);

/* True when the room sensor is an I2C sensor at `addr` on `bus`. */
bool room_sensor_uses_i2c(sensors_bus_t bus, uint8_t addr);

/* One measurement; fields without a reading stay invalid. Logs one line. */
void room_sensor_read(floor_readings_t *out);
