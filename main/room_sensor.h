/*
 * Optional room sensor next to the controller (outside the grow box): temperature + humidity, plus pressure for a
 * BME280. Shown in Zigbee2MQTT / Home Assistant like a plain Zigbee climate sensor (endpoint "room") and on the
 * display. Picked in menuconfig: Grow Box -> Room sensor.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "sensors.h"

/* Call after sensors_init() (I2C types share its buses). */
esp_err_t room_sensor_init(void);

/* SENSOR_CAP_HUMIDITY / SENSOR_CAP_PRESSURE of the configured sensor; 0 when there is none. */
uint8_t room_sensor_caps(void);

/* One measurement; fields without a reading stay invalid. Logs one line. */
void room_sensor_read(floor_readings_t *out);
